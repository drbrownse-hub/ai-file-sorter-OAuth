#pragma once

#if __has_include(<jsoncpp/json/json.h>)
#include <jsoncpp/json/json.h>
#elif __has_include(<json/json.h>)
#include <json/json.h>
#else
#error "jsoncpp headers not found. Install jsoncpp development files."
#endif

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

enum class CodexErrorKind {
    RuntimeNotFound,
    StartupFailed,
    ProtocolError,
    AuthenticationRequired,
    RateLimited,
    ModelUnavailable,
    ImageUnsupported,
    TurnFailed,
    Timeout,
    ProcessCrashed,
    Cancelled,
};

class CodexError final : public std::runtime_error {
public:
    CodexError(CodexErrorKind kind,
               std::string message,
               std::optional<int> protocol_code = std::nullopt);

    CodexErrorKind kind() const noexcept;
    std::optional<int> protocol_code() const noexcept;

private:
    CodexErrorKind kind_;
    std::optional<int> protocol_code_;
};

struct CodexAccountInfo {
    bool authenticated{false};
    std::string email;
    std::string plan_type;
};

struct CodexModelInfo {
    std::string id;
    std::string display_name;
    bool is_default{false};
    bool accepts_image{false};
};

struct CodexInferenceConfig {
    std::string model;
    std::string inference_cwd;
    std::string base_instructions;
    std::string developer_instructions;
};

struct CodexUserInput {
    enum class Kind { Text, ImageDataUrl };

    Kind kind{Kind::Text};
    std::string value;
};

struct CodexResponse {
    Json::Int64 id{0};
    Json::Value result;
};

enum class CodexAccountNotificationKind { Updated, LoginCompleted, RateLimitsUpdated };

struct CodexAccountNotification {
    CodexAccountNotificationKind kind{CodexAccountNotificationKind::Updated};
    bool authenticated{false};
    std::string plan_type;
    std::string login_id;
    bool success{false};
    std::string error_message;
    Json::Value rate_limits;
};

struct CodexUsage {
    int input_tokens{0};
    int output_tokens{0};
};

enum class CodexTurnStatus { Completed, Interrupted, Failed };

struct CodexTurnCompletion {
    std::string thread_id;
    std::string turn_id;
    CodexTurnStatus status{CodexTurnStatus::Failed};
    CodexUsage usage;
    std::string error_message;
};

namespace CodexProtocol {

Json::Value make_initialize_params(std::string_view version);
Json::Value make_chatgpt_login_params();
Json::Value make_chatgpt_device_code_login_params();
Json::Value make_thread_start_params(const CodexInferenceConfig& config);
Json::Value make_turn_start_params(std::string_view thread_id,
                                   const std::vector<CodexUserInput>& inputs,
                                   const Json::Value& output_schema);

CodexResponse parse_response(const Json::Value& message);
CodexAccountInfo parse_account_read_response(const Json::Value& result);
Json::Value parse_account_rate_limits_read_response(const Json::Value& result);
std::vector<CodexModelInfo> parse_model_list_response(const Json::Value& result);
std::optional<CodexAccountNotification> parse_account_notification(const Json::Value& notification);
bool append_agent_message_delta(const Json::Value& notification,
                                std::string_view thread_id,
                                std::string_view turn_id,
                                std::string& text);
std::optional<CodexTurnCompletion> parse_turn_completed_notification(const Json::Value& notification);

} // namespace CodexProtocol
