#include "skill_prompt_compiler.h"

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
    if (type == "integer") return value.is_number_integer();
    if (type == "boolean") return value.is_boolean();
    if (type == "null") return value.is_null();
    return false;
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
        }
    }

    if (value.is_array()) {
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
        const auto length = value.get<std::string>().size();
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

}  // namespace

core::Result<std::vector<SkillCall>> ParseToolCalls(
    const llm::ChatCompletionResponse& response) {
    std::vector<SkillCall> calls;
    for (const auto& tool_call : response.tool_calls) {
        if (tool_call.id.empty() || tool_call.name.empty() || tool_call.arguments_json.empty()) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       "tool call is missing id, name or arguments");
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

llm::ChatMessage MakeToolResultMessage(const SkillResult& result) {
    llm::ChatMessage message;
    message.role = llm::ChatRole::Tool;
    message.tool_call_id = result.call_id;
    if (result.status.ok()) {
        message.content = result.result_json.empty() ? "{}" : result.result_json;
        if (!result.provenance_json.empty()) {
            try {
                auto payload = nlohmann::json::parse(message.content);
                const auto provenance = nlohmann::json::parse(result.provenance_json);
                if (payload.is_object()) {
                    payload["provenance"] = provenance;
                } else {
                    payload = nlohmann::json{
                        {"result", std::move(payload)},
                        {"provenance", provenance},
                    };
                }
                message.content = payload.dump();
            } catch (const nlohmann::json::exception&) {
                // 工具结果本身仍需原样回填；业务 executor 可在上层把格式错误转换为失败结果。
                message.content = result.result_json;
            }
        }
    } else {
        nlohmann::json error = {
            {"error", {
                {"code", static_cast<int>(result.status.code())},
                {"message", result.status.message()}
            }}
        };
        message.content = error.dump();
    }
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
        try {
            const auto schema = nlohmann::json::parse(manifest.value().input_schema_json);
            const auto arguments = nlohmann::json::parse(call.arguments_json);
            if (auto status = ValidateJsonSchemaValue(schema, arguments, "$"); !status.ok()) {
                return status;
            }
        } catch (const nlohmann::json::exception& error) {
            return core::Status::Error(core::ErrorCode::InvalidArgument,
                                       std::string("invalid tool manifest schema: ") + error.what());
        }
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
