#include "emotion_fusion_analyzer.h"

#include "embedding_pipeline.h"
#include "llm_client.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <utility>

namespace agent::service::persona {
namespace {

const std::set<std::string>& EmotionLabelSet() {
    static const std::set<std::string> labels{
        "joy",
        "sadness",
        "anger",
        "fear",
        "surprise",
        "disgust",
        "neutral",
        "excitement",
        "tenderness",
        "curiosity",
    };
    return labels;
}

std::string NormalizeEmotionLabel(std::string label) {
    if (EmotionLabelSet().contains(label)) {
        return label;
    }
    if (label == "frustration" || label == "annoyance" || label == "irritation") {
        return "anger";
    }
    if (label == "anxiety" || label == "nervousness" || label == "worry") {
        return "fear";
    }
    if (label == "disappointment" || label == "grief") {
        return "sadness";
    }
    if (label == "happiness" || label == "amusement") {
        return "joy";
    }
    if (label == "confusion" || label == "interest") {
        return "curiosity";
    }
    if (label == "gratitude" || label == "love" || label == "caring") {
        return "tenderness";
    }
    return "neutral";
}

double Clamp(double value, double lo, double hi) {
    return std::max(lo, std::min(value, hi));
}

double MapGet(const std::map<std::string, double>& values, std::string_view key, double fallback) {
    auto it = values.find(std::string(key));
    return it == values.end() ? fallback : it->second;
}

std::pair<double, double> TopTwoMargin(const std::map<std::string, double>& probabilities) {
    double first = 0.0;
    double second = 0.0;
    for (const auto& [_, value] : probabilities) {
        if (value > first) {
            second = first;
            first = value;
        } else if (value > second) {
            second = value;
        }
    }
    return {first, first - second};
}

} // namespace

core::Result<std::vector<std::vector<EmotionEvidence>>> IEmotionEvidenceProvider::CollectBatch(
    std::span<const std::string_view> texts,
    std::string_view trace_id) const {
    std::vector<std::vector<EmotionEvidence>> batches;
    batches.reserve(texts.size());
    for (auto text : texts) {
        auto evidence = Collect(text, trace_id);
        if (!evidence.ok()) {
            return evidence.status();
        }
        batches.push_back(std::move(evidence).value());
    }
    return batches;
}

KeywordEmotionEvidenceProvider::KeywordEmotionEvidenceProvider(std::vector<EmotionKeywordRule> rules) {
    rules_.reserve(rules.size());
    for (const auto& rule : rules) {
        if (rule.label.empty() || rule.pattern.empty()) {
            continue;
        }
        try {
            rules_.push_back(CompiledRule{
                rule.label,
                rule.pattern,
                Clamp(rule.score, 0.0, 1.0),
                std::regex(rule.pattern),
            });
        } catch (const std::regex_error&) {
            continue;
        }
    }
}

core::Result<std::vector<EmotionEvidence>> KeywordEmotionEvidenceProvider::Collect(std::string_view text,
                                                                                   std::string_view) const {
    std::vector<EmotionEvidence> evidence;
    const std::string value(text);
    for (const auto& rule : rules_) {
        std::smatch match;
        if (std::regex_search(value, match, rule.regex)) {
            evidence.push_back(EmotionEvidence{
                rule.label,
                rule.score,
                "keyword",
                match.empty() ? rule.pattern : match.str(),
            });
        }
    }
    return evidence;
}

core::Result<std::shared_ptr<VectorEmotionEvidenceProvider>> VectorEmotionEvidenceProvider::Create(
    std::shared_ptr<::vector::EmbeddingPipeline> embedding,
    std::vector<EmotionVectorPrototype> prototypes,
    EmotionVectorEvidenceOptions options) {
    if (!embedding) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "emotion vector embedding pipeline is required");
    }
    std::vector<EmotionVectorPrototype> valid_prototypes;
    valid_prototypes.reserve(prototypes.size());
    std::vector<std::string_view> prototype_texts;
    prototype_texts.reserve(prototypes.size());
    for (auto& prototype : prototypes) {
        if (prototype.label.empty() || prototype.text.empty()) {
            continue;
        }
        valid_prototypes.push_back(std::move(prototype));
        prototype_texts.push_back(valid_prototypes.back().text);
    }

    std::vector<EmbeddedPrototype> embedded;
    embedded.reserve(valid_prototypes.size());
    if (valid_prototypes.empty()) {
        return std::shared_ptr<VectorEmotionEvidenceProvider>(
            new VectorEmotionEvidenceProvider(std::move(embedding), std::move(embedded), options));
    }

    auto vectors = embedding->EncodeBatch(prototype_texts);
    if (!vectors.ok()) {
        return vectors.status();
    }
    if (vectors.value().batch_size != valid_prototypes.size()) {
        return core::Status::Error(core::ErrorCode::InternalError, "emotion vector prototype batch size mismatch");
    }
    for (std::size_t row = 0; row < valid_prototypes.size(); ++row) {
        auto vector = vectors.value().row(row);
        embedded.push_back(EmbeddedPrototype{
            std::move(valid_prototypes[row]),
            std::vector<float>(vector.begin(), vector.end()),
        });
    }
    return std::shared_ptr<VectorEmotionEvidenceProvider>(
        new VectorEmotionEvidenceProvider(std::move(embedding), std::move(embedded), options));
}

