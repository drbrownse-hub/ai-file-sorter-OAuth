#include <catch2/catch_test_macros.hpp>

#include "CodexImageAnalyzer.hpp"
#include "CodexRuntimeService.hpp"
#include "TestHelpers.hpp"

#include <jsoncpp/json/json.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QThread>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>

namespace {

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(1500))
{
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
        : log_dir_(),
          scenario_guard_("AIFS_FAKE_CODEX_SCENARIO", std::string(scenario)),
          event_log_guard_("AIFS_FAKE_CODEX_EVENT_LOG", (log_dir_.path() / "events.log").string())
    {
    }

    std::string event_log() const
    {
        std::ifstream stream((log_dir_.path() / "events.log").string());
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

private:
    TempDir log_dir_;
    EnvVarGuard scenario_guard_;
    EnvVarGuard event_log_guard_;
};

CodexRuntimeConfig runtime_config(const std::string& codex_home)
{
    return {
        .executable_path = AIFS_FAKE_CODEX_APP_SERVER_PATH,
        .application_directory = ".",
        .path_entries = {},
        .codex_home = codex_home,
        .model = {},
        .client_version = "image-analyzer-test",
    };
}

std::shared_ptr<CodexRuntimeService> authenticated_runtime(TempDir& codex_home)
{
    auto runtime = std::make_shared<CodexRuntimeService>();
    runtime->configure(runtime_config(codex_home.path().string()));
    runtime->start_or_refresh_async();
    REQUIRE(wait_until([&] {
        const auto snapshot = runtime->snapshot();
        return snapshot.running && snapshot.authenticated && snapshot.models.size() == 2;
    }));
    return runtime;
}

Json::Value captured_turn(const FakeRuntimeEnvironment& environment)
{
    const std::string events = environment.event_log();
    const std::string marker = "turn-request ";
    const std::size_t marker_start = events.find(marker);
    REQUIRE(marker_start != std::string::npos);
    const std::size_t json_start = marker_start + marker.size();
    const std::size_t line_end = events.find('\n', json_start);
    const std::string json = events.substr(json_start, line_end == std::string::npos ? std::string::npos
                                                                                       : line_end - json_start);

    Json::CharReaderBuilder reader_builder;
    Json::Value request;
    std::string errors;
    std::istringstream stream(json);
    REQUIRE(Json::parseFromStream(reader_builder, stream, &request, &errors));
    return request;
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

std::filesystem::path write_large_red_square(TempDir& directory)
{
    const std::filesystem::path image_path = directory.path() / "private-source-name.png";
    QImage image(4096, 2048, QImage::Format_RGB32);
    image.fill(Qt::red);
    REQUIRE(image.save(QString::fromStdString(image_path.string()), "PNG"));
    return image_path;
}

} // namespace

TEST_CASE("Codex image analyzer sends normalized PNG data URL and no source path")
{
    QtAppContext qt;
    FakeRuntimeEnvironment environment("image-analyzer");
    TempDir codex_home;
    TempDir image_directory;
    const std::filesystem::path image_path = write_large_red_square(image_directory);
    auto runtime = authenticated_runtime(codex_home);
    CodexImageAnalyzer analyzer(runtime, "");

    const ImageAnalysisResult result = analyzer.analyze(image_path);
    CHECK(result.description == "a red square");
    CHECK(result.suggested_name == "red-square.png");
    CHECK_FALSE(result.diagnostics.available);

    const Json::Value request = captured_turn(environment);
    REQUIRE(request["params"]["input"].isArray());
    REQUIRE(request["params"]["input"].size() == 2);
    CHECK(request["params"]["input"][0]["type"].asString() == "text");
    CHECK(request["params"]["input"][1]["type"].asString() == "image");
    CHECK_FALSE(request["params"]["input"][1].isMember("path"));
    CHECK(request["params"]["input"][1]["imageUrl"].asString().find("data:image/png;base64,") == 0);
    CHECK(request["params"]["outputSchema"]["type"].asString() == "object");
    CHECK(request["params"]["outputSchema"]["additionalProperties"].asBool() == false);
    CHECK(request["params"]["outputSchema"]["required"].size() == 2);
    CHECK(request["params"]["outputSchema"]["properties"]["description"]["type"].asString() == "string");
    CHECK(request["params"]["outputSchema"]["properties"]["suggested_name"]["type"].asString() == "string");

    const std::string data_url = request["params"]["input"][1]["imageUrl"].asString();
    const std::string prefix = "data:image/png;base64,";
    const QByteArray png = QByteArray::fromBase64(QByteArray::fromStdString(data_url.substr(prefix.size())));
    QImage normalized;
    REQUIRE(normalized.loadFromData(png, "PNG"));
    CHECK(normalized.size() == QSize(2048, 1024));
    CHECK(environment.event_log().find(image_path.string()) == std::string::npos);
}

TEST_CASE("Codex image analyzer refuses a text-only model before making a turn")
{
    QtAppContext qt;
    FakeRuntimeEnvironment environment("image-analyzer");
    TempDir codex_home;
    TempDir image_directory;
    const std::filesystem::path image_path = write_large_red_square(image_directory);
    auto runtime = authenticated_runtime(codex_home);
    CodexImageAnalyzer analyzer(runtime, "text-only");

    require_codex_error([&] { static_cast<void>(analyzer.analyze(image_path)); }, CodexErrorKind::ImageUnsupported);
    CHECK(environment.event_log().find("request turn/start") == std::string::npos);
}

TEST_CASE("Codex image analyzer rejects an undecodable image before making a turn")
{
    QtAppContext qt;
    FakeRuntimeEnvironment environment("image-analyzer");
    TempDir codex_home;
    TempDir image_directory;
    const std::filesystem::path image_path = image_directory.path() / "not-an-image.bin";
    {
        std::ofstream stream(image_path);
        stream << "not a decodable image";
    }
    auto runtime = authenticated_runtime(codex_home);
    CodexImageAnalyzer analyzer(runtime, "");

    require_codex_error([&] { static_cast<void>(analyzer.analyze(image_path)); }, CodexErrorKind::TurnFailed);
    CHECK(environment.event_log().find("request turn/start") == std::string::npos);
}

TEST_CASE("Codex image analyzer rejects malformed visual structured responses")
{
    QtAppContext qt;
    SECTION("missing field")
    {
        FakeRuntimeEnvironment environment("image-analyzer-malformed-missing");
        TempDir codex_home;
        TempDir image_directory;
        const std::filesystem::path image_path = write_large_red_square(image_directory);
        auto runtime = authenticated_runtime(codex_home);
        CodexImageAnalyzer analyzer(runtime, "");
        require_codex_error([&] { static_cast<void>(analyzer.analyze(image_path)); }, CodexErrorKind::ProtocolError);
    }
    SECTION("non-string field")
    {
        FakeRuntimeEnvironment environment("image-analyzer-malformed-nonstring");
        TempDir codex_home;
        TempDir image_directory;
        const std::filesystem::path image_path = write_large_red_square(image_directory);
        auto runtime = authenticated_runtime(codex_home);
        CodexImageAnalyzer analyzer(runtime, "");
        require_codex_error([&] { static_cast<void>(analyzer.analyze(image_path)); }, CodexErrorKind::ProtocolError);
    }
    SECTION("extra field")
    {
        FakeRuntimeEnvironment environment("image-analyzer-malformed-extra");
        TempDir codex_home;
        TempDir image_directory;
        const std::filesystem::path image_path = write_large_red_square(image_directory);
        auto runtime = authenticated_runtime(codex_home);
        CodexImageAnalyzer analyzer(runtime, "");
        require_codex_error([&] { static_cast<void>(analyzer.analyze(image_path)); }, CodexErrorKind::ProtocolError);
    }
}
