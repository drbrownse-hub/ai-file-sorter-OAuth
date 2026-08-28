#pragma once

#include "CodexProtocol.hpp"

#include <QObject>

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QProcess;
class CodexAppServerTestAccess;

struct CodexLaunchConfig {
    std::string executable_path;
    std::string codex_home;
    std::string client_version;
};

struct CodexTurnRequest {
    CodexInferenceConfig config;
    std::vector<CodexUserInput> inputs;
    Json::Value output_schema;
};

struct CodexTurnResult {
    std::string text;
    CodexTurnCompletion completion;
};

class CodexAppServer final : public QObject {
    Q_OBJECT

public:
    explicit CodexAppServer(QObject* parent = nullptr);
    ~CodexAppServer() override;

    void start(const CodexLaunchConfig& config);
    void stop();
    bool is_running() const;

    Json::Value request(std::string_view method,
                        const Json::Value& params,
                        std::chrono::milliseconds timeout);
    void notify(std::string_view method, const Json::Value& params = {});
    std::optional<CodexAccountNotification> take_account_notification();

    CodexTurnResult run_ephemeral_turn(const CodexTurnRequest& request,
                                       std::chrono::milliseconds timeout,
                                       const std::function<bool()>& cancelled);
    void interrupt_turn(std::string_view thread_id, std::string_view turn_id);

#ifdef AI_FILE_SORTER_TEST_BUILD
    void set_test_active_turn_hook(std::function<void()> hook);
#endif

signals:
    void transportStateChanged();
    void notificationReceived();
    void accountNotificationReceived();

private:
    friend class CodexAppServerTestAccess;

    struct ActiveTurn {
        std::string thread_id;
        std::string turn_id;
        std::string text;
        std::optional<CodexTurnCompletion> completion;
    };

    void ensure_owner_thread() const;
    Json::Value request_internal(std::string_view method,
                                 const Json::Value& params,
                                 std::chrono::milliseconds timeout,
                                 const std::function<bool()>& cancelled,
                                 bool allow_initializing);
    void notify_internal(std::string_view method, const Json::Value& params, bool allow_initializing);
    void send_message(const Json::Value& message);
    void read_stdout();
    void read_stderr();
    void process_stdout_record(const QByteArray& record);
    void process_notification(const Json::Value& notification);
    void set_fatal_error(CodexErrorKind kind, std::string message);
    void throw_if_unavailable() const;
    void ensure_public_ready() const;
    void retire_after_setup_failure(CodexErrorKind kind);
    void wait_until(const std::function<bool()>& ready,
                    std::chrono::milliseconds timeout,
                    const std::function<bool()>& cancelled = {});

    QProcess* process_;
    QByteArray stdout_buffer_;
    QByteArray stderr_buffer_;
    Json::Int64 next_request_id_{1};
    std::map<Json::Int64, Json::Value> responses_;
    std::deque<Json::Value> notifications_;
    std::deque<Json::Value> pending_turn_notifications_;
    std::string pending_turn_thread_id_;
    std::optional<CodexAccountNotification> last_account_notification_;
    std::optional<ActiveTurn> active_turn_;
    std::optional<CodexErrorKind> fatal_error_kind_;
    std::string fatal_error_message_;
    bool running_{false};
    bool stopping_{false};
    bool initializing_{false};
    bool initialized_{false};

#ifdef AI_FILE_SORTER_TEST_BUILD
    std::function<void()> test_active_turn_hook_;
#endif

    static constexpr std::size_t max_pending_notifications_ = 256;
};
