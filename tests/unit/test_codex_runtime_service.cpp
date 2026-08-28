#include <catch2/catch_test_macros.hpp>

#include "CodexRuntimeService.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

void ensure_qt_application()
{
    if (QCoreApplication::instance()) {
        return;
    }

    static int argc = 1;
    static char arg0[] = "codex-runtime-tests";
    static char* argv[] = {arg0, nullptr};
    static QCoreApplication application(argc, argv);
    Q_UNUSED(application);
}

bool wait_until(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = std::chrono::milliseconds(1500))
{
    ensure_qt_application();
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeout.count()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return predicate();
}

class FakeRuntimeEnvironment final {
public:
    explicit FakeRuntimeEnvironment(const char* scenario)
        : scenario_existed_(qEnvironmentVariableIsSet("AIFS_FAKE_CODEX_SCENARIO")),
          previous_scenario_(qgetenv("AIFS_FAKE_CODEX_SCENARIO")),
          log_dir_()
    {
        ensure_qt_application();
        qputenv("AIFS_FAKE_CODEX_SCENARIO", scenario);
        event_log_ = log_dir_.filePath("events.log");
        qputenv("AIFS_FAKE_CODEX_EVENT_LOG", event_log_.toUtf8());
        crash_marker_ = log_dir_.filePath("crash.marker");
        qputenv("AIFS_FAKE_CODEX_CRASH_MARKER", crash_marker_.toUtf8());
    }

    ~FakeRuntimeEnvironment()
    {
        if (scenario_existed_) {
            qputenv("AIFS_FAKE_CODEX_SCENARIO", previous_scenario_);
        } else {
            qunsetenv("AIFS_FAKE_CODEX_SCENARIO");
        }
        qunsetenv("AIFS_FAKE_CODEX_EVENT_LOG");
        qunsetenv("AIFS_FAKE_CODEX_CRASH_MARKER");
    }

    std::string event_log() const
    {
        std::ifstream stream(event_log_.toStdString());
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

private:
    bool scenario_existed_;
    QByteArray previous_scenario_;
    QTemporaryDir log_dir_;
    QString event_log_;
    QString crash_marker_;
};

CodexRuntimeConfig runtime_config(const std::string& codex_home)
{
    return {
        .executable_path = AIFS_FAKE_CODEX_APP_SERVER_PATH,
        .application_directory = ".",
        .path_entries = {},
        .codex_home = codex_home,
        .model = {},
        .client_version = "runtime-test",
    };
}

void copy_fake_runtime(const std::filesystem::path& destination)
{
    std::filesystem::copy_file(AIFS_FAKE_CODEX_APP_SERVER_PATH, destination,
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::permissions(destination,
                                  std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
                                      std::filesystem::perms::owner_write,
                                  std::filesystem::perm_options::add);
}

CodexTurnRequest turn_request(std::string text = "runtime test")
{
    return {
        .config = {
            .inference_cwd = {},
            .base_instructions = "Return supplied analysis only.",
            .developer_instructions = "Do not use tools.",
        },
        .inputs = {{CodexUserInput::Kind::Text, std::move(text)}},
    };
}

bool contains_event(const FakeRuntimeEnvironment& environment, std::string_view event)
{
    return environment.event_log().find(event) != std::string::npos;
}

std::size_t count_events(const FakeRuntimeEnvironment& environment, std::string_view event)
{
    const std::string log = environment.event_log();
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = log.find(event, offset)) != std::string::npos) {
        ++count;
        offset += event.size();
    }
    return count;
}

} // namespace

