#include <catch2/catch_test_macros.hpp>

#include "CodexAppServer.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>

#include <chrono>
#include <optional>
#include <string>

class CodexAppServerTestAccess final {
public:
    static std::size_t pending_notification_count(const CodexAppServer& server)
    {
        return server.notifications_.size();
    }
};

namespace {

void ensure_qt_application()
{
    if (QApplication::instance()) {
        return;
    }

    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    static int argc = 1;
    static char arg0[] = "codex-app-server-tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication* app = new QApplication(argc, argv);
    Q_UNUSED(app);
}

class FakeScenario final {
public:
    explicit FakeScenario(const char* scenario)
        : existed_(qEnvironmentVariableIsSet("AIFS_FAKE_CODEX_SCENARIO")), previous_(qgetenv("AIFS_FAKE_CODEX_SCENARIO"))
    {
        ensure_qt_application();
        qputenv("AIFS_FAKE_CODEX_SCENARIO", scenario);
    }

    ~FakeScenario()
    {
        if (existed_) {
            qputenv("AIFS_FAKE_CODEX_SCENARIO", previous_);
        } else {
            qunsetenv("AIFS_FAKE_CODEX_SCENARIO");
        }
    }

private:
    bool existed_;
    QByteArray previous_;
};

CodexLaunchConfig fake_launch_config()
{
    return {
        .executable_path = AIFS_FAKE_CODEX_APP_SERVER_PATH,
        .codex_home = ".",
        .client_version = "test",
    };
}

CodexTurnRequest fake_turn_request()
{
    return {
        .config = {
            .inference_cwd = ".",
            .base_instructions = "Return supplied analysis only.",
            .developer_instructions = "Do not use tools.",
        },
        .inputs = {{CodexUserInput::Kind::Text, "Classify this supplied text."}},
    };
}

} // namespace

TEST_CASE("CodexAppServer rejects queued operations during initialization")
{
    FakeScenario scenario("delayed-initialize");
    CodexAppServer server;
    std::optional<CodexErrorKind> error_kind;

    QTimer::singleShot(0, [&] {
        try {
            static_cast<void>(server.request("account/read", Json::Value(Json::objectValue),
                                             std::chrono::milliseconds(250)));
        } catch (const CodexError& error) {
            error_kind = error.kind();
        }
    });

    server.start(fake_launch_config());

    REQUIRE(error_kind.has_value());
    CHECK(error_kind.value() == CodexErrorKind::StartupFailed);
}

TEST_CASE("CodexAppServer observes cancellation while starting a thread")
{
    FakeScenario scenario("slow-thread-start");
    CodexAppServer server;
    server.start(fake_launch_config());
    QElapsedTimer elapsed;
    elapsed.start();
    std::optional<CodexErrorKind> error_kind;

    try {
        static_cast<void>(server.run_ephemeral_turn(fake_turn_request(), std::chrono::milliseconds(400), [&] {
            return elapsed.elapsed() >= 40;
        }));
    } catch (const CodexError& error) {
        error_kind = error.kind();
    }

    REQUIRE(error_kind.has_value());
    CHECK(error_kind.value() == CodexErrorKind::Cancelled);
    CHECK(elapsed.elapsed() < 250);
    CHECK_FALSE(server.is_running());
    std::optional<CodexErrorKind> next_error_kind;
    try {
        static_cast<void>(server.request("account/read", Json::Value(Json::objectValue),
                                         std::chrono::milliseconds(50)));
    } catch (const CodexError& error) {
        next_error_kind = error.kind();
    }
    REQUIRE(next_error_kind.has_value());
    CHECK(next_error_kind.value() == CodexErrorKind::ProcessCrashed);
}

TEST_CASE("CodexAppServer observes cancellation while starting a turn")
{
    FakeScenario scenario("slow-turn-start");
    CodexAppServer server;
    server.start(fake_launch_config());
    QElapsedTimer elapsed;
    elapsed.start();
    std::optional<CodexErrorKind> error_kind;

    try {
        static_cast<void>(server.run_ephemeral_turn(fake_turn_request(), std::chrono::milliseconds(400), [&] {
            return elapsed.elapsed() >= 40;
        }));
    } catch (const CodexError& error) {
        error_kind = error.kind();
    }

    REQUIRE(error_kind.has_value());
    CHECK(error_kind.value() == CodexErrorKind::Cancelled);
    CHECK(elapsed.elapsed() < 250);
    CHECK_FALSE(server.is_running());
    std::optional<CodexErrorKind> next_error_kind;
    try {
        static_cast<void>(server.request("account/read", Json::Value(Json::objectValue),
                                         std::chrono::milliseconds(50)));
    } catch (const CodexError& error) {
        next_error_kind = error.kind();
    }
    REQUIRE(next_error_kind.has_value());
    CHECK(next_error_kind.value() == CodexErrorKind::ProcessCrashed);
}

