#include "skill_prompt_compiler.h"
#include "../core/text_validation.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <nlohmann/json.hpp>

namespace agent::skill {
namespace {

using Json = nlohmann::json;

std::string SchemaPath(std::string_view path, std::string_view property) {
    return std::string(path) + "." + std::string(property);
}

bool JsonTypeMatches(const Json& value, std::string_view type) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "number") return value.is_number();
    if (type == "integer") return value.is_number_integer() ||
        (value.is_number_float() && std::isfinite(value.get<double>()) &&
         std::floor(value.get<double>()) == value.get<double>());
    if (type == "boolean") return value.is_boolean();
    if (type == "null") return value.is_null();
    return false;
}

// 当前 manifest 不依赖通用 JSON Schema 库；明确支持子集并拒绝未知断言，防止错误的安全保证。
core::Status ValidateSchemaNode(const Json& schema, std::size_t depth = 0) {
    if (!schema.is_object() || depth > 32) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "skill schema must be an object with depth at most 32");
    }
    static const std::unordered_set<std::string> supported{
        "type", "enum", "const", "required", "properties", "additionalProperties", "items",
        "minLength", "maxLength", "minimum", "maximum", "minItems", "maxItems",
        "title", "description", "default", "examples", "$schema", "$id",
        "deprecated", "readOnly", "writeOnly"};
    const auto invalid = [](std::string_view key) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "unsupported or malformed skill schema keyword: " + std::string(key));
    };
    for (const auto& [key, value] : schema.items()) {
        if (!supported.contains(key)) return invalid(key);
        if (key == "type") {
            const auto valid_type = [](const Json& type) {
                if (!type.is_string()) return false;
                const auto name = type.get<std::string>();
                return name == "object" || name == "array" || name == "string" || name == "number" ||
                    name == "integer" || name == "boolean" || name == "null";
            };
            if (!valid_type(value) && (!value.is_array() || value.empty() ||
                !std::all_of(value.begin(), value.end(), valid_type))) return invalid(key);
        } else if (key == "properties") {
            if (!value.is_object()) return invalid(key);
            for (const auto& child : value) {
                if (auto status = ValidateSchemaNode(child, depth + 1); !status.ok()) return status;
            }
        } else if (key == "items" || (key == "additionalProperties" && value.is_object())) {
            if (auto status = ValidateSchemaNode(value, depth + 1); !status.ok()) return status;
        } else if (key == "additionalProperties") {
            if (!value.is_boolean()) return invalid(key);
        } else if (key == "required") {
            if (!value.is_array() || !std::all_of(value.begin(), value.end(),
                    [](const Json& name) { return name.is_string(); })) return invalid(key);
        } else if (key == "enum") {
            if (!value.is_array() || value.empty()) return invalid(key);
        } else if (key == "minLength" || key == "maxLength" || key == "minItems" || key == "maxItems") {
            if (!value.is_number_integer() || value.get<double>() < 0) return invalid(key);
        } else if (key == "minimum" || key == "maximum") {
            if (!value.is_number()) return invalid(key);
        }
    }
    return core::Status::Ok();
}