VectorEmotionEvidenceProvider::VectorEmotionEvidenceProvider(
    std::shared_ptr<::vector::EmbeddingPipeline> embedding,
    std::vector<EmbeddedPrototype> prototypes,
    EmotionVectorEvidenceOptions options)
    : embedding_(std::move(embedding)),
      prototypes_(std::move(prototypes)),
      options_(options) {}

core::Result<std::vector<EmotionEvidence>> VectorEmotionEvidenceProvider::Collect(std::string_view text,
                                                                                  std::string_view) const {
    if (!embedding_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "emotion vector embedding pipeline is required");
    }
    if (prototypes_.empty()) {
        return std::vector<EmotionEvidence>{};
    }
    auto query = embedding_->Encode(text);
    if (!query.ok()) {
        return query.status();
    }

    return CollectFromEmbedding(query.value());
}

core::Result<std::vector<std::vector<EmotionEvidence>>> VectorEmotionEvidenceProvider::CollectBatch(
    std::span<const std::string_view> texts,
    std::string_view) const {
    if (!embedding_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "emotion vector embedding pipeline is required");
    }
    std::vector<std::vector<EmotionEvidence>> batches;
    batches.reserve(texts.size());
    if (texts.empty()) {
        return batches;
    }
    if (prototypes_.empty()) {
        batches.resize(texts.size());
        return batches;
    }
    auto queries = embedding_->EncodeBatch(texts);
    if (!queries.ok()) {
        return queries.status();
    }
    if (queries.value().batch_size != texts.size()) {
        return core::Status::Error(core::ErrorCode::InternalError, "emotion vector embedding batch size mismatch");
    }
    for (std::size_t row = 0; row < queries.value().batch_size; ++row) {
        batches.push_back(CollectFromEmbedding(queries.value().row(row)));
    }
    return batches;
}

std::vector<EmotionEvidence> VectorEmotionEvidenceProvider::CollectFromEmbedding(std::span<const float> query) const {
    std::vector<EmotionEvidence> evidence;
    for (const auto& prototype : prototypes_) {
        const double similarity = CosineSimilarity(query, prototype.embedding);
        if (similarity < options_.similarity_threshold) {
            continue;
        }
        evidence.push_back(EmotionEvidence{
            prototype.prototype.label,
            Clamp(similarity * prototype.prototype.score, 0.0, 1.0),
            "vector",
            prototype.prototype.text,
        });
    }
    std::sort(evidence.begin(), evidence.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.score > rhs.score;
    });
    if (options_.max_evidence > 0 && evidence.size() > options_.max_evidence) {
        evidence.resize(options_.max_evidence);
    }
    return evidence;
}

double VectorEmotionEvidenceProvider::CosineSimilarity(const std::vector<float>& lhs, const std::vector<float>& rhs) {
    return CosineSimilarity(std::span<const float>(lhs.data(), lhs.size()), rhs);
}

