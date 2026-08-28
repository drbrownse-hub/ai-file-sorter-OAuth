#include "CodexRuntimeService.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

struct CodexRuntimeSharedState {
    mutable std::mutex mutex;
    CodexRuntimeSnapshot snapshot;
#ifdef AI_FILE_SORTER_TEST_BUILD
    mutable std::mutex test_hook_mutex;
    std::function<void()> active_turn_hook;
    std::function<void(bool)> worker_destruction_hook;
#endif
};

namespace {

constexpr auto operation_timeout = std::chrono::seconds(5);
constexpr auto probe_start_timeout = std::chrono::seconds(2);
constexpr auto probe_finish_timeout = std::chrono::seconds(3);
constexpr int unsupported_method_error_code = -32601;

void update_snapshot(const std::shared_ptr<CodexRuntimeSharedState>& state,
                     const std::function<void(CodexRuntimeSnapshot&)>& update)
{
    std::lock_guard lock(state->mutex);
    update(state->snapshot);
}

bool is_unsupported_method_error(const CodexError& error)
{
    return error.kind() == CodexErrorKind::ProtocolError &&
           error.protocol_code() == unsupported_method_error_code;
}

std::string value_string(const Json::Value& value, const char* key)
{
    return value[key].isString() ? value[key].asString() : std::string();
}

std::optional<std::pair<std::filesystem::path, std::string>> probe_executable(
    const std::filesystem::path& candidate)
{
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(candidate, filesystem_error)) {
        return std::nullopt;
    }

    QProcess process;
    process.setProgram(QString::fromStdString(candidate.string()));
    process.setArguments({QStringLiteral("--version")});
    process.start();
    if (!process.waitForStarted(static_cast<int>(probe_start_timeout.count() * 1000))) {
        return std::nullopt;
    }
    if (!process.waitForFinished(static_cast<int>(probe_finish_timeout.count() * 1000))) {
        process.kill();
        process.waitForFinished(1000);
        return std::nullopt;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return std::nullopt;
    }

    const std::string version = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed().toStdString();
    if (version.empty()) {
        return std::nullopt;
    }
    return std::make_pair(candidate, version);
}

std::vector<std::filesystem::path> executable_candidates(const CodexRuntimeConfig& config)
{
    std::vector<std::filesystem::path> candidates;
    auto append_if_nonempty = [&candidates](const std::filesystem::path& path) {
        if (!path.empty()) {
            candidates.push_back(path);
        }
    };
    auto append_from_directory = [&append_if_nonempty](const std::filesystem::path& directory) {
        if (directory.empty()) {
            return;
        }
        append_if_nonempty(directory / "codex.exe");
        append_if_nonempty(directory / "codex");
    };

    append_if_nonempty(config.executable_path);
    const std::filesystem::path application_directory =
        config.application_directory.empty()
            ? std::filesystem::path(QCoreApplication::applicationDirPath().toStdString())
            : std::filesystem::path(config.application_directory);
    append_from_directory(application_directory);

    std::vector<std::string> path_entries = config.path_entries;
    if (path_entries.empty()) {
        const QString path = QProcessEnvironment::systemEnvironment().value(QStringLiteral("PATH"));
        path_entries.reserve(path.split(QDir::listSeparator(), Qt::SkipEmptyParts).size());
        for (const QString& entry : path.split(QDir::listSeparator(), Qt::SkipEmptyParts)) {
            path_entries.push_back(entry.toStdString());
        }
    }
    for (const std::string& entry : path_entries) {
        append_from_directory(entry);
    }
    return candidates;
}

std::optional<CodexModelInfo> find_model(const std::vector<CodexModelInfo>& models, std::string_view id)
{
    const auto iterator = std::find_if(models.begin(), models.end(), [id](const CodexModelInfo& model) {
        return model.id == id;
    });
    if (iterator == models.end()) {
        return std::nullopt;
    }
    return *iterator;
}

} // namespace

