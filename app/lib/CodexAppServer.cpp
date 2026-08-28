#include "CodexAppServer.hpp"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

namespace {

constexpr auto startup_timeout = std::chrono::seconds(5);

std::string string_member(const Json::Value& value, const char* name)
{
    return value[name].isString() ? value[name].asString() : std::string();
}

int timer_interval(std::chrono::milliseconds duration)
{
    return static_cast<int>(std::min<std::chrono::milliseconds::rep>(duration.count(),
                                                                     std::numeric_limits<int>::max()));
}

bool notification_matches_thread(const Json::Value& notification, std::string_view thread_id)
{
    return notification["params"]["threadId"].isString() &&
           notification["params"]["threadId"].asString() == thread_id;
}

} // namespace

CodexAppServer::CodexAppServer(QObject* parent)
    : QObject(parent), process_(new QProcess(this))
{
    connect(process_, &QProcess::readyReadStandardOutput, this, &CodexAppServer::read_stdout);
    connect(process_, &QProcess::readyReadStandardError, this, &CodexAppServer::read_stderr);
    connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exit_code, QProcess::ExitStatus exit_status) {
                read_stderr();
                running_ = false;
                initializing_ = false;
                initialized_ = false;

                if (!stopping_ && !fatal_error_kind_) {
                    std::ostringstream message;
                    message << "Codex app-server exited unexpectedly (exit code " << exit_code;
                    if (exit_status == QProcess::CrashExit) {
                        message << ", crashed";
                    }
                    message << ").";
                    if (!stderr_buffer_.isEmpty()) {
                        message << " " << stderr_buffer_.toStdString();
                    }
                    set_fatal_error(CodexErrorKind::ProcessCrashed, message.str());
                } else {
                    emit transportStateChanged();
                }
            });
}

CodexAppServer::~CodexAppServer()
{
    if (QThread::currentThread() == thread()) {
        stop();
    }
}

void CodexAppServer::start(const CodexLaunchConfig& config)
{
    ensure_owner_thread();
    if (running_) {
        throw CodexError(CodexErrorKind::StartupFailed, "Codex app-server is already running.");
    }
    if (config.executable_path.empty()) {
        throw CodexError(CodexErrorKind::RuntimeNotFound, "Codex executable path is empty.");
    }

    stdout_buffer_.clear();
    stderr_buffer_.clear();
    responses_.clear();
    notifications_.clear();
    pending_turn_notifications_.clear();
    pending_turn_thread_id_.clear();
    last_account_notification_.reset();
    active_turn_.reset();
    fatal_error_kind_.reset();
    fatal_error_message_.clear();
    initialized_ = false;
    initializing_ = false;
    stopping_ = false;

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("CODEX_HOME"), QString::fromStdString(config.codex_home));
    process_->setProcessEnvironment(environment);
    process_->setProgram(QString::fromStdString(config.executable_path));
    process_->setArguments({QStringLiteral("app-server")});
    process_->start();

    if (!process_->waitForStarted(timer_interval(startup_timeout))) {
        throw CodexError(CodexErrorKind::StartupFailed,
                         "Could not start Codex app-server: " + process_->errorString().toStdString());
    }
    running_ = true;
    initializing_ = true;

    try {
        static_cast<void>(request_internal("initialize", CodexProtocol::make_initialize_params(config.client_version),
                                           startup_timeout, {}, true));
        notify_internal("initialized", Json::Value(Json::objectValue), true);
        initialized_ = true;
        initializing_ = false;
        emit transportStateChanged();
    } catch (...) {
        stop();
        throw;
    }
}

void CodexAppServer::stop()
{
    ensure_owner_thread();
    active_turn_.reset();
    responses_.clear();
    notifications_.clear();
    pending_turn_notifications_.clear();
    pending_turn_thread_id_.clear();
    last_account_notification_.reset();
    initializing_ = false;
    initialized_ = false;

    if (process_->state() == QProcess::NotRunning) {
        running_ = false;
        stopping_ = false;
        return;
    }

    stopping_ = true;
    process_->closeWriteChannel();
    process_->terminate();
    if (!process_->waitForFinished(1000)) {
        process_->kill();
        process_->waitForFinished(1000);
    }
    running_ = false;
    stopping_ = false;
    responses_.clear();
    notifications_.clear();
    pending_turn_notifications_.clear();
    pending_turn_thread_id_.clear();
}