core::Status ValidateJsonSchemaValue(
    const Json& schema,
    const Json& value,
    std::string_view path) {
    if (!schema.is_object()) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "tool input schema node must be an object");
    }

    if (const auto type = schema.find("type"); type != schema.end()) {
        bool type_matches = false;
        if (type->is_string()) {
            type_matches = JsonTypeMatches(value, type->get<std::string>());
        } else if (type->is_array()) {
            for (const auto& candidate : *type) {
                if (candidate.is_string() &&
                    JsonTypeMatches(value, candidate.get<std::string>())) {
                    type_matches = true;
                    break;
                }
            }
        } else {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "tool input schema type must be a string or array");
        }
        if (!type_matches) {
            return core::Status::Error(
                core::ErrorCode::InvalidArgument,
                "tool argument type mismatch at " + std::string(path));
        }
    }

    if (const auto enumeration = schema.find("enum"); enumeration != schema.end()) {
        if (!enumeration->is_array()) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "tool input schema enum must be an array");
        }
        bool found = false;
        for (const auto& candidate : *enumeration) {
            if (candidate == value) {
                found = true;
                break;
            }
        }
        if (!found) {
            return core::Status::Error(
                core::ErrorCode::InvalidArgument,
                "tool argument is not an allowed enum value at " + std::string(path));
        }
    }
    if (const auto constant = schema.find("const"); constant != schema.end() &&
        *constant != value) {
        return core::Status::Error(
            core::ErrorCode::InvalidArgument,
            "tool argument does not match const at " + std::string(path));
    }

    if (value.is_object()) {
        const auto required = schema.find("required");
        if (required != schema.end()) {
            if (!required->is_array()) {
                return core::Status::Error(core::ErrorCode::InvalidArgument,
                                           "tool input schema required must be an array");
            }
            for (const auto& name : *required) {
                if (!name.is_string()) {
                    return core::Status::Error(core::ErrorCode::InvalidArgument,
                                               "tool input schema required entries must be strings");
                }
                const auto key = name.get<std::string>();
                if (!value.contains(key)) {
                    return core::Status::Error(
                        core::ErrorCode::InvalidArgument,
                        "tool call is missing required argument: " + key);
                }
            }
        }

        const auto properties = schema.find("properties");
        if (properties != schema.end() && !properties->is_object()) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "tool input schema properties must be an object");
        }
        const auto additional = schema.find("additionalProperties");
        const bool reject_additional =
            additional != schema.end() && additional->is_boolean() && !additional->get<bool>();
        for (const auto& [name, child] : value.items()) {
            if (properties != schema.end()) {
                const auto property = properties->find(name);
                if (property != properties->end()) {
                    if (auto status = ValidateJsonSchemaValue(
                            *property, child, SchemaPath(path, name)); !status.ok()) {
                        return status;
                    }
                    continue;
                }
            }
            if (reject_additional) {
                return core::Status::Error(
                    core::ErrorCode::InvalidArgument,
                    "tool call contains unexpected argument: " + name);
            }
            if (additional != schema.end() && additional->is_object()) {
                if (auto status = ValidateJsonSchemaValue(
                        *additional, child, SchemaPath(path, name)); !status.ok()) return status;
            }
        }
    }

    if (value.is_array()) {
        if ((schema.contains("minItems") && value.size() < schema["minItems"].get<std::size_t>()) ||
            (schema.contains("maxItems") && value.size() > schema["maxItems"].get<std::size_t>())) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "tool array length is outside schema bounds at " + std::string(path));
        }
        if (const auto items = schema.find("items"); items != schema.end()) {
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (auto status = ValidateJsonSchemaValue(
                        *items, value[index], SchemaPath(path, std::to_string(index))); !status.ok()) {
                    return status;
                }
            }
        }
    }

    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (!core::IsValidUtf8(text)) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "tool string argument is not valid UTF-8 at " + std::string(path));
        }
        const auto length = static_cast<std::size_t>(std::count_if(
            text.begin(), text.end(), [](unsigned char byte) { return (byte & 0xC0) != 0x80; }));
        if (schema.contains("minLength") &&
            (!schema["minLength"].is_number_unsigned() ||
             length < schema["minLength"].get<std::size_t>())) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "tool argument is shorter than minLength at " + std::string(path));
        }
        if (schema.contains("maxLength") &&
            (!schema["maxLength"].is_number_unsigned() ||
             length > schema["maxLength"].get<std::size_t>())) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "tool argument exceeds maxLength at " + std::string(path));
        }
    }

    if (value.is_number()) {
        if (schema.contains("minimum") &&
            (!schema["minimum"].is_number() || value.get<double>() < schema["minimum"].get<double>())) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "tool argument is below minimum at " + std::string(path));
        }
        if (schema.contains("maximum") &&
            (!schema["maximum"].is_number() || value.get<double>() > schema["maximum"].get<double>())) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "tool argument exceeds maximum at " + std::string(path));
        }
    }
    return core::Status::Ok();
}

}

