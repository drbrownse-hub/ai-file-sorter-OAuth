#include <catch2/catch_test_macros.hpp>

#include "CodexBackendIds.hpp"
#include "Settings.hpp"
#include "TestHelpers.hpp"

#include <fstream>
#include <iterator>
#include <string>

TEST_CASE("ChatGPT backend settings round-trip without OAuth secrets") {
    QtAppContext qt;
    TempDir config_dir;
    EnvVarGuard guard("AI_FILE_SORTER_CONFIG_DIR", config_dir.path().string());

    {
        Settings settings;
        settings.load();
        settings.set_llm_choice(LLMChoice::Remote_ChatGPT);
        settings.set_codex_executable_path("C:/Tools/codex.exe");
        settings.set_chatgpt_model("gpt-5-codex");
        settings.set_visual_model_id(std::string(kChatGptVisualBackendId));
        REQUIRE(settings.save());
    }

    const std::filesystem::path config_path = config_dir.path() / "AIFileSorter" / "config.ini";
    std::ifstream config_file(config_path);
    REQUIRE(config_file.is_open());
    const std::string config_contents((std::istreambuf_iterator<char>(config_file)),
                                      std::istreambuf_iterator<char>());
    CHECK(config_contents.find("AccessToken") == std::string::npos);
    CHECK(config_contents.find("RefreshToken") == std::string::npos);
    CHECK(config_contents.find("OAuthToken") == std::string::npos);

    Settings reloaded;
    REQUIRE(reloaded.load());
    CHECK(reloaded.get_llm_choice() == LLMChoice::Remote_ChatGPT);
    CHECK(reloaded.get_codex_executable_path() == "C:/Tools/codex.exe");
    CHECK(reloaded.get_chatgpt_model() == "gpt-5-codex");
    CHECK(reloaded.get_visual_model_id() == kChatGptVisualBackendId);
}

TEST_CASE("ChatGPT account backend is a remote LLM choice") {
    CHECK(is_remote_choice(LLMChoice::Remote_ChatGPT));
}