bool CodexAppServer::is_running() const
{
    ensure_owner_thread();
    return running_;
}

Json::Value CodexAppServer::request(std::string_view method,
                                    const Json::Value& params,
                                    std::chrono::milliseconds timeout)
{
    ensure_owner_thread();
    ensure_public_ready();
    if (method == "initialize" || method == "initialized") {
        throw CodexError(CodexErrorKind::ProtocolError,
                         "Codex handshake methods are reserved for transport initialization.");
    }
    return request_internal(method, params, timeout, {}, false);
}

Json::Value CodexAppServer::request_internal(std::string_view method,
                                             const Json::Value& params,
                                             std::chrono::milliseconds timeout,
                                             const std::function<bool()>& cancelled,
                                             bool allow_initializing)
{
    ensure_owner_thread();
    throw_if_unavailable();
    if (initializing_ && (!allow_initializing || method != "initialize")) {
        throw CodexError(CodexErrorKind::StartupFailed,
                         "Codex app-server is still completing its initialization handshake.");
    }

    const Json::Int64 id = next_request_id_++;
    Json::Value message(Json::objectValue);
    message["id"] = id;
    message["method"] = std::string(method);
    message["params"] = params;
    send_message(message);

    wait_until([this, id] { return responses_.contains(id); }, timeout, cancelled);

    Json::Value response = std::move(responses_.at(id));
    responses_.erase(id);
    return CodexProtocol::parse_response(response).result;
}

void CodexAppServer::notify(std::string_view method, const Json::Value& params)
{
    ensure_owner_thread();
    ensure_public_ready();
    if (method == "initialize" || method == "initialized") {
        throw CodexError(CodexErrorKind::ProtocolError,
                         "Codex handshake methods are reserved for transport initialization.");
    }
    notify_internal(method, params, false);
}

std::optional<CodexAccountNotification> CodexAppServer::take_account_notification()
{
    ensure_owner_thread();
    std::optional<CodexAccountNotification> notification = std::move(last_account_notification_);
    last_account_notification_.reset();
    return notification;
}

void CodexAppServer::notify_internal(std::string_view method,
                                     const Json::Value& params,
                                     bool allow_initializing)
{
    ensure_owner_thread();
    throw_if_unavailable();
    if (initializing_ && (!allow_initializing || method != "initialized")) {
        throw CodexError(CodexErrorKind::StartupFailed,
                         "Codex app-server is still completing its initialization handshake.");
    }

    Json::Value message(Json::objectValue);
    message["method"] = std::string(method);
    message["params"] = params;
    send_message(message);
}

CodexTurnResult CodexAppServer::run_ephemeral_turn(const CodexTurnRequest& turn_request,
                                                   std::chrono::milliseconds timeout,
                                                   const std::function<bool()>& cancelled)
{
    ensure_owner_thread();
    QElapsedTimer operation_timer;
    operation_timer.start();

    const auto remaining = [&] {
        return timeout - std::chrono::milliseconds(operation_timer.elapsed());
    };
    const auto check_cancelled = [&] {
        if (cancelled && cancelled()) {
            throw CodexError(CodexErrorKind::Cancelled, "Codex turn was cancelled.");
        }
    };

    Json::Value thread_response;
    try {
        check_cancelled();
        thread_response = request_internal(
            "thread/start", CodexProtocol::make_thread_start_params(turn_request.config), remaining(), cancelled,
            false);
    } catch (const CodexError& error) {
        retire_after_setup_failure(error.kind());
        throw;
    }
    const std::string thread_id = string_member(thread_response["thread"], "id");
    if (thread_id.empty()) {
        throw CodexError(CodexErrorKind::ProtocolError, "Codex thread/start response is missing a thread id.");
    }

    Json::Value turn_response;
    try {
        check_cancelled();
        notifications_.clear();
        pending_turn_notifications_.clear();
        pending_turn_thread_id_ = thread_id;
        turn_response = request_internal(
            "turn/start", CodexProtocol::make_turn_start_params(thread_id, turn_request.inputs, turn_request.output_schema),
            remaining(), cancelled, false);
    } catch (const CodexError& error) {
        pending_turn_notifications_.clear();
        pending_turn_thread_id_.clear();
        retire_after_setup_failure(error.kind());
        throw;
    }
    const std::string turn_id = string_member(turn_response["turn"], "id");
    if (turn_id.empty()) {
        throw CodexError(CodexErrorKind::ProtocolError, "Codex turn/start response is missing a turn id.");
    }

    active_turn_ = ActiveTurn{.thread_id = thread_id, .turn_id = turn_id};
#ifdef AI_FILE_SORTER_TEST_BUILD
    if (test_active_turn_hook_) {
        test_active_turn_hook_();
    }
#endif
    for (const Json::Value& notification : notifications_) {
        process_notification(notification);
    }
    for (const Json::Value& notification : pending_turn_notifications_) {
        process_notification(notification);
    }
    notifications_.clear();
    pending_turn_notifications_.clear();
    pending_turn_thread_id_.clear();

    try {
        wait_until([this] { return active_turn_ && active_turn_->completion.has_value(); }, remaining(), cancelled);
    } catch (const CodexError& error) {
        if ((error.kind() == CodexErrorKind::Cancelled || error.kind() == CodexErrorKind::Timeout) && running_) {
            interrupt_turn(thread_id, turn_id);
        }
        active_turn_.reset();
        throw;
    }

    CodexTurnResult result{
        .text = std::move(active_turn_->text),
        .completion = std::move(active_turn_->completion.value()),
    };
    active_turn_.reset();
    return result;
}