double VectorEmotionEvidenceProvider::CosineSimilarity(std::span<const float> lhs, const std::vector<float>& rhs) {
    if (lhs.empty() || lhs.size() != rhs.size()) {
        return 0.0;
    }
    double dot = 0.0;
    double lhs_norm = 0.0;
    double rhs_norm = 0.0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        dot += static_cast<double>(lhs[i]) * static_cast<double>(rhs[i]);
        lhs_norm += static_cast<double>(lhs[i]) * static_cast<double>(lhs[i]);
        rhs_norm += static_cast<double>(rhs[i]) * static_cast<double>(rhs[i]);
    }
    if (lhs_norm <= 0.0 || rhs_norm <= 0.0) {
        return 0.0;
    }
    return Clamp(dot / (std::sqrt(lhs_norm) * std::sqrt(rhs_norm)), -1.0, 1.0);
}

LlmEmotionFallbackAnalyzer::LlmEmotionFallbackAnalyzer(std::shared_ptr<agent::llm::ILlmClient> llm,
                                                       LlmEmotionFallbackOptions options)
    : llm_(std::move(llm)),
      options_(std::move(options)) {}

core::Result<EmotionAnalysis> LlmEmotionFallbackAnalyzer::Analyze(
    std::string_view text,
    std::string_view trace_id,
    std::shared_ptr<const PersonalityConfig>) {
    if (!llm_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "LLM emotion fallback client is required");
    }

    agent::llm::ChatCompletionRequest request;
    request.model = options_.model;
    request.temperature = 0.0f;
    request.max_tokens = 64;
    request.messages = {
        {agent::llm::ChatRole::System,
         "You are a strict emotion classifier. Return exactly one compact JSON object and no extra text. "
         "The emotion field MUST be one of these exact labels only: "
         "joy, sadness, anger, fear, surprise, disgust, neutral, excitement, tenderness, curiosity. "
         "Do not invent labels such as frustration, anxiety, disappointment, love, confusion, or annoyance; "
         "map them to the closest allowed label. Required JSON schema: "
         "{\"emotion\":\"joy|sadness|anger|fear|surprise|disgust|neutral|excitement|tenderness|curiosity\","
         "\"confidence\":0.0,\"intensity\":0.0,\"behavior\":\"neutral_acknowledge\",\"tone\":\"calm\"}. "
         "confidence and intensity must be numbers in [0,1]."},
        {agent::llm::ChatRole::User, std::string(text)},
    };
    static_cast<void>(trace_id);

    auto completed = llm_->Complete(request);
    if (!completed.ok()) {
        return completed.status();
    }
    try {
        const auto json = nlohmann::json::parse(completed.value().content);
        EmotionAnalysis analysis;
        analysis.emotion.primary = NormalizeEmotionLabel(json.value("emotion", "neutral"));
        analysis.emotion.primary_prob = Clamp(json.value("confidence", options_.default_confidence), 0.0, 1.0);
        analysis.emotion.intensity = Clamp(json.value("intensity", analysis.emotion.primary_prob), 0.0, 1.0);
        analysis.emotion.probabilities = {{analysis.emotion.primary, analysis.emotion.primary_prob}};
        analysis.behavior = json.value("behavior", "neutral_acknowledge");
        analysis.tone = json.value("tone", "calm");
        return analysis;
    } catch (const std::exception& e) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, std::string("LLM emotion fallback JSON parse failed: ") + e.what());
    }
}

FusedEmotionAnalyzer::FusedEmotionAnalyzer(std::shared_ptr<IEmotionAnalyzer> primary,
                                           EmotionFusionAnalyzerOptions options,
                                           std::vector<std::shared_ptr<IEmotionEvidenceProvider>> providers,
                                           std::shared_ptr<IEmotionAnalyzer> fallback)
    : primary_(std::move(primary)),
      options_(std::move(options)),
      providers_(std::move(providers)),
      fallback_(std::move(fallback)) {}

core::Result<EmotionAnalysis> FusedEmotionAnalyzer::Analyze(std::string_view text,
                                                            std::string_view trace_id,
                                                            std::shared_ptr<const PersonalityConfig> personality) {
    if (!primary_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition, "primary emotion analyzer is required");
    }

    auto primary = primary_->Analyze(text, trace_id, personality);
    if (!primary.ok() || !options_.enabled) {
        return primary;
    }

    return FusePrimary(text, trace_id, std::move(personality), std::move(primary).value());
}