TEST_CASE("Codex runtime discovery prefers explicit, beside-app, then PATH candidates")
{
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const std::filesystem::path root = temp.path().toStdString();
    const std::filesystem::path beside = root / "beside";
    const std::filesystem::path path_root = root / "path";
    std::filesystem::create_directories(beside);
    std::filesystem::create_directories(path_root);
    copy_fake_runtime(beside / "codex.exe");
    copy_fake_runtime(path_root / "codex.exe");

    SECTION("explicit configured executable")
    {
        FakeRuntimeEnvironment environment("runtime-authenticated");
        CodexRuntimeService service;
        auto config = runtime_config(root.string());
        config.application_directory = beside.string();
        config.path_entries = {path_root.string()};
        service.configure(config);
        service.start_or_refresh_async();
        REQUIRE(wait_until([&] { return service.snapshot().runtime_found; }));
        CHECK(service.snapshot().runtime_version.find(std::string(AIFS_FAKE_CODEX_APP_SERVER_PATH)) != std::string::npos);
    }

    SECTION("executable beside the application")
    {
        FakeRuntimeEnvironment environment("runtime-authenticated");
        CodexRuntimeService service;
        auto config = runtime_config(root.string());
        config.executable_path.clear();
        config.application_directory = beside.string();
        config.path_entries = {path_root.string()};
        service.configure(config);
        service.start_or_refresh_async();
        REQUIRE(wait_until([&] { return service.snapshot().runtime_found; }));
        CHECK(service.snapshot().runtime_version.find((beside / "codex.exe").string()) != std::string::npos);
    }

    SECTION("executable on the injected PATH")
    {
        FakeRuntimeEnvironment environment("runtime-authenticated");
        CodexRuntimeService service;
        auto config = runtime_config(root.string());
        config.executable_path.clear();
        config.application_directory = (root / "missing").string();
        config.path_entries = {path_root.string()};
        service.configure(config);
        service.start_or_refresh_async();
        REQUIRE(wait_until([&] { return service.snapshot().runtime_found; }));
        CHECK(service.snapshot().runtime_version.find((path_root / "codex.exe").string()) != std::string::npos);
    }
}

TEST_CASE("Codex runtime reports RuntimeNotFound without machine discovery")
{
    FakeRuntimeEnvironment environment("runtime-authenticated");
    QTemporaryDir temp;
    CodexRuntimeService service;
    auto config = runtime_config(temp.path().toStdString());
    config.executable_path.clear();
    config.application_directory = temp.filePath("missing").toStdString();
    config.path_entries = {temp.filePath("empty-path").toStdString()};
    service.configure(config);
    service.start_or_refresh_async();

    REQUIRE(wait_until([&] { return !service.snapshot().last_error.empty(); }));
    CHECK_FALSE(service.snapshot().runtime_found);
    CHECK(service.snapshot().last_error.find("not found") != std::string::npos);
}

TEST_CASE("Codex runtime owns one process and serves worker-thread inference")
{
    FakeRuntimeEnvironment environment("runtime-authenticated");
    QTemporaryDir temp;
    CodexRuntimeService service;
    service.configure(runtime_config(temp.path().toStdString()));
    service.start_or_refresh_async();

    REQUIRE(wait_until([&] {
        const auto snapshot = service.snapshot();
        return snapshot.running && snapshot.authenticated && snapshot.models.size() == 2;
    }));
    CHECK(service.selected_model_accepts_images("") == true);
    CHECK(service.selected_model_accepts_images("text-only") == false);

    std::optional<CodexTurnResult> result;
    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            result = service.run_turn(turn_request());
        } catch (...) {
            failure = std::current_exception();
        }
    });
    worker.join();
    REQUIRE_FALSE(failure);
    REQUIRE(result.has_value());
    CHECK(result->text == "fake completion");

    const std::string events = environment.event_log();
    CHECK_FALSE(events.empty());
    CHECK(events.find("request account/read") != std::string::npos);
    CHECK(events.find("request model/list") != std::string::npos);
    CHECK(events.find("turn-complete") != std::string::npos);
    const std::size_t app_server_start = events.find("app-server-start");
    CHECK(app_server_start != std::string::npos);
    CHECK(events.find("app-server-start", app_server_start + 1) == std::string::npos);
    const std::filesystem::path inference_cwd = std::filesystem::path(temp.path().toStdString()) / "inference";
    CHECK(events.find("thread-start model=image-default cwd=" +
                     inference_cwd.string()) != std::string::npos);
}

