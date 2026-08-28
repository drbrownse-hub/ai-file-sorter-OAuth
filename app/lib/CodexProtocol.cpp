#include "CodexProtocol.hpp"

#include <algorithm>
#include <string>

namespace {

std::string string_member(const Json::Value& value, const char* name)
{
    const Json::Value& member = value[name];
    return member.isString() ? member.asString() : std::string();
}

int int_member(const Json::Value& value, const char* camel_case, const char* snake_case)
{
    if (value[camel_case].isInt()) {
        return value[camel_case].asInt();
    }
    return value[snake_case].isInt() ? value[snake_case].asInt() : 0;
}

bool is_image_data_url(std::string_view value)
{
    return value.starts_with("data:image/") && value.find(";base64,") != std::string_view::npos;
}

CodexTurnStatus parse_turn_status(std::string_view status)
{
    if (status == "completed") {
        return CodexTurnStatus::Completed;
    }
    if (status == "interrupted") {
        return CodexTurnStatus::Interrupted;
    }
    return CodexTurnStatus::Failed;
}

} // namespace

CodexError::CodexError(CodexErrorKind kind, std::string message)
    : std::runtime_error(std::move(message)), kind_(kind)
{
}

CodexErrorKind CodexError::kind() const noexcept
{
    return kind_;
}