core::Status FusedEmotionAnalyzer::AnalyzeAsync(
    std::string text,
    std::string trace_id,
    std::shared_ptr<const PersonalityConfig> personality,
    AnalyzeCompletion completion) {
    if (!completion) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "emotion completion is required");
    }
    if (!primary_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                   "primary emotion analyzer is required");
    }
    auto async_primary = std::dynamic_pointer_cast<IAsyncEmotionAnalyzer>(primary_);
    if (!async_primary) {
        completion(Analyze(text, trace_id, std::move(personality)));
        return core::Status::Ok();
    }
    return async_primary->AnalyzeAsync(
        text,
        trace_id,
        personality,
        [this, text, trace_id, personality,
         completion = std::move(completion)](
            core::Result<EmotionAnalysis> primary) mutable {
            if (!primary.ok() || !options_.enabled) {
                completion(std::move(primary));
                return;
            }
            completion(FusePrimary(
                text,
                trace_id,
                std::move(personality),
                std::move(primary).value()));
        });
}

core::Result<EmotionAnalysis> FusedEmotionAnalyzer::FusePrimary(
    std::string_view text,
    std::string_view trace_id,
    std::shared_ptr<const PersonalityConfig> personality,
    EmotionAnalysis primary) const {

    std::vector<EmotionEvidence> evidence;
    for (const auto& provider : providers_) {
        if (!provider) {
            continue;
        }
        auto collected = provider->Collect(text, trace_id);
        if (!collected.ok()) {
            return collected.status();
        }
        evidence.insert(evidence.end(),
                        std::make_move_iterator(collected.value().begin()),
                        std::make_move_iterator(collected.value().end()));
    }

    auto fused = FuseForTesting(primary, evidence);
    if (!fused.ok()) {
        return fused.status();
    }
    if (fallback_ && ShouldUseFallback(fused.value())) {
        auto fallback = fallback_->Analyze(text, trace_id, std::move(personality));
        if (fallback.ok()) {
            return ApplyFallbackGate(fused.value(), fallback.value());
        }
    }
    return fused;
}

core::Result<EmotionAnalysis> FusedEmotionAnalyzer::FuseForTesting(
    const EmotionAnalysis& primary,
    const std::vector<EmotionEvidence>& evidence) const {
    auto logits = BuildLogits(primary, evidence);
    if (logits.empty()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "emotion fusion has no labels");
    }
    auto probabilities = Softmax(logits);
    if (probabilities.empty()) {
        return core::Status::Error(core::ErrorCode::InternalError, "emotion fusion produced no probabilities");
    }
    return BuildResult(primary, probabilities);
}

std::map<std::string, double> FusedEmotionAnalyzer::BuildLogits(
    const EmotionAnalysis& primary,
    const std::vector<EmotionEvidence>& evidence) const {
    return BuildLogitsForCalibration(BuildFeaturesForCalibration(primary, evidence), options_);
}

std::vector<EmotionFusionFeature> FusedEmotionAnalyzer::BuildFeaturesForCalibration(
    const EmotionAnalysis& primary,
    const std::vector<EmotionEvidence>& evidence) const {
    std::set<std::string> labels;
    labels.insert(primary.emotion.primary);
    for (const auto& [label, _] : primary.emotion.probabilities) {
        labels.insert(label);
    }
    for (const auto& item : evidence) {
        if (!item.label.empty()) {
            labels.insert(item.label);
        }
    }

    std::map<std::string, double> keyword_scores;
    std::map<std::string, double> vector_scores;
    std::map<std::string, double> llm_scores;
    for (const auto& item : evidence) {
        if (item.label.empty() || item.score <= 0.0) {
            continue;
        }
        const double weighted = Clamp(item.score, 0.0, 1.0) * SourceWeight(item.source);
        if (item.source == "keyword") {
            keyword_scores[item.label] += weighted;
        } else if (item.source == "vector") {
            vector_scores[item.label] += weighted;
        } else if (item.source == "llm") {
            llm_scores[item.label] += weighted;
        } else {
            vector_scores[item.label] += weighted;
        }
    }

    const auto [_, bert_margin] = TopTwoMargin(primary.emotion.probabilities);
    std::vector<EmotionFusionFeature> features;
    features.reserve(labels.size());
    for (const auto& label : labels) {
        const double bert_prob = MapGet(primary.emotion.probabilities,
                                        label,
                                        label == primary.emotion.primary ? primary.emotion.primary_prob : 0.0);
        const double reliability = Reliability(label);
        const double keyword_score = MapGet(keyword_scores, label, 0.0);
        const double vector_score = MapGet(vector_scores, label, 0.0);
        const double llm_score = MapGet(llm_scores, label, 0.0);
        const double primary_margin_bonus = label == primary.emotion.primary ? bert_margin : -bert_margin;
        features.push_back(EmotionFusionFeature{
            label,
            bert_prob,
            reliability,
            keyword_score,
            vector_score,
            llm_score,
            primary_margin_bonus,
        });
    }
    return features;
}