class CodexRuntimeWorker final : public QObject {
    Q_OBJECT

public:
    explicit CodexRuntimeWorker(std::shared_ptr<CodexRuntimeSharedState> state)
        : state_(std::move(state))
    {
    }

    ~CodexRuntimeWorker() override
    {
#ifdef AI_FILE_SORTER_TEST_BUILD
        std::function<void(bool)> hook;
        {
            std::lock_guard lock(state_->test_hook_mutex);
            hook = state_->worker_destruction_hook;
        }
        if (hook) {
            hook(QThread::currentThread() == thread());
        }
#endif
    }

    void enqueue(std::function<void()> operation)
    {
        Q_ASSERT(QThread::currentThread() == thread());
        pending_operations_.push_back(std::move(operation));
        dispatch_next();
    }

    bool operation_active() const
    {
        Q_ASSERT(QThread::currentThread() == thread());
        return operation_active_;
    }

    void configure(CodexRuntimeConfig config)
    {
        Q_ASSERT(QThread::currentThread() == thread());
        ++configuration_generation_;
        if (app_server_) {
            app_server_->stop();
            app_server_.reset();
        }
        config_ = std::move(config);
        executable_.reset();
        started_once_ = false;
        restart_attempted_ = false;
        pending_account_refresh_ = false;
        pending_account_error_.clear();
        rate_limits_read_supported_ = true;
        update_snapshot(state_, [](CodexRuntimeSnapshot& snapshot) {
            snapshot = {};
        });
        emit stateChanged();
    }

    void configure_executable_path(std::string executable_path)
    {
        Q_ASSERT(QThread::currentThread() == thread());
        if (config_.executable_path == executable_path) {
            return;
        }
        ++configuration_generation_;
        if (app_server_) {
            app_server_->stop();
            app_server_.reset();
        }
        config_.executable_path = std::move(executable_path);
        executable_.reset();
        started_once_ = false;
        restart_attempted_ = false;
        pending_account_refresh_ = false;
        pending_account_error_.clear();
        rate_limits_read_supported_ = true;
        update_snapshot(state_, [](CodexRuntimeSnapshot& snapshot) {
            snapshot = {};
        });
        emit stateChanged();
    }

    void shutdown()
    {
        Q_ASSERT(QThread::currentThread() == thread());
        if (app_server_) {
            app_server_->stop();
            app_server_.reset();
        }
        started_once_ = false;
        restart_attempted_ = false;
        pending_account_refresh_ = false;
        pending_account_error_.clear();
        rate_limits_read_supported_ = true;
        update_snapshot(state_, [](CodexRuntimeSnapshot& snapshot) { snapshot.running = false; });
        emit stateChanged();
    }

    void start_or_refresh()
    {
        Q_ASSERT(QThread::currentThread() == thread());
        try {
            ensure_running();
            refresh_account_and_models();
        } catch (const CodexError& error) {
            record_error(error);
        }
    }

    void begin_chatgpt_login()
    {
        Q_ASSERT(QThread::currentThread() == thread());
        begin_login(CodexProtocol::make_chatgpt_login_params(), false);
    }

    void begin_device_code_login()
    {
        Q_ASSERT(QThread::currentThread() == thread());
        begin_login(CodexProtocol::make_chatgpt_device_code_login_params(), true);
    }

    void logout()
    {
        Q_ASSERT(QThread::currentThread() == thread());
        try {
            ensure_running();
            app_server_->request("account/logout", Json::Value(Json::objectValue), operation_timeout);
            refresh_account_and_models();
        } catch (const CodexError& error) {
            record_error(error);
        }
    }

