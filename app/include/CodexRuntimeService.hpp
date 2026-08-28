#pragma once

#include "CodexAppServer.hpp"

#include <QObject>
#include <QUrl>

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

struct CodexRuntimeConfig {
    std::string executable_path;
    std::string application_directory;
    std::vector<std::string> path_entries;
    std::string codex_home;
    std::string model;
    std::string client_version;
};

struct CodexRuntimeSnapshot {
    bool runtime_found{false};
    bool running{false};
    bool authenticated{false};
    std::string runtime_version;
    CodexAccountInfo account;
    std::vector<CodexModelInfo> models;
    std::string last_error;
};

class QThread;
class CodexRuntimeWorker;
struct CodexRuntimeSharedState;

class CodexRuntimeService final : public QObject {
    Q_OBJECT

public:
    explicit CodexRuntimeService(QObject* parent = nullptr);
    ~CodexRuntimeService() override;

    void configure(CodexRuntimeConfig config);
    /**
     * @brief Update only the executable path while preserving the configured runtime context.
     * @param executable_path New explicit Codex executable path.
     */
    void configure_executable_path_async(std::string executable_path);
    CodexRuntimeSnapshot snapshot() const;

    void start_or_refresh_async();
    /**
     * @brief Request cancellation of the currently running inference turn.
     *
     * The request is thread-safe and returns immediately. It affects only an
     * active turn; the next turn starts with a clear cancellation state.
     */
    void cancel_active_turn_async() noexcept;
    void begin_chatgpt_login_async();
    void begin_device_code_login_async();
    void logout_async();

#ifdef AI_FILE_SORTER_TEST_BUILD
    void set_test_run_turn_unwind_hook(std::function<void()> hook);
    void set_test_destructor_before_shutdown_hook(std::function<void()> hook);
    void set_test_active_turn_hook(std::function<void()> hook);
    void set_test_worker_destruction_hook(std::function<void(bool)> hook);
#endif

    CodexTurnResult run_turn(const CodexTurnRequest& request,
                             const std::function<bool()>& cancelled = {});
    bool selected_model_accepts_images(std::string_view configured_model) const;

signals:
    void stateChanged();
    void loginUrlReady(const QUrl& url);
    void deviceCodeReady(const QUrl& verification_url, const QString& user_code);

private:
    void begin_operation();
    void end_operation();
    void wait_for_operations();

    std::shared_ptr<CodexRuntimeSharedState> state_;
    QThread* worker_thread_;
    CodexRuntimeWorker* worker_;
    mutable std::mutex turn_mutex_;
    mutable std::mutex turn_state_mutex_;
    mutable std::mutex operation_mutex_;
    std::condition_variable operation_finished_;
    std::size_t active_operations_{0};
    std::atomic<bool> active_turn_{false};
    std::atomic<bool> cancel_active_turn_{false};

#ifdef AI_FILE_SORTER_TEST_BUILD
    mutable std::mutex test_hook_mutex_;
    std::function<void()> test_run_turn_unwind_hook_;
    std::function<void()> test_destructor_before_shutdown_hook_;

    void invoke_test_run_turn_unwind_hook();
    void invoke_test_destructor_before_shutdown_hook();
#endif
};