void CodexAppServer::interrupt_turn(std::string_view thread_id, std::string_view turn_id)
{
    Json::Value params(Json::objectValue);
    params["threadId"] = std::string(thread_id);
    params["turnId"] = std::string(turn_id);
    notify("turn/interrupt", params);
}

#ifdef AI_FILE_SORTER_TEST_BUILD
void CodexAppServer::set_test_write_chunk_limit(std::size_t limit)
{
    ensure_owner_thread();
    if (limit == 0) {
        test_write_chunk_limit_.reset();
    } else {
        test_write_chunk_limit_ = limit;
    }
}

void CodexAppServer::set_test_active_turn_hook(std::function<void()> hook)
{
    ensure_owner_thread();
    test_active_turn_hook_ = std::move(hook);
}
#endif

void CodexAppServer::ensure_owner_thread() const
{
    if (QThread::currentThread() != thread()) {
        throw CodexError(CodexErrorKind::ProtocolError,
                         "CodexAppServer may only be accessed from its owning Qt thread.");
    }
}

void CodexAppServer::send_message(const Json::Value& message)
{
    Json::StreamWriterBuilder writer_builder;
    writer_builder["indentation"] = "";
    const QByteArray record = QByteArray::fromStdString(Json::writeString(writer_builder, message)) + '\n';
    qsizetype offset = 0;
    while (offset < record.size()) {
        qint64 write_size = record.size() - offset;
#ifdef AI_FILE_SORTER_TEST_BUILD
        if (test_write_chunk_limit_) {
            write_size = std::min(write_size, static_cast<qint64>(*test_write_chunk_limit_));
        }
#endif
        const qint64 written = process_->write(record.constData() + offset, write_size);
        if (written <= 0) {
            set_fatal_error(CodexErrorKind::ProcessCrashed, "Could not write to the Codex app-server process.");
            throw_if_unavailable();
        }
        offset += static_cast<qsizetype>(written);
    }
}

void CodexAppServer::read_stdout()
{
    ensure_owner_thread();
    stdout_buffer_ += process_->readAllStandardOutput();

    qsizetype newline = stdout_buffer_.indexOf('\n');
    while (newline >= 0) {
        QByteArray record = stdout_buffer_.left(newline);
        stdout_buffer_.remove(0, newline + 1);
        if (record.endsWith('\r')) {
            record.chop(1);
        }
        if (!record.isEmpty()) {
            process_stdout_record(record);
        }
        newline = stdout_buffer_.indexOf('\n');
    }
}

void CodexAppServer::read_stderr()
{
    ensure_owner_thread();
    stderr_buffer_ += process_->readAllStandardError();
}