TEST_CASE("CodexAppServer bounds idle notification retention")
{
    FakeScenario scenario("notification-flood");
    CodexAppServer server;
    server.start(fake_launch_config());

    const Json::Value response = server.request("account/read", Json::Value(Json::objectValue),
                                                std::chrono::milliseconds(250));

    CHECK(response["scenario"].asString() == "notification-flood");
    CHECK(CodexAppServerTestAccess::pending_notification_count(server) <= 256);
}

TEST_CASE("CodexAppServer preserves pending turn notifications before turn response")
{
    FakeScenario scenario("turn-notification-flood-before-response");
    CodexAppServer server;
    server.start(fake_launch_config());

    const CodexTurnResult result = server.run_ephemeral_turn(fake_turn_request(), std::chrono::milliseconds(500), [] {
        return false;
    });

    CHECK(result.text == std::string(300, 'x'));
    CHECK(result.completion.status == CodexTurnStatus::Completed);
    CHECK(result.completion.usage.input_tokens == 3);
    CHECK(result.completion.usage.output_tokens == 5);
}

TEST_CASE("CodexAppServer handles short writes for large inline image requests")
{
    FakeScenario scenario("vision-turn");
    CodexAppServer server;
    server.start(fake_launch_config());
    server.set_test_write_chunk_limit(7);

    const std::vector<CodexUserInput> inputs = {
        {CodexUserInput::Kind::ImageDataUrl, "data:image/png;base64," + std::string(128 * 1024, 'A')},
    };
    const Json::Value params = CodexProtocol::make_turn_start_params("fake-thread", inputs, Json::Value());

    const Json::Value response = server.request("turn/start", params, std::chrono::milliseconds(500));

    REQUIRE(response["turn"]["id"].isString());
    CHECK(response["turn"]["id"].asString() == "fake-turn-0");
}

TEST_CASE("CodexAppServer reserves handshake methods after initialization")
{
    FakeScenario scenario("authenticated");
    CodexAppServer server;
    server.start(fake_launch_config());

    CHECK_THROWS_AS(server.request("initialize", Json::Value(Json::objectValue), std::chrono::milliseconds(50)),
                    CodexError);
    CHECK_THROWS_AS(server.notify("initialized"), CodexError);
}

TEST_CASE("CodexAppServer initializes and correlates out-of-order responses")
{
    FakeScenario scenario("authenticated");
    CodexAppServer server;
    server.start(fake_launch_config());

    std::optional<Json::Value> inner_result;
    QTimer::singleShot(0, [&] {
        inner_result = server.request("test/inner", Json::Value(Json::objectValue), std::chrono::milliseconds(250));
    });

    const Json::Value outer_result = server.request("test/outer", Json::Value(Json::objectValue),
                                                    std::chrono::milliseconds(250));
    REQUIRE(inner_result.has_value());
    CHECK(inner_result.value()["sequence"].asString() == "inner");
    CHECK(outer_result["sequence"].asString() == "outer");
}

TEST_CASE("CodexAppServer keeps reading notifications while waiting for a turn")
{
    FakeScenario scenario("text-turn");
    CodexAppServer server;
    server.start(fake_launch_config());

    const CodexTurnResult result = server.run_ephemeral_turn(fake_turn_request(), std::chrono::milliseconds(250), [] {
        return false;
    });

    CHECK(result.text == "fake completion");
    CHECK(result.completion.status == CodexTurnStatus::Completed);
    CHECK(result.completion.usage.input_tokens == 3);
    CHECK(result.completion.usage.output_tokens == 5);
}

TEST_CASE("CodexAppServer maps malformed JSON to protocol error")
{
    FakeScenario scenario("malformed-json");
    CodexAppServer server;

    try {
        server.start(fake_launch_config());
        FAIL("Expected malformed JSON to fail startup.");
    } catch (const CodexError& error) {
        CHECK(error.kind() == CodexErrorKind::ProtocolError);
    }
}

TEST_CASE("CodexAppServer reports child crash without replaying the request")
{
    FakeScenario scenario("crash-after-turn-start");
    CodexAppServer server;
    server.start(fake_launch_config());

    try {
        static_cast<void>(server.run_ephemeral_turn(fake_turn_request(), std::chrono::milliseconds(250), [] {
            return false;
        }));
        FAIL("Expected the fake child crash to fail the active turn.");
    } catch (const CodexError& error) {
        CHECK(error.kind() == CodexErrorKind::ProcessCrashed);
        CHECK(std::string(error.what()).find("fake child crashed after turn start") != std::string::npos);
    }
    CHECK_FALSE(server.is_running());
    CHECK_THROWS_AS(server.request("account/read", Json::Value(Json::objectValue), std::chrono::milliseconds(50)),
                    CodexError);
}

TEST_CASE("CodexAppServer times out a non-completing turn")
{
    FakeScenario scenario("slow-turn");
    CodexAppServer server;
    server.start(fake_launch_config());

    try {
        static_cast<void>(server.run_ephemeral_turn(fake_turn_request(), std::chrono::milliseconds(50), [] {
            return false;
        }));
        FAIL("Expected the non-completing turn to time out.");
    } catch (const CodexError& error) {
        CHECK(error.kind() == CodexErrorKind::Timeout);
    }
}