namespace CodexProtocol {

Json::Value make_initialize_params(std::string_view version)
{
    Json::Value params(Json::objectValue);
    params["clientInfo"]["name"] = "ai_file_sorter";
    params["clientInfo"]["title"] = "AI File Sorter";
    params["clientInfo"]["version"] = std::string(version);
    return params;
}

Json::Value make_chatgpt_login_params()
{
    Json::Value params(Json::objectValue);
    params["type"] = "chatgpt";
    return params;
}

Json::Value make_chatgpt_device_code_login_params()
{
    Json::Value params(Json::objectValue);
    params["type"] = "chatgptDeviceCode";
    return params;
}

Json::Value make_thread_start_params(const CodexInferenceConfig& config)
{
    Json::Value params(Json::objectValue);
    if (!config.model.empty()) {
        params["model"] = config.model;
    }
    params["ephemeral"] = true;
    params["approvalPolicy"] = "never";
    params["sandbox"] = "read-only";
    params["cwd"] = config.inference_cwd;
    params["baseInstructions"] = config.base_instructions;
    params["developerInstructions"] = config.developer_instructions;
    return params;
}

Json::Value make_turn_start_params(std::string_view thread_id,
                                   const std::vector<CodexUserInput>& inputs,
                                   const Json::Value& output_schema)
{
    Json::Value params(Json::objectValue);
    params["threadId"] = std::string(thread_id);
    params["input"] = Json::Value(Json::arrayValue);

    for (const CodexUserInput& input : inputs) {
        Json::Value serialized(Json::objectValue);
        if (input.kind == CodexUserInput::Kind::Text) {
            serialized["type"] = "text";
            serialized["text"] = input.value;
        } else {
            if (!is_image_data_url(input.value)) {
                throw CodexError(CodexErrorKind::ImageUnsupported,
                                 "Codex image input must be an inline image data URL.");
            }
            serialized["type"] = "image";
            serialized["imageUrl"] = input.value;
        }
        params["input"].append(std::move(serialized));
    }

    if (!output_schema.isNull()) {
        params["outputSchema"] = output_schema;
    }
    return params;
}

CodexResponse parse_response(const Json::Value& message)
{
    if (!message.isObject() || !message["id"].isIntegral()) {
        throw CodexError(CodexErrorKind::ProtocolError, "Codex response is missing an integer id.");
    }
    if (message.isMember("error")) {
        const Json::Value& error = message["error"];
        const int code = error["code"].isInt() ? error["code"].asInt() : 0;
        const std::string error_message = string_member(error, "message");
        if (code == -32001) {
            throw CodexError(CodexErrorKind::RateLimited,
                             error_message.empty() ? "Codex server overloaded; retry later." : error_message);
        }
        throw CodexError(CodexErrorKind::ProtocolError,
                         error_message.empty() ? "Codex returned a protocol error." : error_message);
    }
    if (!message.isMember("result")) {
        throw CodexError(CodexErrorKind::ProtocolError, "Codex response is missing a result.");
    }
    return {message["id"].asInt64(), message["result"]};
}

CodexAccountInfo parse_account_read_response(const Json::Value& result)
{
    CodexAccountInfo account;
    const Json::Value& raw_account = result["account"];
    if (!raw_account.isObject() || string_member(raw_account, "type") != "chatgpt") {
        return account;
    }
    account.authenticated = true;
    account.email = string_member(raw_account, "email");
    account.plan_type = string_member(raw_account, "planType");
    return account;
}

std::vector<CodexModelInfo> parse_model_list_response(const Json::Value& result)
{
    std::vector<CodexModelInfo> models;
    const Json::Value& data = result["data"];
    if (!data.isArray()) {
        return models;
    }
    models.reserve(data.size());
    for (const Json::Value& raw_model : data) {
        CodexModelInfo model;
        model.id = string_member(raw_model, "id");
        model.display_name = string_member(raw_model, "displayName");
        model.is_default = raw_model["isDefault"].asBool();
        const Json::Value& modalities = raw_model["inputModalities"];
        model.accepts_image = std::any_of(modalities.begin(), modalities.end(), [](const Json::Value& modality) {
            return modality.isString() && modality.asString() == "image";
        });
        models.push_back(std::move(model));
    }
    return models;
}

std::optional<CodexAccountNotification> parse_account_notification(const Json::Value& notification)
{
    if (!notification.isObject()) {
        return std::nullopt;
    }
    const std::string method = string_member(notification, "method");
    const Json::Value& params = notification["params"];
    if (method == "account/updated") {
        return CodexAccountNotification{
            .kind = CodexAccountNotificationKind::Updated,
            .authenticated = string_member(params, "authMode") == "chatgpt",
            .plan_type = string_member(params, "planType"),
        };
    }
    if (method == "account/login/completed") {
        return CodexAccountNotification{
            .kind = CodexAccountNotificationKind::LoginCompleted,
            .login_id = string_member(params, "loginId"),
            .success = params["success"].asBool(),
            .error_message = string_member(params, "error"),
        };
    }
    if (method == "account/rateLimits/updated") {
        return CodexAccountNotification{
            .kind = CodexAccountNotificationKind::RateLimitsUpdated,
            .rate_limits = params["rateLimits"],
        };
    }
    return std::nullopt;
}

bool append_agent_message_delta(const Json::Value& notification,
                                std::string_view thread_id,
                                std::string_view turn_id,
                                std::string& text)
{
    if (!notification.isObject() || string_member(notification, "method") != "item/agentMessage/delta") {
        return false;
    }
    const Json::Value& params = notification["params"];
    if (string_member(params, "threadId") != thread_id || string_member(params, "turnId") != turn_id) {
        return false;
    }
    text += string_member(params, "delta");
    return true;
}

std::optional<CodexTurnCompletion> parse_turn_completed_notification(const Json::Value& notification)
{
    if (!notification.isObject() || string_member(notification, "method") != "turn/completed") {
        return std::nullopt;
    }
    const Json::Value& params = notification["params"];
    const Json::Value& turn = params["turn"];
    if (!turn.isObject()) {
        throw CodexError(CodexErrorKind::ProtocolError, "Codex turn completion is missing a turn.");
    }
    const Json::Value& usage = turn["usage"];
    return CodexTurnCompletion{
        .thread_id = string_member(params, "threadId"),
        .turn_id = string_member(turn, "id"),
        .status = parse_turn_status(string_member(turn, "status")),
        .usage = {
            .input_tokens = int_member(usage, "inputTokens", "input_tokens"),
            .output_tokens = int_member(usage, "outputTokens", "output_tokens"),
        },
        .error_message = string_member(turn["error"], "message"),
    };
}

} // namespace CodexProtocol