    CodexTurnResult run_turn(const CodexTurnRequest& request,
                             const std::function<bool()>& cancelled)
    {
        Q_ASSERT(QThread::currentThread() == thread());
        try {
            const bool restarted = ensure_running();
            if (restarted) {
                refresh_account_and_models();
            } else {
                refresh_if_needed();
            }
            CodexRuntimeSnapshot current = read_snapshot();
            if (!current.authenticated) {
                throw CodexError(CodexErrorKind::AuthenticationRequired,
                                 "Sign in with ChatGPT before using the Codex runtime.");
            }

            CodexTurnRequest effective = request;
            effective.config.inference_cwd = inference_cwd_.string();
            if (effective.config.model.empty()) {
                effective.config.model = config_.model;
            }
            if (!effective.config.model.empty() && !find_model(current.models, effective.config.model)) {
                update_snapshot(state_, [this](CodexRuntimeSnapshot& snapshot) {
                    snapshot.last_error = "Configured Codex model is unavailable; using the current default model.";
                });
                emit stateChanged();
                effective.config.model.clear();
            }
            if (effective.config.model.empty()) {
                const auto default_model = std::find_if(
                    current.models.begin(), current.models.end(),
                    [](const CodexModelInfo& model) { return model.is_default && !model.id.empty(); });
                if (default_model == current.models.end()) {
                    throw CodexError(CodexErrorKind::ModelUnavailable,
                                     "Codex model catalog has no usable default model.");
                }
                effective.config.model = default_model->id;
            }
            const CodexTurnResult result = app_server_->run_ephemeral_turn(effective, operation_timeout, cancelled);
            if (result.completion.status != CodexTurnStatus::Completed) {
                throw CodexError(CodexErrorKind::TurnFailed,
                                 result.completion.error_message.empty()
                                     ? "Codex turn did not complete successfully."
                                     : result.completion.error_message);
            }
            update_snapshot(state_, [](CodexRuntimeSnapshot& snapshot) {
                snapshot.running = true;
            });
            emit stateChanged();
            return result;
        } catch (const CodexError& error) {
            record_error(error);
            throw;
        }
    }

signals:
    void stateChanged();
    void loginUrlReady(const QUrl& url);
    void deviceCodeReady(const QUrl& verification_url, const QString& user_code);

private:
    CodexRuntimeSnapshot read_snapshot() const
    {
        std::lock_guard lock(state_->mutex);
        return state_->snapshot;
    }

    void publish_running(bool running)
    {
        update_snapshot(state_, [running](CodexRuntimeSnapshot& snapshot) { snapshot.running = running; });
        emit stateChanged();
    }

    void record_error(const CodexError& error)
    {
        const bool running = app_server_ && app_server_->is_running();
        update_snapshot(state_, [&error, running](CodexRuntimeSnapshot& snapshot) {
            snapshot.running = running;
            snapshot.last_error = error.what();
            if (error.kind() == CodexErrorKind::RuntimeNotFound) {
                snapshot.runtime_found = false;
            }
        });
        emit stateChanged();
    }

    void ensure_runtime_directories()
    {
        std::filesystem::path codex_home = config_.codex_home.empty()
                                               ? std::filesystem::path(
                                                     QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                                         .toStdString()) /
                                                     "codex"
                                               : std::filesystem::path(config_.codex_home);
        inference_cwd_ = codex_home / "inference";
        std::error_code error;
        std::filesystem::create_directories(codex_home, error);
        if (error) {
            throw CodexError(CodexErrorKind::StartupFailed,
                             "Could not create the isolated Codex home: " + error.message());
        }
        error.clear();
        std::filesystem::create_directories(inference_cwd_, error);
        if (error) {
            throw CodexError(CodexErrorKind::StartupFailed,
                             "Could not create the Codex inference directory: " + error.message());
        }
        codex_home_ = std::move(codex_home);
    }

    void discover_runtime()
    {
        for (const auto& candidate : executable_candidates(config_)) {
            const auto probe = probe_executable(candidate);
            if (!probe) {
                continue;
            }
            executable_ = probe->first;
            runtime_version_ = probe->second;
            update_snapshot(state_, [this](CodexRuntimeSnapshot& snapshot) {
                snapshot.runtime_found = true;
                snapshot.runtime_version = runtime_version_;
                snapshot.last_error.clear();
            });
            emit stateChanged();
            return;
        }
        update_snapshot(state_, [](CodexRuntimeSnapshot& snapshot) {
            snapshot.runtime_found = false;
            snapshot.running = false;
            snapshot.last_error = "Codex runtime not found.";
        });
        emit stateChanged();
        throw CodexError(CodexErrorKind::RuntimeNotFound, "Codex runtime not found.");
    }