std::map<std::string, double> FusedEmotionAnalyzer::BuildLogitsForCalibration(
    const std::vector<EmotionFusionFeature>& features,
    const EmotionFusionAnalyzerOptions& options) const {
    std::map<std::string, double> logits;
    for (const auto& feature : features) {
        if (feature.label.empty()) {
            continue;
        }
        logits[feature.label] = options.head_bias
            + options.bert_signal_weight * options.bert_weight * feature.bert_prob * feature.reliability
            + options.keyword_signal_weight * feature.keyword_score
            + options.vector_signal_weight * feature.vector_score
            + options.margin_signal_weight * feature.margin_bonus;
    }
    return logits;
}

std::map<std::string, double> FusedEmotionAnalyzer::Softmax(
    const std::map<std::string, double>& logits) const {
    return SoftmaxForCalibration(logits);
}

std::map<std::string, double> FusedEmotionAnalyzer::SoftmaxForCalibration(
    const std::map<std::string, double>& logits) const {
    std::map<std::string, double> probabilities;
    if (logits.empty()) {
        return probabilities;
    }

    const auto max_it = std::max_element(logits.begin(), logits.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
    const double max_logit = max_it->second;
    double sum = 0.0;
    for (const auto& [label, logit] : logits) {
        if (!std::isfinite(logit)) {
            continue;
        }
        const double value = std::exp(logit - max_logit);
        probabilities[label] = value;
        sum += value;
    }
    if (sum <= 0.0) {
        return {};
    }
    for (auto& [_, value] : probabilities) {
        value /= sum;
    }
    return probabilities;
}

EmotionAnalysis FusedEmotionAnalyzer::BuildResult(const EmotionAnalysis& primary,
                                                  const std::map<std::string, double>& probabilities) const {
    if (probabilities.empty()) {
        return primary;
    }

    EmotionAnalysis result = primary;
    result.emotion.probabilities = probabilities;

    auto best = std::max_element(
        result.emotion.probabilities.begin(),
        result.emotion.probabilities.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
    if (best != result.emotion.probabilities.end()) {
        result.emotion.primary = best->first;
        result.emotion.primary_prob = best->second;
        result.emotion.intensity = Clamp(std::max(primary.emotion.intensity, best->second), 0.0, 1.0);
    }
    return result;
}

EmotionAnalysis FusedEmotionAnalyzer::ApplyFallbackGate(const EmotionAnalysis& fused,
                                                        const EmotionAnalysis& fallback) const {
    const double fallback_confidence = fallback.emotion.primary_prob > 0.0
        ? fallback.emotion.primary_prob
        : 0.0;
    const double fused_same_prob = MapGet(fused.emotion.probabilities, fallback.emotion.primary, 0.0);
    const bool accept = fallback_confidence >= options_.llm_gate_confidence
        && fallback_confidence - fused_same_prob >= options_.llm_gate_min_delta;
    if (!accept) {
        return fused;
    }

    EmotionAnalysis result = fused;
    result.emotion.primary = fallback.emotion.primary;
    result.emotion.primary_prob = Clamp(fallback_confidence, 0.0, 1.0);
    result.emotion.intensity = Clamp(std::max(fused.emotion.intensity, fallback_confidence), 0.0, 1.0);
    result.behavior = fallback.behavior.empty() ? fused.behavior : fallback.behavior;
    result.tone = fallback.tone.empty() ? fused.tone : fallback.tone;

    auto probabilities = fused.emotion.probabilities;
    probabilities[result.emotion.primary] = std::max(probabilities[result.emotion.primary], result.emotion.primary_prob);
    double sum = 0.0;
    for (const auto& [_, value] : probabilities) {
        sum += std::max(0.0, value);
    }
    if (sum > 0.0) {
        for (auto& [_, value] : probabilities) {
            value = std::max(0.0, value) / sum;
        }
        result.emotion.probabilities = std::move(probabilities);
        result.emotion.primary_prob = MapGet(result.emotion.probabilities, result.emotion.primary, result.emotion.primary_prob);
    }
    return result;
}

bool FusedEmotionAnalyzer::ShouldUseFallback(const EmotionAnalysis& fused) const {
    const auto [top, margin] = TopTwoMargin(fused.emotion.probabilities);
    return top < options_.accept_confidence || margin < options_.ambiguity_margin;
}

double FusedEmotionAnalyzer::Reliability(std::string_view label) const {
    return MapGet(options_.label_reliability, label, options_.default_reliability);
}

double FusedEmotionAnalyzer::SourceWeight(std::string_view source) const {
    return MapGet(options_.source_weights, source, 0.5);
}

std::vector<EmotionKeywordRule> DefaultEmotionKeywordRules() {
    return {
        {"joy", "(开心|高兴|太好了|喜欢|有意思|明白了|懂了|会了|舒服了|爽|真棒|不错|好耶|哈哈|笑死|乐了)", 0.95},
        {"sadness", "(难过|沮丧|失落|没希望|不想学|学不会|考砸|崩溃|心累|emo|破防|想哭|哭了|寄了|完了|麻了|裂开|低落|绝望|没救了|好累|累死|绷不住了|蚌埠住了)", 0.95},
        {"anger", "(烦死了|讨厌|生气|不公平|气死|太烦|离谱|服了|无语|恼火|火大|破防了|急了|气人|暴躁|忍不了|不爽|真服了|什么鬼|搞什么|有病吧|烦人)", 0.95},
        {"fear", "(害怕|担心|紧张|怕错|不敢|焦虑|慌了|心慌|吓人|有点怕|压力好大|不安|忐忑|恐怖|可怕|吓死|吓到了|别吓我|完蛋了|不会要)", 0.90},
        {"surprise", "(惊讶|没想到|居然|竟然|原来如此|啊\\?|啊？|真的假的|不会吧|居然是|还能这样|震惊|离谱了|卧槽|我去|啊这|好家伙|不是吧|离大谱|牛啊|这也行|涨知识了|第一次知道)", 0.85},
        {"disgust", "(恶心|反感|受不了|厌恶|下头|恶心人|不适|看不下去|嫌弃|膈应|无感|别恶心我|太恶心|难绷|晦气|恶俗|油腻|尴尬死了)", 0.90},
        {"excitement", "(激动|兴奋|燃起来|太燃了|冲啊|起飞|期待|爽到了|上头)", 0.88},
        {"tenderness", "(谢谢|感谢|辛苦了|温柔|暖心|感动|好暖|抱抱|安慰|被治愈|好贴心|泪目|破防但好暖|太好了谢谢|谢谢老师|爱了|好温柔|有被安慰到)", 0.88},
        {"curiosity", "(为什么|怎么来的|想知道|好奇|能不能再讲|这一步|求解|啥意思|什么意思|怎么回事|为啥|咋来的)", 0.80},
        {"neutral", "(好的|嗯|知道了|收到|可以|OK|ok|行|了解|明白|好)", 0.50},
    };
}

std::vector<EmotionVectorPrototype> DefaultEmotionVectorPrototypes() {
    return {
        {"joy", "我终于懂了，这道题我会了", 1.0},
        {"joy", "这个知识点很有意思，我很开心", 0.95},
        {"sadness", "我怎么都学不会，感觉很难过", 1.0},
        {"sadness", "这次考试考砸了，我很失落", 0.95},
        {"anger", "这道题太烦了，我真的很生气", 1.0},
        {"anger", "为什么总是错，我受不了了", 0.9},
        {"fear", "我怕做错，考试前很紧张", 1.0},
        {"fear", "我有点担心，不敢继续做", 0.9},
        {"surprise", "没想到原来是这样，太意外了", 0.9},
        {"disgust", "这个内容让我很反感，受不了", 0.9},
        {"curiosity", "这一步为什么这样做，我想知道原因", 1.0},
        {"curiosity", "这个公式怎么来的，能不能再讲一下", 0.95},
        {"neutral", "好的，我知道了", 0.75},
    };
}

} // namespace agent::service::persona
