#include "sse.h"
#include "text_validation.h"

#include <exception>
#include <utility>

namespace agent::net {

SseDecoder::SseDecoder(Sink sink, std::size_t max_event_bytes)
    : sink_(std::move(sink)), limit_(max_event_bytes) {}

core::Status SseDecoder::Feed(std::string_view bytes) {
    if (!status_.ok()) return status_;
    try {
        for (const char value : bytes) {
            if (skip_lf_) {
                skip_lf_ = false;
                if (value == '\n') continue;
            }
            if (value == '\r' || value == '\n') {
                skip_lf_ = value == '\r';
                status_ = Line();
                line_.clear();
                if (!status_.ok()) return status_;
            } else {
                // 连注释/未知字段也计入事件上限，防止无限心跳行或无空行输入占内存。
                if (++event_bytes_ > limit_) {
                    status_ = core::Status::Error(core::ErrorCode::ResourceExhausted,
                                                  "SSE event exceeds byte limit");
                    return status_;
                }
                line_.push_back(value);
            }
        }
    } catch (const std::exception&) {
        status_ = core::Status::Error(core::ErrorCode::InternalError, "SSE callback or allocation failed");
    } catch (...) {
        status_ = core::Status::Error(core::ErrorCode::InternalError, "SSE callback failed");
    }
    return status_;
}

core::Status SseDecoder::Line() {
    if (first_line_) {
        first_line_ = false;
        if (line_.starts_with("\xef\xbb\xbf")) line_.erase(0, 3);
    }
    if (!core::IsValidUtf8(line_))
        return core::Status::Error(core::ErrorCode::DataLoss, "SSE line is not valid UTF-8");
    if (line_.empty()) {
        core::Status result;
        if (has_data_) {
            event_.data.pop_back(); // 每条 data 行附加 LF，派发时去掉最后一个。
            result = sink_(event_);
        }
        event_ = {};
        has_data_ = false;
        event_bytes_ = 0;
        return result;
    }
    if (line_.front() == ':') return core::Status::Ok();
    const auto colon = line_.find(':');
    const auto field = std::string_view(line_).substr(0, colon);
    auto value = colon == std::string::npos ? std::string_view{} : std::string_view(line_).substr(colon + 1);
    if (value.starts_with(' ')) value.remove_prefix(1);
    if (field == "data") {
        event_.data.append(value);
        event_.data.push_back('\n');
        has_data_ = true;
    } else if (field == "event") {
        event_.event = value;
    } else if (field == "id" && value.find('\0') == std::string_view::npos) {
        event_.id = value;
    }
    // retry/未知字段按 SSE 规范忽略；是否重试由 operation 的副作用边界决定。
    return core::Status::Ok();
}

core::Status SseDecoder::Finish() {
    if (!status_.ok()) return status_;
    if (!line_.empty() || has_data_) {
        status_ = core::Status::Error(core::ErrorCode::DataLoss, "SSE ended inside an event");
    }
    return status_;
}

}