    void create_server_if_needed()
    {
        if (!app_server_) {
            app_server_ = std::make_unique<CodexAppServer>();
            connect(app_server_.get(), &CodexAppServer::transportStateChanged, this,
                    &CodexRuntimeWorker::on_transport_state_changed);
            connect(app_server_.get(), &CodexAppServer::accountNotificationReceived, this,
                    &CodexRuntimeWorker::on_account_notification);
#ifdef AI_FILE_SORTER_TEST_BUILD
            app_server_->set_test_active_turn_hook([this] {
                std::function<void()> hook;
                {
                    std::lock_guard lock(state_->test_hook_mutex);
                    hook = state_->active_turn_hook;
                }
                if (hook) {
                    hook();
                }
            });
#endif
        }
    }

    bool ensure_running()
    {
        if (!executable_) {
            discover_runtime();
        }
        ensure_runtime_directories();
        create_server_if_needed();
        if (app_server_->is_running()) {
            publish_running(true);
            return false;
        }
        if (started_once_ && restart_attempted_) {
            throw CodexError(CodexErrorKind::ProcessCrashed,
                             "Codex app-server restart limit reached; runtime is disconnected.");
        }
        if (started_once_) {
            restart_attempted_ = true;
        }
        // Reserve the single restart budget before starting the process. A child
        // can exit during the initialize handshake, before start() returns.
        started_once_ = true;

        CodexLaunchConfig launch;
        launch.executable_path = executable_->string();
        launch.codex_home = codex_home_.string();
        launch.client_version = config_.client_version.empty() ? "ai-file-sorter" : config_.client_version;
        app_server_->start(launch);
        publish_running(true);
        return true;
    }

    void refresh_if_needed()
    {
        const CodexRuntimeSnapshot current = read_snapshot();
        if (!current.authenticated || current.models.empty()) {
            refresh_account_and_models();
        }
    }

    void refresh_account_and_models()
    {
        const Json::Value account_result =
            app_server_->request("account/read", Json::Value(Json::objectValue), operation_timeout);
        const CodexAccountInfo account = CodexProtocol::parse_account_read_response(account_result);
        std::vector<CodexModelInfo> models;
        Json::Value rate_limits;
        bool rate_limits_refreshed = false;
        if (account.authenticated) {
            const Json::Value model_result =
                app_server_->request("model/list", Json::Value(Json::objectValue), operation_timeout);
            models = CodexProtocol::parse_model_list_response(model_result);
            if (rate_limits_read_supported_) {
                try {
                    const Json::Value rate_limits_result =
                        app_server_->request("account/rateLimits/read", Json::Value(Json::objectValue), operation_timeout);
                    rate_limits = CodexProtocol::parse_account_rate_limits_read_response(rate_limits_result);
                    rate_limits_refreshed = true;
                } catch (const CodexError& error) {
                    if (!is_unsupported_method_error(error)) {
                        throw;
                    }
                    rate_limits_read_supported_ = false;
                }
            }
        }
        update_snapshot(state_, [&account, &models, &rate_limits, rate_limits_refreshed](CodexRuntimeSnapshot& snapshot) {
            snapshot.account = account;
            snapshot.authenticated = account.authenticated;
            if (!account.authenticated) {
                snapshot.rate_limits = Json::Value();
            } else if (rate_limits_refreshed) {
                snapshot.rate_limits = rate_limits;
            }
            snapshot.models = models;
            snapshot.running = true;
            snapshot.last_error.clear();
        });
        emit stateChanged();
    }

