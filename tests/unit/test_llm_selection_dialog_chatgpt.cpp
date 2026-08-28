#include <catch2/catch_test_macros.hpp>

#include "CodexRuntimeService.hpp"
#include "CodexBackendIds.hpp"
#include "LLMSelectionDialog.hpp"
#include "LLMSelectionDialogTestAccess.hpp"
#include "MainApp.hpp"
#include "Settings.hpp"
#include "TestHelpers.hpp"
#include "VisualModelCatalog.hpp"

#include <QDialog>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QThread>
#include <QRadioButton>

#include <chrono>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

namespace {

CodexRuntimeSnapshot authenticated_snapshot(std::string selected_model = "")
{
    CodexRuntimeSnapshot snapshot;
    snapshot.runtime_found = true;
    snapshot.running = true;
    snapshot.authenticated = true;
    snapshot.runtime_version = "1.2.3";
    snapshot.account = CodexAccountInfo{true, "user@example.com", "pro"};
    snapshot.models = {
        {"gpt-5", "GPT-5", selected_model.empty(), true},
        {"gpt-text", "GPT Text", selected_model == "gpt-text", false},
    };
    if (selected_model.empty()) {
        snapshot.models.front().is_default = true;
    }
    return snapshot;
}

CodexRuntimeSnapshot unauthenticated_snapshot()
{
    CodexRuntimeSnapshot snapshot;
    snapshot.runtime_found = true;
    snapshot.running = true;
    snapshot.authenticated = false;
    snapshot.runtime_version = "1.2.3";
    return snapshot;
}

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(3000))
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeout.count()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return predicate();
}

bool file_contains(const std::string& path, const std::string& text)
{
    std::ifstream stream(path);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()).find(text)
        != std::string::npos;
}

void write_gguf_file(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.is_open());
    stream.write("GGUF", 4);
    stream.put('\0');
}

} // namespace

TEST_CASE("LLM selection dialog distinguishes ChatGPT account from API-key mode")
{
    QtAppContext qt;
    TempDir config_dir;
    EnvVarGuard config_guard("AI_FILE_SORTER_CONFIG_DIR", config_dir.path().string());

    Settings settings;
    settings.load();
    auto runtime = std::make_shared<CodexRuntimeService>();
    settings.set_llm_choice(LLMChoice::Remote_ChatGPT);

    LLMSelectionDialog dialog(settings, runtime);
    const auto refs = LLMSelectionDialogTestAccess::codex_controls(dialog);

    REQUIRE(refs.account_radio != nullptr);
    CHECK(refs.account_radio->text() == QStringLiteral("ChatGPT account (Codex subscription)"));
    CHECK(dialog.get_selected_llm_choice() == LLMChoice::Remote_ChatGPT);
    CHECK(refs.openai_inputs != nullptr);
    CHECK(refs.openai_inputs->isHidden());

    bool has_api_key_mode = false;
    for (auto* radio : dialog.findChildren<QRadioButton*>()) {
        has_api_key_mode = has_api_key_mode || radio->text() == QStringLiteral("ChatGPT (OpenAI API key)");
    }
    CHECK(has_api_key_mode);
}

TEST_CASE("ChatGPT account controls remain available for local text plus ChatGPT vision")
{
    QtAppContext qt;
    TempDir config_dir;
    EnvVarGuard config_guard("AI_FILE_SORTER_CONFIG_DIR", config_dir.path().string());

    Settings settings;
    settings.load();
    settings.set_llm_choice(LLMChoice::Local_4b_Gemma);
    settings.set_visual_model_id(std::string(kChatGptVisualBackendId));
    auto runtime = std::make_shared<CodexRuntimeService>();

    LLMSelectionDialog dialog(settings, runtime);
    LLMSelectionDialogTestAccess::set_codex_runtime_snapshot_override(
        dialog, authenticated_snapshot());
    LLMSelectionDialogTestAccess::refresh_codex_controls(dialog);
    const auto refs = LLMSelectionDialogTestAccess::codex_controls(dialog);

    REQUIRE(refs.account_group != nullptr);
    CHECK_FALSE(refs.account_group->isHidden());
    CHECK(refs.account_radio != nullptr);
    CHECK_FALSE(refs.account_radio->isChecked());
    CHECK(refs.executable_edit != nullptr);
    CHECK(refs.executable_edit->isEnabled());
    CHECK(refs.model_combo != nullptr);
    CHECK(refs.model_combo->count() == 3);
}