core::Status ValidateSkillSchema(std::string_view schema_json) {
    try {
        if (schema_json.size() > 64 * 1024 || !core::IsValidUtf8(schema_json)) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "skill schema exceeds size limit or contains invalid UTF-8");
        }
        return ValidateSchemaNode(Json::parse(schema_json));
    } catch (const Json::exception&) {
        return core::Status::Error(core::ErrorCode::InvalidArgument, "skill schema is invalid JSON");
    }
}

core::Status ValidateSkillJsonSchema(
    std::string_view schema_json,
    std::string_view value_json) {
    if (auto status = ValidateSkillSchema(schema_json); !status.ok()) return status;
    try {
        const auto schema = Json::parse(schema_json);
        const auto value = Json::parse(value_json);
        return ValidateJsonSchemaValue(schema, value, "$");
    } catch (const Json::exception&) {
        return core::Status::Error(core::ErrorCode::InvalidArgument,
                                   "tool schema or value is invalid JSON");
    }
}

core::Result<std::vector<SkillCall>> ParseToolCalls(
    const llm::ChatCompletionResponse& response) {
    std::vector<SkillCall> calls;
    std::unordered_set<std::string> call_ids;
    if (response.tool_calls.size() > 32) {
        return core::Status::Error(core::ErrorCode::ResourceExhausted, "too many tool calls in one round");
    }
    for (const auto& tool_call : response.tool_calls) {
        if (tool_call.id.empty() || tool_call.name.empty() || tool_call.arguments_json.empty()) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "tool call is missing id, name or arguments");
        }
        if (!call_ids.insert(tool_call.id).second || !core::IsValidUtf8(tool_call.id)) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "tool call id must be unique and valid UTF-8");
        }
        if (tool_call.arguments_json.size() > 64 * 1024 || tool_call.id.size() > 256 ||
            tool_call.name.size() > 256) {
            return core::Status::Error(core::ErrorCode::ResourceExhausted, "tool call exceeds size limit");
        }
        if (!core::IsValidUtf8(tool_call.name) || !core::IsValidUtf8(tool_call.arguments_json) ||
            core::HasInvalidTextControl(tool_call.id) || core::HasInvalidTextControl(tool_call.name)) {
            return core::Status::Error(core::ErrorCode::InvalidArgument, "tool call contains invalid text");
        }
        try {
            const auto arguments = nlohmann::json::parse(tool_call.arguments_json);
            if (!arguments.is_object()) {
                return core::Status::Error(core::ErrorCode::InvalidArgument,
                                           "tool call arguments must be a JSON object");
            }
        } catch (const nlohmann::json::exception& error) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       std::string("invalid tool call arguments: ") + error.what());
        }
        calls.push_back(SkillCall{tool_call.id, tool_call.name, {}, {}, tool_call.arguments_json});
    }
    if (!calls.empty()) return calls;
    return core::Status::Error(core::ErrorCode::NotFound, "LLM response contains no tool calls");
}