    void begin_login(const Json::Value& params, bool device_code)
    {
        try {
            ensure_running();
            const Json::Value result =
                app_server_->request("account/login/start", params, operation_timeout);
            if (device_code) {
                const QUrl verification_url(QString::fromStdString(value_string(result, "verificationUrl")));
                const QString user_code = QString::fromStdString(value_string(result, "userCode"));
                if (!verification_url.isValid() || user_code.isEmpty()) {
                    throw CodexError(CodexErrorKind::ProtocolError,
                                     "Codex device login response is missing verification details.");
                }
                emit deviceCodeReady(verification_url, user_code);
            } else {
                const QUrl login_url(QString::fromStdString(value_string(result, "authUrl")));
                if (!login_url.isValid()) {
                    throw CodexError(CodexErrorKind::ProtocolError,
                                     "Codex login response is missing an authentication URL.");
                }
                emit loginUrlReady(login_url);
            }
        } catch (const CodexError& error) {
            record_error(error);
        }
    }

    void on_account_notification()
    {
        Q_ASSERT(QThread::currentThread() == thread());
        if (!app_server_) {
            return;
        }
        const auto notification = app_server_->take_account_notification();
        if (!notification) {
            return;
        }
        if (notification->kind == CodexAccountNotificationKind::RateLimitsUpdated) {
            update_snapshot(state_, [&notification](CodexRuntimeSnapshot& snapshot) {
                snapshot.rate_limits = notification->rate_limits;
            });
            emit stateChanged();
            return;
        }
        pending_account_refresh_ = true;
        if (notification->kind == CodexAccountNotificationKind::LoginCompleted && !notification->success) {
            pending_account_error_ = notification->error_message.empty()
                                         ? "Codex ChatGPT login did not complete."
                                         : "Codex ChatGPT login failed: " + notification->error_message;
        }
        schedule_account_refresh();
    }

    void dispatch_next()
    {
        Q_ASSERT(QThread::currentThread() == thread());
        if (operation_active_ || pending_operations_.empty()) {
            return;
        }
        operation_active_ = true;
        std::function<void()> operation = std::move(pending_operations_.front());
        pending_operations_.pop_front();
        operation();
        operation_active_ = false;
        schedule_account_refresh();
        if (!pending_operations_.empty()) {
            QTimer::singleShot(0, this, [this] { dispatch_next(); });
        }
    }

    void schedule_account_refresh()
    {
        if (operation_active_ || !pending_account_refresh_ || refresh_scheduled_) {
            return;
        }
        const std::size_t refresh_generation = configuration_generation_;
        refresh_scheduled_ = true;
        QTimer::singleShot(0, this, [this, refresh_generation] {
            refresh_scheduled_ = false;
            if (refresh_generation != configuration_generation_) {
                schedule_account_refresh();
                return;
            }
            if (!pending_account_refresh_) {
                return;
            }
            pending_account_refresh_ = false;
            const std::string account_error = std::move(pending_account_error_);
            pending_account_error_.clear();
            enqueue([this, refresh_generation, account_error] {
                if (refresh_generation != configuration_generation_ || !app_server_) {
                    return;
                }
                try {
                    refresh_account_and_models();
                    if (!account_error.empty()) {
                        update_snapshot(state_, [&account_error](CodexRuntimeSnapshot& snapshot) {
                            snapshot.last_error = account_error;
                        });
                        emit stateChanged();
                    }
                } catch (const CodexError& error) {
                    record_error(error);
                }
            });
        });
    }

    void on_transport_state_changed()
    {
        Q_ASSERT(QThread::currentThread() == thread());
        if (!app_server_) {
            return;
        }
        const bool running = app_server_->is_running();
        update_snapshot(state_, [running](CodexRuntimeSnapshot& snapshot) {
            snapshot.running = running;
            if (!running && snapshot.last_error.empty()) {
                snapshot.last_error = "Codex app-server is disconnected.";
            }
        });
        emit stateChanged();
    }