TEST_CASE("Codex runtime serializes concurrent text and image turns")
{
    FakeRuntimeEnvironment environment("serialize-turns");
    QTemporaryDir temp;
    CodexRuntimeService service;
    service.configure(runtime_config(temp.path().toStdString()));
    service.start_or_refresh_async();
    REQUIRE(wait_until([&] { return service.snapshot().authenticated; }));

    std::atomic<int> completed{0};
    std::exception_ptr first_failure;
    std::exception_ptr second_failure;
    std::thread first([&] {
        try {
            static_cast<void>(service.run_turn(turn_request("first")));
            ++completed;
        } catch (...) {
            first_failure = std::current_exception();
        }
    });
    std::thread second([&] {
        try {
            CodexTurnRequest image = turn_request("second");
            image.inputs.push_back({CodexUserInput::Kind::ImageDataUrl, "data:image/png;base64,AA=="});
            static_cast<void>(service.run_turn(image));
            ++completed;
        } catch (...) {
            second_failure = std::current_exception();
        }
    });
    first.join();
    second.join();

    const std::string events = environment.event_log();
    CHECK_FALSE(first_failure);
    CHECK_FALSE(second_failure);
    REQUIRE(completed == 2);
    const std::size_t first_start = events.find("turn-start");
    const std::size_t first_complete = events.find("turn-complete");
    const std::size_t second_start = events.find("turn-start", first_start + 1);
    const std::size_t second_complete = events.find("turn-complete", first_complete + 1);
    CHECK(first_start != std::string::npos);
    CHECK(first_start < first_complete);
    CHECK(first_complete < second_start);
    CHECK(second_start < second_complete);
}

TEST_CASE("Codex runtime restarts only on the next operation after a crash")
{
    FakeRuntimeEnvironment environment("crash-once");
    QTemporaryDir temp;
    CodexRuntimeService service;
    service.configure(runtime_config(temp.path().toStdString()));
    service.start_or_refresh_async();
    REQUIRE(wait_until([&] { return service.snapshot().authenticated; }));

    CHECK_THROWS_AS(service.run_turn(turn_request()), CodexError);
    CHECK_FALSE(service.snapshot().running);
    CodexTurnResult recovered;
    REQUIRE_NOTHROW(recovered = service.run_turn(turn_request()));
    CHECK(recovered.text == "fake completion");
    const std::string events = environment.event_log();
    const std::size_t first_start = events.find("app-server-start");
    const std::size_t second_start = events.find("app-server-start", first_start + 1);
    CHECK(first_start != std::string::npos);
    CHECK(second_start != std::string::npos);
}

TEST_CASE("Codex runtime exposes ChatGPT browser/device login and logout without tokens")
{
    FakeRuntimeEnvironment environment("login-flows");
    QTemporaryDir temp;
    CodexRuntimeService service;
    service.configure(runtime_config(temp.path().toStdString()));
    service.start_or_refresh_async();
    REQUIRE(wait_until([&] { return service.snapshot().running && !service.snapshot().authenticated; }));

    std::optional<QUrl> login_url;
    std::optional<QUrl> device_url;
    std::optional<QString> user_code;
    QObject::connect(&service, &CodexRuntimeService::loginUrlReady, &service,
                     [&](const QUrl& url) { login_url = url; });
    QObject::connect(&service, &CodexRuntimeService::deviceCodeReady, &service,
                     [&](const QUrl& url, const QString& code) {
                         device_url = url;
                         user_code = code;
                     });

    service.begin_chatgpt_login_async();
    REQUIRE(wait_until([&] { return login_url.has_value() && service.snapshot().authenticated; }));
    CHECK(login_url->toString() == "https://example.invalid/fake-login");

    service.logout_async();
    REQUIRE(wait_until([&] { return !service.snapshot().authenticated; }));

    service.begin_device_code_login_async();
    REQUIRE(wait_until([&] { return device_url.has_value() && user_code.has_value(); }));
    CHECK(device_url->toString() == "https://example.invalid/device");
    CHECK(user_code.value() == "ABCD-EFGH");
}