TEST_CASE("ChatGPT account state shows sign-in controls without an API key field")
{
    QtAppContext qt;
    TempDir config_dir;
    EnvVarGuard config_guard("AI_FILE_SORTER_CONFIG_DIR", config_dir.path().string());

    Settings settings;
    settings.load();
    settings.set_llm_choice(LLMChoice::Remote_ChatGPT);
    settings.set_visual_model_id(std::string(kChatGptVisualBackendId));
    settings.set_chatgpt_model("gpt-5");
    auto runtime = std::make_shared<CodexRuntimeService>();
    LLMSelectionDialog dialog(settings, runtime);

    LLMSelectionDialogTestAccess::set_codex_runtime_snapshot_override(
        dialog, unauthenticated_snapshot());
    LLMSelectionDialogTestAccess::refresh_codex_controls(dialog);
    const auto refs = LLMSelectionDialogTestAccess::codex_controls(dialog);

    REQUIRE(refs.account_status_label != nullptr);
    CHECK(refs.account_status_label->text().contains(QStringLiteral("Not signed in")));
    REQUIRE(refs.sign_in_button != nullptr);
    CHECK(refs.sign_in_button->isEnabled());
    REQUIRE(refs.sign_out_button != nullptr);
    CHECK_FALSE(refs.sign_out_button->isEnabled());
    REQUIRE(refs.openai_inputs != nullptr);
    CHECK(refs.openai_inputs->isHidden());
    CHECK(dialog.get_chatgpt_model() == "gpt-5");
    CHECK_FALSE(LLMSelectionDialogTestAccess::visual_backend_enabled(
        dialog, std::string(kChatGptVisualBackendId)));
    CHECK(LLMSelectionDialogTestAccess::visual_backend_unavailable_reason(
              dialog, std::string(kChatGptVisualBackendId))
              .find("Sign in") != std::string::npos);
}

TEST_CASE("Authenticated ChatGPT state shows account plan and model list")
{
    QtAppContext qt;
    TempDir config_dir;
    EnvVarGuard config_guard("AI_FILE_SORTER_CONFIG_DIR", config_dir.path().string());

    Settings settings;
    settings.load();
    settings.set_llm_choice(LLMChoice::Remote_ChatGPT);
    auto runtime = std::make_shared<CodexRuntimeService>();
    LLMSelectionDialog dialog(settings, runtime);
    LLMSelectionDialogTestAccess::set_codex_runtime_snapshot_override(
        dialog, authenticated_snapshot());
    LLMSelectionDialogTestAccess::refresh_codex_controls(dialog);
    const auto refs = LLMSelectionDialogTestAccess::codex_controls(dialog);

    REQUIRE(refs.runtime_status_label != nullptr);
    CHECK(refs.runtime_status_label->text().contains(QStringLiteral("1.2.3")));
    REQUIRE(refs.account_status_label != nullptr);
    CHECK(refs.account_status_label->text().contains(QStringLiteral("user@example.com")));
    CHECK(refs.account_status_label->text().contains(QStringLiteral("pro")));
    REQUIRE(refs.model_combo != nullptr);
    CHECK(refs.model_combo->itemText(0) == QStringLiteral("Auto (Codex default)"));
    CHECK(refs.model_combo->findData(QStringLiteral("gpt-5")) >= 0);
    CHECK(refs.model_combo->findData(QStringLiteral("gpt-text")) >= 0);
    CHECK(LLMSelectionDialogTestAccess::visual_backend_enabled(
        dialog, std::string(kChatGptVisualBackendId)));
}

TEST_CASE("ChatGPT visual Auto requires an explicit default model")
{
    QtAppContext qt;
    TempDir config_dir;
    EnvVarGuard config_guard("AI_FILE_SORTER_CONFIG_DIR", config_dir.path().string());

    Settings settings;
    settings.load();
    settings.set_llm_choice(LLMChoice::Local_4b_Gemma);
    settings.set_visual_model_id(std::string(kChatGptVisualBackendId));
    auto runtime = std::make_shared<CodexRuntimeService>();
    LLMSelectionDialog dialog(settings, runtime);

    auto snapshot = authenticated_snapshot();
    for (auto& model : snapshot.models) {
        model.is_default = false;
    }
    LLMSelectionDialogTestAccess::set_codex_runtime_snapshot_override(dialog, std::move(snapshot));
    LLMSelectionDialogTestAccess::refresh_codex_controls(dialog);

    CHECK_FALSE(LLMSelectionDialogTestAccess::visual_backend_enabled(
        dialog, std::string(kChatGptVisualBackendId)));
    const auto reason = LLMSelectionDialogTestAccess::visual_backend_unavailable_reason(
        dialog, std::string(kChatGptVisualBackendId));
    CHECK(reason.find("default") != std::string::npos);
}