    std::shared_ptr<CodexRuntimeSharedState> state_;
    CodexRuntimeConfig config_;
    std::optional<std::filesystem::path> executable_;
    std::filesystem::path codex_home_;
    std::filesystem::path inference_cwd_;
    std::string runtime_version_;
    std::unique_ptr<CodexAppServer> app_server_;
    std::deque<std::function<void()>> pending_operations_;
    bool operation_active_{false};
    bool pending_account_refresh_{false};
    std::string pending_account_error_;
    bool refresh_scheduled_{false};
    bool rate_limits_read_supported_{true};
    bool started_once_{false};
    bool restart_attempted_{false};
    std::size_t configuration_generation_{0};
};

CodexRuntimeService::CodexRuntimeService(QObject* parent)
    : QObject(parent), state_(std::make_shared<CodexRuntimeSharedState>()), worker_thread_(new QThread(this)),
      worker_(new CodexRuntimeWorker(state_))
{
    worker_->moveToThread(worker_thread_);
    connect(worker_, &CodexRuntimeWorker::stateChanged, this, &CodexRuntimeService::stateChanged,
            Qt::QueuedConnection);
    connect(worker_, &CodexRuntimeWorker::loginUrlReady, this, &CodexRuntimeService::loginUrlReady,
            Qt::QueuedConnection);
    connect(worker_, &CodexRuntimeWorker::deviceCodeReady, this, &CodexRuntimeService::deviceCodeReady,
            Qt::QueuedConnection);
    connect(worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);
    worker_thread_->start();
}

CodexRuntimeService::~CodexRuntimeService()
{
    if (!worker_) {
        return;
    }
    cancel_active_turn_async();
    std::unique_lock turn_lock(turn_mutex_);
    wait_for_operations();
#ifdef AI_FILE_SORTER_TEST_BUILD
    invoke_test_destructor_before_shutdown_hook();
#endif
    std::mutex completion_mutex;
    std::condition_variable completion_finished;
    bool complete = false;
    auto shutdown = [&] {
        worker_->shutdown();
        {
            std::lock_guard lock(completion_mutex);
            complete = true;
        }
        completion_finished.notify_one();
    };
    if (QThread::currentThread() == worker_->thread()) {
        if (!worker_->operation_active()) {
            worker_->enqueue(shutdown);
        }
    } else {
        QMetaObject::invokeMethod(worker_, [worker = worker_, shutdown] {
            worker->enqueue(shutdown);
        }, Qt::QueuedConnection);
    }
    if (QThread::currentThread() != worker_->thread()) {
        std::unique_lock lock(completion_mutex);
        completion_finished.wait(lock, [&] { return complete; });
    }
    worker_thread_->quit();
    worker_thread_->wait();
    worker_ = nullptr;
}