TEST_CASE("Codex runtime queues reconfiguration behind an active turn")
{
    FakeRuntimeEnvironment environment("delayed-turn");
    QTemporaryDir temp;
    QTemporaryDir replacement;
    REQUIRE(temp.isValid());
    REQUIRE(replacement.isValid());
    CodexRuntimeService service;
    service.configure(runtime_config(temp.path().toStdString()));
    service.start_or_refresh_async();
    REQUIRE(wait_until([&] { return service.snapshot().authenticated; }));

    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            static_cast<void>(service.run_turn(turn_request()));
        } catch (...) {
            failure = std::current_exception();
        }
    });
    REQUIRE(wait_until([&] { return contains_event(environment, "turn-start"); }));
    service.configure(runtime_config(replacement.path().toStdString()));
    worker.join();

    CHECK_FALSE(failure);
    CHECK_FALSE(service.snapshot().runtime_found);
    CHECK(contains_event(environment, "turn-complete"));
}

TEST_CASE("Codex runtime queues logout behind an active turn")
{
    FakeRuntimeEnvironment environment("delayed-turn");
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    CodexRuntimeService service;
    service.configure(runtime_config(temp.path().toStdString()));
    service.start_or_refresh_async();
    REQUIRE(wait_until([&] { return service.snapshot().authenticated; }));

    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            static_cast<void>(service.run_turn(turn_request()));
        } catch (...) {
            failure = std::current_exception();
        }
    });
    REQUIRE(wait_until([&] { return contains_event(environment, "turn-start"); }));
    service.logout_async();
    worker.join();
    REQUIRE(wait_until([&] { return !service.snapshot().authenticated; }));

    CHECK_FALSE(failure);
    const std::string events = environment.event_log();
    CHECK(events.find("turn-complete") < events.find("request account/logout"));
}

TEST_CASE("Codex runtime allows only one restart after repeated crashes")
{
    FakeRuntimeEnvironment environment("crash-always");
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    CodexRuntimeService service;
    service.configure(runtime_config(temp.path().toStdString()));
    service.start_or_refresh_async();
    REQUIRE(wait_until([&] { return service.snapshot().authenticated; }));

    CHECK_THROWS_AS(service.run_turn(turn_request("first")), CodexError);
    CHECK_THROWS_AS(service.run_turn(turn_request("second")), CodexError);
    CHECK_THROWS_AS(service.run_turn(turn_request("third")), CodexError);
    const std::string events = environment.event_log();
    const std::size_t first_start = events.find("app-server-start");
    const std::size_t second_start = events.find("app-server-start", first_start + 1);
    CHECK(first_start != std::string::npos);
    CHECK(second_start != std::string::npos);
    CHECK(events.find("app-server-start", second_start + 1) == std::string::npos);
}

TEST_CASE("Codex runtime refreshes authentication after account loss")
{
    FakeRuntimeEnvironment environment("auth-loss");
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    CodexRuntimeService service;
    service.configure(runtime_config(temp.path().toStdString()));
    service.start_or_refresh_async();
    REQUIRE(wait_until([&] { return service.snapshot().authenticated; }));
    REQUIRE(wait_until([&] { return !service.snapshot().authenticated; }));

    try {
        static_cast<void>(service.run_turn(turn_request()));
        FAIL("expected authentication error");
    } catch (const CodexError& error) {
        CHECK(error.kind() == CodexErrorKind::AuthenticationRequired);
    }
}

TEST_CASE("Codex runtime drops account refreshes from a replaced app-server")
{
    FakeRuntimeEnvironment environment("auth-loss-delayed-turn");
    QTemporaryDir temp;
    QTemporaryDir replacement;
    REQUIRE(temp.isValid());
    REQUIRE(replacement.isValid());
    CodexRuntimeService service;
    service.configure(runtime_config(temp.path().toStdString()));
    service.start_or_refresh_async();
    REQUIRE(wait_until([&] { return service.snapshot().authenticated; }));

    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            static_cast<void>(service.run_turn(turn_request()));
        } catch (...) {
            failure = std::current_exception();
        }
    });
    REQUIRE(wait_until([&] { return contains_event(environment, "turn-start"); }));
    service.configure(runtime_config(replacement.path().toStdString()));
    worker.join();

    CHECK_FALSE(failure);
    CHECK_FALSE(service.snapshot().runtime_found);
    const std::string events = environment.event_log();
    CHECK(events.find("request account/read") != std::string::npos);
    CHECK(events.find("request account/read", events.find("turn-complete")) == std::string::npos);
}