llm::ChatMessage MakeToolResultMessage(const SkillResult& result, ToolResultFormat format) {
    llm::ChatMessage message;
    message.role = llm::ChatRole::Tool;
    message.tool_call_id = result.call_id;
    auto status = result.status;
    Json payload = Json::object();
    Json provenance;
    if (status.ok()) {
        try {
            payload = Json::parse(result.result_json.empty() ? "{}" : result.result_json);
            if (!result.provenance_json.empty()) {
                provenance = Json::parse(result.provenance_json);
                if (!provenance.is_object() ||
                    (payload.is_object() && payload.contains("provenance") && payload["provenance"] != provenance)) {
                    status = core::Status::Error(core::ErrorCode::DataLoss,
                                                "tool provenance is invalid or conflicts with payload");
                }
            }
        } catch (const Json::exception&) {
            status = core::Status::Error(core::ErrorCode::DataLoss,
                                        "tool result or provenance is invalid UTF-8 or JSON");
        }
    }
    Json body;
    if (format == ToolResultFormat::EnvelopeV1) {
        body = {{"schema_version", "agentloom.tool-result.v1"},
                {"call_id", result.call_id}, {"skill_id", result.skill_id},
                {"status", !status.ok() ? "error" : (result.result_json.empty() ? "empty" : "success")}};
        if (status.ok()) {
            body["result"] = std::move(payload);
            if (!provenance.is_null()) body["provenance"] = std::move(provenance);
        }
    } else if (status.ok()) {
        if (provenance.is_null()) {
            // 保留旧消费者可见的 payload 字段及原始 JSON 排版。
            message.content = result.result_json.empty() ? "{}" : result.result_json;
            return message;
        }
        if (payload.is_object()) {
            payload["provenance"] = std::move(provenance);
            body = std::move(payload);
        } else {
            body = {{"result", std::move(payload)}, {"provenance", std::move(provenance)}};
        }
    }
    if (!status.ok()) {
        body["error"] = {{"code", static_cast<int>(status.code())}, {"message", status.message()}};
    }
    // 错误也是执行器/消费者的外部文本，替换非法 UTF-8，保证失败仍能稳定回填。
    message.content = body.dump(-1, ' ', false, Json::error_handler_t::replace);
    return message;
}

core::Result<std::vector<SkillCall>> ParseToolCalls(
    const llm::ChatCompletionResponse& response,
    const ISkillRegistry& registry) {
    auto parsed = ParseToolCalls(response);
    if (!parsed.ok()) return parsed.status();
    for (auto& call : parsed.value()) {
        auto manifest = registry.RequireByToolName(call.tool_name);
        if (!manifest.ok() && manifest.status().code() == core::ErrorCode::NotFound) {
            // 兼容早期 provider：旧协议把 skill_id 直接作为 function name。
            manifest = registry.Require(call.tool_name);
        }
        if (!manifest.ok()) return manifest.status();
        if (auto status = ValidateSkillJsonSchema(
                manifest.value().input_schema_json, call.arguments_json); !status.ok()) return status;
        call.skill_id = manifest.value().skill_id;
        call.skill_version = manifest.value().version;
    }
    return parsed;
}

SkillPromptCompiler::SkillPromptCompiler(std::shared_ptr<const ISkillRegistry> registry)
    : registry_(std::move(registry)) {}

core::Result<std::string> SkillPromptCompiler::Compile(const SkillPromptRequest& request) const {
    if (!registry_) {
        return core::Status::Error(core::ErrorCode::FailedPrecondition,
                                   "skill registry is not configured");
    }
    std::ostringstream out;
    out << "<skill_tool_protocol>\n"
        << "Use only the declared tools and valid JSON arguments.\n"
        << "Do not claim an external result before the tool returns.\n"
        << "</skill_tool_protocol>\n";
    std::unordered_set<std::string> seen;
    for (const auto& id : request.skill_ids) {
        if (!seen.insert(id).second) continue;
        auto manifest = registry_->Require(id);
        if (!manifest.ok()) return manifest.status();
        out << "<skill_tool id=\"" << manifest.value().skill_id << "\" name=\""
            << manifest.value().tool_name << "\" version=\""
            << manifest.value().version << "\">\n"
            << "description: " << manifest.value().description << "\n"
            << "instruction: " << manifest.value().prompt_instruction << "\n"
            << "input_schema: " << manifest.value().input_schema_json << "\n"
            << "</skill_tool>\n";
    }
    if (!request.session_state.empty()) out << "<skill_session_state>\n" << request.session_state << "\n</skill_session_state>\n";
    if (!request.external_context.empty()) out << "<skill_external_context>\n" << request.external_context << "\n</skill_external_context>\n";
    return out.str();
}

} // namespace agent::skill