void CodexRuntimeService::configure(CodexRuntimeConfig config)
{
    begin_operation();
    std::mutex completion_mutex;
    std::condition_variable completion_finished;
    bool complete = false;
    std::exception_ptr failure;
    auto operation = [&, config = std::move(config), worker = worker_]() mutable {
        try {
            worker->configure(std::move(config));
        } catch (...) {
            failure = std::current_exception();
        }
        {
            std::lock_guard lock(completion_mutex);
            complete = true;
        }
        completion_finished.notify_one();
    };
    if (QThread::currentThread() == worker_->thread()) {
        if (worker_->operation_active()) {
            end_operation();
            throw CodexError(CodexErrorKind::StartupFailed,
                             "Cannot reconfigure the Codex runtime during an active operation.");
        }
        worker_->enqueue(std::move(operation));
    } else if (!QMetaObject::invokeMethod(worker_, [worker = worker_, operation = std::move(operation)]() mutable {
                   worker->enqueue(std::move(operation));
               }, Qt::QueuedConnection)) {
        end_operation();
        throw CodexError(CodexErrorKind::StartupFailed, "Codex runtime worker is unavailable.");
    }
    {
        std::unique_lock lock(completion_mutex);
        completion_finished.wait(lock, [&] { return complete; });
    }
    end_operation();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void CodexRuntimeService::configure_executable_path_async(std::string executable_path)
{
    begin_operation();
    auto operation = [this, executable_path = std::move(executable_path)]() mutable {
        worker_->configure_executable_path(std::move(executable_path));
        end_operation();
    };
    if (!QMetaObject::invokeMethod(worker_, [worker = worker_, operation = std::move(operation)]() mutable {
            worker->enqueue(std::move(operation));
        }, Qt::QueuedConnection)) {
        end_operation();
    }
}

CodexRuntimeSnapshot CodexRuntimeService::snapshot() const
{
    std::lock_guard lock(state_->mutex);
    return state_->snapshot;
}

void CodexRuntimeService::start_or_refresh_async()
{
    begin_operation();
    auto operation = [this, worker = worker_] {
        worker->start_or_refresh();
        end_operation();
    };
    if (!QMetaObject::invokeMethod(worker_, [worker = worker_, operation = std::move(operation)]() mutable {
            worker->enqueue(std::move(operation));
        }, Qt::QueuedConnection)) {
        end_operation();
    }
}

void CodexRuntimeService::cancel_active_turn_async() noexcept
{
    std::lock_guard lock(turn_state_mutex_);
    if (active_turn_.load(std::memory_order_acquire)) {
        cancel_active_turn_.store(true, std::memory_order_release);
    }
}

#ifdef AI_FILE_SORTER_TEST_BUILD
void CodexRuntimeService::set_test_run_turn_unwind_hook(std::function<void()> hook)
{
    std::lock_guard lock(test_hook_mutex_);
    test_run_turn_unwind_hook_ = std::move(hook);
}

void CodexRuntimeService::set_test_destructor_before_shutdown_hook(std::function<void()> hook)
{
    std::lock_guard lock(test_hook_mutex_);
    test_destructor_before_shutdown_hook_ = std::move(hook);
}

void CodexRuntimeService::set_test_active_turn_hook(std::function<void()> hook)
{
    std::lock_guard lock(state_->test_hook_mutex);
    state_->active_turn_hook = std::move(hook);
}

void CodexRuntimeService::set_test_worker_destruction_hook(std::function<void(bool)> hook)
{
    std::lock_guard lock(state_->test_hook_mutex);
    state_->worker_destruction_hook = std::move(hook);
}

void CodexRuntimeService::invoke_test_run_turn_unwind_hook()
{
    std::function<void()> hook;
    {
        std::lock_guard lock(test_hook_mutex_);
        hook = test_run_turn_unwind_hook_;
    }
    if (hook) {
        hook();
    }
}

void CodexRuntimeService::invoke_test_destructor_before_shutdown_hook()
{
    std::function<void()> hook;
    {
        std::lock_guard lock(test_hook_mutex_);
        hook = test_destructor_before_shutdown_hook_;
    }
    if (hook) {
        hook();
    }
}
#endif

void CodexRuntimeService::begin_chatgpt_login_async()
{
    begin_operation();
    auto operation = [this, worker = worker_] {
        worker->begin_chatgpt_login();
        end_operation();
    };
    if (!QMetaObject::invokeMethod(worker_, [worker = worker_, operation = std::move(operation)]() mutable {
            worker->enqueue(std::move(operation));
        }, Qt::QueuedConnection)) {
        end_operation();
    }
}

void CodexRuntimeService::begin_device_code_login_async()
{
    begin_operation();
    auto operation = [this, worker = worker_] {
        worker->begin_device_code_login();
        end_operation();
    };
    if (!QMetaObject::invokeMethod(worker_, [worker = worker_, operation = std::move(operation)]() mutable {
            worker->enqueue(std::move(operation));
        }, Qt::QueuedConnection)) {
        end_operation();
    }
}

void CodexRuntimeService::logout_async()
{
    begin_operation();
    auto operation = [this, worker = worker_] {
        worker->logout();
        end_operation();
    };
    if (!QMetaObject::invokeMethod(worker_, [worker = worker_, operation = std::move(operation)]() mutable {
            worker->enqueue(std::move(operation));
        }, Qt::QueuedConnection)) {
        end_operation();
    }
}

CodexTurnResult CodexRuntimeService::run_turn(const CodexTurnRequest& request,
                                              const std::function<bool()>& cancelled)
{
    std::lock_guard lock(turn_mutex_);
    {
        std::lock_guard turn_state_lock(turn_state_mutex_);
        cancel_active_turn_.store(false, std::memory_order_release);
        active_turn_.store(true, std::memory_order_release);
    }
    const std::unique_ptr<void, std::function<void(void*)>> turn_guard(
        reinterpret_cast<void*>(1), [this](void*) noexcept {
            std::lock_guard lock(turn_state_mutex_);
            cancel_active_turn_.store(false, std::memory_order_release);
            active_turn_.store(false, std::memory_order_release);
        });
    begin_operation();
    std::mutex completion_mutex;
    std::condition_variable completion_finished;
    bool complete = false;
    CodexTurnResult result;
    std::exception_ptr failure;
    const auto combined_cancelled = [this, &cancelled] {
        return cancel_active_turn_.load(std::memory_order_acquire) || (cancelled && cancelled());
    };
    auto operation = [&] {
        try {
            result = worker_->run_turn(request, combined_cancelled);
        } catch (...) {
            failure = std::current_exception();
        }
        {
            std::lock_guard completion_lock(completion_mutex);
            complete = true;
        }
        completion_finished.notify_one();
    };

    if (QThread::currentThread() == worker_->thread()) {
        if (worker_->operation_active()) {
            end_operation();
            throw CodexError(CodexErrorKind::ProtocolError,
                             "Cannot synchronously invoke the Codex runtime during an active operation.");
        }
        worker_->enqueue(std::move(operation));
    } else if (!QMetaObject::invokeMethod(worker_, [worker = worker_, operation = std::move(operation)]() mutable {
                   worker->enqueue(std::move(operation));
               }, Qt::QueuedConnection)) {
        end_operation();
        throw CodexError(CodexErrorKind::StartupFailed, "Codex runtime worker is unavailable.");
    }
    {
        std::unique_lock completion_lock(completion_mutex);
        completion_finished.wait(completion_lock, [&] { return complete; });
    }
    end_operation();
#ifdef AI_FILE_SORTER_TEST_BUILD
    invoke_test_run_turn_unwind_hook();
#endif
    if (failure) {
        std::rethrow_exception(failure);
    }
    return result;
}

bool CodexRuntimeService::selected_model_accepts_images(std::string_view configured_model) const
{
    std::lock_guard lock(state_->mutex);
    if (!state_->snapshot.running || !state_->snapshot.authenticated) {
        return false;
    }
    const auto default_model = std::find_if(state_->snapshot.models.begin(), state_->snapshot.models.end(),
                                            [](const CodexModelInfo& model) { return model.is_default; });
    if (configured_model.empty()) {
        return default_model != state_->snapshot.models.end() && default_model->accepts_image;
    }
    const auto model = find_model(state_->snapshot.models, configured_model);
    if (!model) {
        return default_model != state_->snapshot.models.end() && default_model->accepts_image;
    }
    return model && model->accepts_image;
}

void CodexRuntimeService::begin_operation()
{
    std::lock_guard lock(operation_mutex_);
    ++active_operations_;
}

void CodexRuntimeService::end_operation()
{
    std::lock_guard lock(operation_mutex_);
    if (active_operations_ == 0) {
        return;
    }
    --active_operations_;
    if (active_operations_ == 0) {
        operation_finished_.notify_all();
    }
}

void CodexRuntimeService::wait_for_operations()
{
    std::unique_lock lock(operation_mutex_);
    operation_finished_.wait(lock, [this] { return active_operations_ == 0; });
}

#include "CodexRuntimeService.moc"