TEST_CASE("Removed ChatGPT model falls back to Auto and remains selectable")
{
    QtAppContext qt;
    TempDir config_dir;
    EnvVarGuard config_guard("AI_FILE_SORTER_CONFIG_DIR", config_dir.path().string());

    Settings settings;
    settings.load();
    settings.set_llm_choice(LLMChoice::Remote_ChatGPT);
    settings.set_chatgpt_model("removed-model");
    auto runtime = std::make_shared<CodexRuntimeService>();
    LLMSelectionDialog dialog(settings, runtime);
    LLMSelectionDialogTestAccess::set_codex_runtime_snapshot_override(
        dialog, authenticated_snapshot());
    LLMSelectionDialogTestAccess::refresh_codex_controls(dialog);
    const auto refs = LLMSelectionDialogTestAccess::codex_controls(dialog);

    REQUIRE(refs.model_combo != nullptr);
    CHECK(refs.model_combo->currentIndex() == 0);
    CHECK(refs.model_combo->currentData().toString().isEmpty());
    CHECK(dialog.get_chatgpt_model().empty());
    LLMSelectionDialogTestAccess::accept_dialog(dialog);
    CHECK(dialog.result() == QDialog::Accepted);
}

TEST_CASE("Image-incapable ChatGPT model disables visual choice and blocks acceptance")
{
    QtAppContext qt;
    TempDir config_dir;
    EnvVarGuard config_guard("AI_FILE_SORTER_CONFIG_DIR", config_dir.path().string());

    Settings settings;
    settings.load();
    settings.set_llm_choice(LLMChoice::Local_4b_Gemma);
    settings.set_visual_model_id(std::string(kChatGptVisualBackendId));
    settings.set_chatgpt_model("gpt-text");
    auto runtime = std::make_shared<CodexRuntimeService>();
    LLMSelectionDialog dialog(settings, runtime);
    LLMSelectionDialogTestAccess::set_codex_runtime_snapshot_override(
        dialog, authenticated_snapshot("gpt-text"));
    LLMSelectionDialogTestAccess::refresh_codex_controls(dialog);
    LLMSelectionDialogTestAccess::select_visual_backend(
        dialog, std::string(kChatGptVisualBackendId));

    CHECK_FALSE(LLMSelectionDialogTestAccess::visual_backend_enabled(
        dialog, std::string(kChatGptVisualBackendId)));
    const auto reason = LLMSelectionDialogTestAccess::visual_backend_unavailable_reason(
        dialog, std::string(kChatGptVisualBackendId));
    CHECK_FALSE(reason.empty());
    LLMSelectionDialogTestAccess::accept_dialog(dialog);
    CHECK(dialog.result() != QDialog::Accepted);
    const auto refs = LLMSelectionDialogTestAccess::codex_controls(dialog);
    REQUIRE(refs.model_status_label != nullptr);
    CHECK(refs.model_status_label->text().contains(QStringLiteral("image"), Qt::CaseInsensitive));
}

TEST_CASE("Accepting ChatGPT account selection exposes settings to persist without tokens")
{
    QtAppContext qt;
    TempDir config_dir;
    EnvVarGuard config_guard("AI_FILE_SORTER_CONFIG_DIR", config_dir.path().string());

    Settings settings;
    settings.load();
    settings.set_llm_choice(LLMChoice::Remote_ChatGPT);
    auto runtime = std::make_shared<CodexRuntimeService>();
    LLMSelectionDialog dialog(settings, runtime);
    LLMSelectionDialogTestAccess::set_codex_runtime_snapshot_override(
        dialog, authenticated_snapshot());
    LLMSelectionDialogTestAccess::refresh_codex_controls(dialog);
    const auto refs = LLMSelectionDialogTestAccess::codex_controls(dialog);
    REQUIRE(refs.executable_edit != nullptr);
    refs.executable_edit->setText(QStringLiteral("/opt/codex"));
    REQUIRE(refs.model_combo != nullptr);
    refs.model_combo->setCurrentIndex(refs.model_combo->findData(QStringLiteral("gpt-5")));
    LLMSelectionDialogTestAccess::select_visual_backend(
        dialog, std::string(kChatGptVisualBackendId));

    LLMSelectionDialogTestAccess::accept_dialog(dialog);
    REQUIRE(dialog.result() == QDialog::Accepted);
    CHECK(dialog.get_codex_executable_path() == "/opt/codex");
    CHECK(dialog.get_chatgpt_model() == "gpt-5");
    CHECK(dialog.get_selected_visual_model_id() == std::string(kChatGptVisualBackendId));

    settings.set_codex_executable_path(dialog.get_codex_executable_path());
    settings.set_chatgpt_model(dialog.get_chatgpt_model());
    settings.set_visual_model_id(dialog.get_selected_visual_model_id());
    settings.save();
    Settings reloaded;
    reloaded.load();
    CHECK(reloaded.get_codex_executable_path() == "/opt/codex");
    CHECK(reloaded.get_chatgpt_model() == "gpt-5");
    CHECK(reloaded.get_visual_model_id() == std::string(kChatGptVisualBackendId));
    CHECK(reloaded.get_openai_api_key().empty());
    CHECK(reloaded.get_gemini_api_key().empty());
}

