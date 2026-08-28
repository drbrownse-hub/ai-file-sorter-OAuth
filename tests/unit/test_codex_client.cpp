#include <catch2/catch_test_macros.hpp>

#include "CodexClient.hpp"
#include "CodexRuntimeService.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QThread>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

namespace {

void ensure_qt_application()
{
    if (QCoreApplication::instance()) {
        return;
    }

    static int argc = 1;
    static char arg0[] = "codex-client-tests";
    static char* argv[] = {arg0, nullptr};
    static QCoreApplication application(argc, argv);
    Q_UNUSED(application);
}

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(1500))
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
    }

    ~FakeRuntimeEnvironment()
    {
        if (scenario_existed_) {
            qputenv("AIFS_FAKE_CODEX_SCENARIO", previous_scenario_);
        } else {
            qunsetenv("AIFS_FAKE_CODEX_SCENARIO");
        }
        qunsetenv("AIFS_FAKE_CODEX_EVENT_LOG");
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
};

CodexRuntimeConfig runtime_config(const std::string& codex_home)
{
    return {
        .executable_path = AIFS_FAKE_CODEX_APP_SERVER_PATH,
        .application_directory = ".",
        .path_entries = {},
        .codex_home = codex_home,
        .model = {},
        .client_version = "client-test",
    };
}

std::shared_ptr<CodexRuntimeService> authenticated_runtime(QTemporaryDir& temp)
{
    auto runtime = std::make_shared<CodexRuntimeService>();
    runtime->configure(runtime_config(temp.path().toStdString()));
    runtime->start_or_refresh_async();
    REQUIRE(wait_until([&] {
        const auto snapshot = runtime->snapshot();
        return snapshot.running && snapshot.authenticated;
    }));
    return runtime;
}

void require_codex_error(const std::function<void()>& operation, CodexErrorKind expected_kind)
{
    try {
        operation();
        FAIL("Expected CodexError");
    } catch (const CodexError& error) {
        CHECK(error.kind() == expected_kind);
    }
}

} // namespace

TEST_CASE("CodexClient adapts structured category JSON to existing category contract")
{
    FakeRuntimeEnvironment environment("client-category");
    QTemporaryDir temp;
    auto runtime = authenticated_runtime(temp);
    CodexClient client(runtime, "");

    CHECK(client.categorize_file("paper.pdf",
                                 "/private/source/paper.pdf",
                                 FileType::File,
                                 "Allowed main categories: Research article") ==
          "Research article : Microbiology");
}

TEST_CASE("CodexClient does not send actionable absolute source path")
{
    FakeRuntimeEnvironment environment("client-category");
    QTemporaryDir temp;
    auto runtime = authenticated_runtime(temp);
    CodexClient client(runtime, "");

    static_cast<void>(client.categorize_file("paper.pdf",
                                             "/private/source/paper.pdf",
                                             FileType::File,
                                             ""));

    const std::string events = environment.event_log();
    CHECK(events.find("/private/source/paper.pdf") == std::string::npos);
    CHECK(events.find("paper.pdf") != std::string::npos);
}

TEST_CASE("CodexClient generic completion does not attach category output schema")
{
    FakeRuntimeEnvironment environment("client-generic");
    QTemporaryDir temp;
    auto runtime = authenticated_runtime(temp);
    CodexClient client(runtime, "");

    CHECK(client.complete_prompt("Say hello", 42) == "generic completion");
    CHECK(environment.event_log().find("outputSchema") == std::string::npos);
}

TEST_CASE("CodexClient propagates typed authentication and rate-limit errors")
{
    SECTION("authentication required")
    {
        FakeRuntimeEnvironment environment("auth-required");
        QTemporaryDir temp;
        auto runtime = std::make_shared<CodexRuntimeService>();
        runtime->configure(runtime_config(temp.path().toStdString()));
        runtime->start_or_refresh_async();
        REQUIRE(wait_until([&] { return runtime->snapshot().running; }));
        CodexClient client(runtime, "");

        require_codex_error(
            [&] {
                static_cast<void>(client.categorize_file("paper.pdf", "", FileType::File, ""));
            },
            CodexErrorKind::AuthenticationRequired);
    }

    SECTION("rate limited")
    {
        FakeRuntimeEnvironment environment("client-rate-limited");
        QTemporaryDir temp;
        auto runtime = authenticated_runtime(temp);
        CodexClient client(runtime, "");

        require_codex_error(
            [&] {
                static_cast<void>(client.categorize_file("paper.pdf", "", FileType::File, ""));
            },
            CodexErrorKind::RateLimited);
    }
}

TEST_CASE("CodexClient rejects malformed structured category responses")
{
    SECTION("missing required field")
    {
        FakeRuntimeEnvironment environment("client-invalid-missing");
        QTemporaryDir temp;
        auto runtime = authenticated_runtime(temp);
        CodexClient client(runtime, "");
        require_codex_error(
            [&] {
                static_cast<void>(client.categorize_file("paper.pdf", "", FileType::File, ""));
            },
            CodexErrorKind::ProtocolError);
    }

    SECTION("non-string field")
    {
        FakeRuntimeEnvironment environment("client-invalid-nonstring");
        QTemporaryDir temp;
        auto runtime = authenticated_runtime(temp);
        CodexClient client(runtime, "");
        require_codex_error(
            [&] {
                static_cast<void>(client.categorize_file("paper.pdf", "", FileType::File, ""));
            },
            CodexErrorKind::ProtocolError);
    }

    SECTION("additional field")
    {
        FakeRuntimeEnvironment environment("client-invalid-extra");
        QTemporaryDir temp;
        auto runtime = authenticated_runtime(temp);
        CodexClient client(runtime, "");
        require_codex_error(
            [&] {
                static_cast<void>(client.categorize_file("paper.pdf", "", FileType::File, ""));
            },
            CodexErrorKind::ProtocolError);
    }
}