void CodexAppServer::process_stdout_record(const QByteArray& record)
{
    Json::CharReaderBuilder reader_builder;
    Json::Value message;
    std::string errors;
    std::istringstream stream(record.toStdString());
    if (!Json::parseFromStream(reader_builder, stream, &message, &errors) || !message.isObject()) {
        set_fatal_error(CodexErrorKind::ProtocolError,
                        "Codex app-server emitted malformed JSON: " + errors);
        process_->kill();
        return;
    }

    if (message.isMember("id")) {
        if (!message["id"].isIntegral()) {
            set_fatal_error(CodexErrorKind::ProtocolError,
                            "Codex app-server response has a non-integer request id.");
            process_->kill();
            return;
        }
        responses_[message["id"].asInt64()] = std::move(message);
        emit transportStateChanged();
        return;
    }

    if (message["method"].isString()) {
        if (const auto account_notification = CodexProtocol::parse_account_notification(message)) {
            last_account_notification_ = *account_notification;
            emit accountNotificationReceived();
        }
        if (active_turn_) {
            process_notification(message);
        } else if (!pending_turn_thread_id_.empty() && notification_matches_thread(message, pending_turn_thread_id_)) {
            pending_turn_notifications_.push_back(std::move(message));
        } else {
            notifications_.push_back(std::move(message));
            if (notifications_.size() > max_pending_notifications_) {
                notifications_.pop_front();
            }
        }
        emit notificationReceived();
        emit transportStateChanged();
        return;
    }

    set_fatal_error(CodexErrorKind::ProtocolError,
                    "Codex app-server message is neither a response nor a notification.");
    process_->kill();
}

void CodexAppServer::process_notification(const Json::Value& notification)
{
    if (!active_turn_) {
        return;
    }

    try {
        if (CodexProtocol::append_agent_message_delta(notification, active_turn_->thread_id, active_turn_->turn_id,
                                                       active_turn_->text)) {
            return;
        }

        const std::optional<CodexTurnCompletion> completion =
            CodexProtocol::parse_turn_completed_notification(notification);
        if (completion && completion->thread_id == active_turn_->thread_id &&
            completion->turn_id == active_turn_->turn_id) {
            active_turn_->completion = completion;
        }
    } catch (const CodexError& error) {
        set_fatal_error(error.kind(), error.what());
        process_->kill();
    }
}

void CodexAppServer::set_fatal_error(CodexErrorKind kind, std::string message)
{
    if (!fatal_error_kind_) {
        fatal_error_kind_ = kind;
        fatal_error_message_ = std::move(message);
    }
    emit transportStateChanged();
}

void CodexAppServer::throw_if_unavailable() const
{
    if (fatal_error_kind_) {
        throw CodexError(*fatal_error_kind_, fatal_error_message_);
    }
    if (!running_) {
        throw CodexError(CodexErrorKind::ProcessCrashed, "Codex app-server is not running.");
    }
}

void CodexAppServer::ensure_public_ready() const
{
    throw_if_unavailable();
    if (initializing_ || !initialized_) {
        throw CodexError(CodexErrorKind::StartupFailed,
                         "Codex app-server is not ready for public operations.");
    }
}

void CodexAppServer::retire_after_setup_failure(CodexErrorKind kind)
{
    if ((kind == CodexErrorKind::Cancelled || kind == CodexErrorKind::Timeout) && running_) {
        stop();
    }
}

void CodexAppServer::wait_until(const std::function<bool()>& ready,
                                std::chrono::milliseconds timeout,
                                const std::function<bool()>& cancelled)
{
    QElapsedTimer elapsed;
    elapsed.start();

    while (!ready()) {
        throw_if_unavailable();
        if (cancelled && cancelled()) {
            throw CodexError(CodexErrorKind::Cancelled, "Codex turn was cancelled.");
        }

        const auto remaining = timeout - std::chrono::milliseconds(elapsed.elapsed());
        if (remaining <= std::chrono::milliseconds::zero()) {
            throw CodexError(CodexErrorKind::Timeout, "Timed out waiting for Codex app-server.");
        }

        QEventLoop event_loop;
        QTimer wake_timer;
        wake_timer.setSingleShot(true);
        const auto wake_after = cancelled ? std::min(remaining, std::chrono::milliseconds(10)) : remaining;
        connect(this, &CodexAppServer::transportStateChanged, &event_loop, &QEventLoop::quit);
        connect(&wake_timer, &QTimer::timeout, &event_loop, &QEventLoop::quit);
        wake_timer.start(timer_interval(wake_after));
        event_loop.exec(QEventLoop::AllEvents);
    }
}