TEST_CASE("Codex runtime consumes one restart budget after bootstrap crash")
{
    FakeRuntimeEnvironment environment("crash-initialize");
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    CodexRuntimeService service;
    service.configure(runtime_config(temp.path().toStdString()));

    service.start_or_refresh_async();
    REQUIRE(wait_until([&] { return count_events(environment, "app-server-start") >= 1; }));
    service.start_or_refresh_async();
    service.start_or_refresh_async();
    REQUIRE(wait_until([&] {
        return service.snapshot().last_error.find("restart limit reached") != std::string::npos;
    }));

    CHECK(count_events(environment, "app-server-start") == 2);
}

TEST_CASE("Codex runtime always uses its dedicated inference working directory")
{
    FakeRuntimeEnvironment environment("runtime-authenticated");
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    CodexRuntimeService service;
    service.configure(runtime_config(temp.path().toStdString()));
    service.start_or_refresh_async();
    REQUIRE(wait_until([&] { return service.snapshot().authenticated; }));

    CodexTurnRequest request = turn_request();
    request.config.inference_cwd = "/user/controlled/directory";
    REQUIRE_NOTHROW(service.run_turn(request));
    const std::string expected =
        "thread-start model=image-default cwd=" + (std::filesystem::path(temp.path().toStdString()) / "inference").string();
    CHECK(environment.event_log().find(expected) != std::string::npos);
    CHECK(environment.event_log().find("/user/controlled/directory") == std::string::npos);
}

TEST_CASE("Codex runtime preserves a failed login error")
{
    FakeRuntimeEnvironment environment("login-failure");
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    CodexRuntimeService service;
    service.configure(runtime_config(temp.path().toStdString()));
    service.start_or_refresh_async();
    REQUIRE(wait_until([&] { return service.snapshot().running && !service.snapshot().authenticated; }));

    std::optional<QUrl> login_url;
    QObject::connect(&service, &CodexRuntimeService::loginUrlReady, &service,
                     [&](const QUrl& url) { login_url = url; });
    service.begin_chatgpt_login_async();
    REQUIRE(wait_until([&] { return login_url.has_value(); }));
    REQUIRE(wait_until([&] { return !service.snapshot().last_error.empty(); }));
    CHECK(service.snapshot().last_error.find("cancelled") != std::string::npos);
    CHECK_FALSE(service.snapshot().authenticated);
}

TEST_CASE("Codex runtime rejects failed turn completion statuses")
{
    FakeRuntimeEnvironment environment("failed-turn");
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    CodexRuntimeService service;
    service.configure(runtime_config(temp.path().toStdString()));
    service.start_or_refresh_async();
    REQUIRE(wait_until([&] { return service.snapshot().authenticated; }));

    try {
        static_cast<void>(service.run_turn(turn_request()));
        FAIL("expected failed turn error");
    } catch (const CodexError& error) {
        CHECK(error.kind() == CodexErrorKind::TurnFailed);
        CHECK(std::string(error.what()).find("failed") != std::string::npos);
    }
}

TEST_CASE("Codex runtime resolves a removed selected model to the current default")
{
    FakeRuntimeEnvironment environment("runtime-authenticated");
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    CodexRuntimeService service;
    auto config = runtime_config(temp.path().toStdString());
    config.model = "removed-model";
    service.configure(config);
    service.start_or_refresh_async();
    REQUIRE(wait_until([&] { return service.snapshot().authenticated; }));

    CHECK(service.selected_model_accepts_images("removed-model"));
}