TEST_CASE("MainApp starts its shared Codex runtime after configuring it")
{
    QtAppContext qt;
    TempDir config_dir;
    EnvVarGuard config_guard("AI_FILE_SORTER_CONFIG_DIR", config_dir.path().string());
    EnvVarGuard scenario_guard("AIFS_FAKE_CODEX_SCENARIO", std::string("runtime-authenticated"));
    QTemporaryDir log_dir;
    REQUIRE(log_dir.isValid());
    const std::string event_log = log_dir.filePath("events.log").toStdString();
    EnvVarGuard event_log_guard("AIFS_FAKE_CODEX_EVENT_LOG", event_log);

    Settings settings;
    settings.load();
    settings.set_llm_choice(LLMChoice::Remote_ChatGPT);
    settings.set_codex_executable_path(AIFS_FAKE_CODEX_APP_SERVER_PATH);
    REQUIRE(settings.save());

    MainApp app(settings, false, true);
    CHECK(wait_until([&] { return file_contains(event_log, "request account/read"); }));
}

TEST_CASE("Browsing to a Codex executable starts the existing shared runtime")
{
    QtAppContext qt;
    TempDir config_dir;
    EnvVarGuard config_guard("AI_FILE_SORTER_CONFIG_DIR", config_dir.path().string());
    EnvVarGuard scenario_guard("AIFS_FAKE_CODEX_SCENARIO", std::string("runtime-authenticated"));

    Settings settings;
    settings.load();
    settings.set_llm_choice(LLMChoice::Remote_ChatGPT);
    auto runtime = std::make_shared<CodexRuntimeService>();
    LLMSelectionDialog dialog(settings, runtime);
    const auto refs = LLMSelectionDialogTestAccess::codex_controls(dialog);
    REQUIRE(refs.executable_edit != nullptr);
    refs.executable_edit->setText(QString::fromUtf8(AIFS_FAKE_CODEX_APP_SERVER_PATH));
    LLMSelectionDialogTestAccess::apply_codex_executable_path(dialog);

    CHECK(wait_until([&] {
        const auto snapshot = runtime->snapshot();
        return snapshot.runtime_found && snapshot.running;
    }));
}

TEST_CASE("Local text plus ChatGPT vision re-enables acceptance after an image-capable model is selected")
{
    QtAppContext qt;
    TempDir temp;
    EnvVarGuard home_guard("HOME", temp.path().string());
    EnvVarGuard appdata_guard("APPDATA", temp.path().string());
    EnvVarGuard localappdata_guard("LOCALAPPDATA", temp.path().string());
    TempDir config_dir;
    EnvVarGuard config_guard("AI_FILE_SORTER_CONFIG_DIR", config_dir.path().string());
    const std::string visual_url = "https://local.example/gemma-3-4b-it-Q4_K_M.gguf";
    EnvVarGuard local_url_guard("LOCAL_LLM_3B_DOWNLOAD_URL", visual_url);
    EnvVarGuard visual_url_guard("GEMMA3_4B_MODEL_URL", visual_url);
    const auto* descriptor = find_visual_model_descriptor("gemma-3-4b-it");
    REQUIRE(descriptor != nullptr);
    REQUIRE_FALSE(descriptor->artifacts.empty());
    write_gguf_file(visual_artifact_storage_path(*descriptor, descriptor->artifacts.front()));

    Settings settings;
    settings.load();
    settings.set_llm_choice(LLMChoice::Local_4b_Gemma);
    settings.set_visual_model_id(std::string(kChatGptVisualBackendId));
    settings.set_chatgpt_model("gpt-text");
    auto runtime = std::make_shared<CodexRuntimeService>();
    LLMSelectionDialog dialog(settings, runtime);
    LLMSelectionDialogTestAccess::set_codex_runtime_snapshot_override(
        dialog, authenticated_snapshot("gpt-text"));
    LLMSelectionDialogTestAccess::refresh_codex_controls(dialog);
    const auto refs = LLMSelectionDialogTestAccess::codex_controls(dialog);
    REQUIRE(refs.model_combo != nullptr);
    LLMSelectionDialogTestAccess::select_visual_backend(
        dialog, std::string(kChatGptVisualBackendId));
    LLMSelectionDialogTestAccess::accept_dialog(dialog);
    CHECK(dialog.result() != QDialog::Accepted);

    LLMSelectionDialogTestAccess::set_codex_runtime_snapshot_override(
        dialog, authenticated_snapshot("gpt-5"));
    refs.model_combo->setCurrentIndex(refs.model_combo->findData(QStringLiteral("gpt-5")));
    LLMSelectionDialogTestAccess::refresh_codex_controls(dialog);
    LLMSelectionDialogTestAccess::accept_dialog(dialog);
    CHECK(dialog.result() == QDialog::Accepted);
}
