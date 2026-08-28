#include <catch2/catch_test_macros.hpp>

#include "AnalysisCoordinator.hpp"
#include "AnalysisWorkflowContext.hpp"
#include "CodexBackendIds.hpp"
#include "ImageAnalyzerFactory.hpp"
#include "ImageAnalyzer.hpp"
#include "Settings.hpp"
#include "TestHelpers.hpp"
#include "VisualModelCatalog.hpp"

#define private public
#include "CodexClient.hpp"
#include "CodexImageAnalyzer.hpp"
#include "MainApp.hpp"
#undef private

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <spdlog/spdlog.h>

namespace {

class StubImageAnalyzer final : public ImageAnalyzer {
public:
    explicit StubImageAnalyzer(int& call_count)
        : call_count_(call_count)
    {
    }

    ImageAnalysisResult analyze(const std::filesystem::path&) override
    {
        ++call_count_;
        return ImageAnalysisResult{"A test image", "test-image", {}};
    }

private:
    int& call_count_;
};

class ThrowingImageAnalyzer final : public ImageAnalyzer {
public:
    ImageAnalysisResult analyze(const std::filesystem::path&) override
    {
        throw std::runtime_error("remote visual inference failed");
    }
};

class StubTextClient final : public ILLMClient {
public:
    std::string categorize_file(const std::string&,
                                const std::string&,
                                FileType,
                                const std::string&) override
    {
        return R"({"main_category":"Test","subcategory":"Text"})";
    }

    std::string complete_prompt(const std::string&, int) override
    {
        return {};
    }

    void set_prompt_logging_enabled(bool enabled) override
    {
        prompt_logging_enabled = enabled;
    }

    bool prompt_logging_enabled{false};
};

void write_gguf_file(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("GGUF", 4);
    out.put('\0');
}

void write_one_pixel_png(const std::filesystem::path& image_path)
{
    constexpr unsigned char png_bytes[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
        0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41,
        0x54, 0x78, 0x9c, 0x63, 0x60, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0xe2, 0x21, 0xbc, 0x33, 0x00,
        0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
        0x42, 0x60, 0x82};
    std::ofstream image_file(image_path, std::ios::binary | std::ios::trunc);
    image_file.write(reinterpret_cast<const char*>(png_bytes), sizeof(png_bytes));
}

void configure_single_image_workflow(AnalysisWorkflowContext& workflow,
                                     const std::filesystem::path& folder)
{
    workflow.core_logger = spdlog::default_logger();
    workflow.get_folder_path = [&folder]() { return folder.string(); };
    workflow.tr = [](const char* text) { return QString::fromUtf8(text); };
    workflow.should_abort_analysis = []() { return false; };
    workflow.prune_empty_cached_entries_for = [](const std::string&) {};
    workflow.log_cached_highlights = []() {};
    workflow.log_pending_queue = []() {};
    workflow.effective_scan_options = []() { return FileScanOptions::Files; };
    workflow.filter_file_entries = [](std::vector<FileEntry>&) {};
    workflow.notify_review_preview_changed = []() {};
    workflow.notify_recategorization_reset = [](const CategorizedFile&, const std::string&) {};
    workflow.append_progress = [](const std::string&) {};
    workflow.configure_progress_stages = [](const std::vector<AnalysisWorkflowContext::StagePlan>&) {};
    workflow.set_progress_stage_items = [](AnalysisWorkflowContext::StageId,
                                           const std::vector<FileEntry>&) {};
    workflow.set_progress_active_stage = [](AnalysisWorkflowContext::StageId) {};
    workflow.mark_progress_stage_item_in_progress = [](AnalysisWorkflowContext::StageId,
                                                       const FileEntry&) {};
    workflow.mark_progress_stage_item_completed = [](AnalysisWorkflowContext::StageId,
                                                     const FileEntry&) {};
    workflow.mark_progress_stage_item_skipped = [](AnalysisWorkflowContext::StageId,
                                                   const FileEntry&) {};
}

struct ImageAnalyzerFactoryProbeGuard {
    ~ImageAnalyzerFactoryProbeGuard()
    {
        ImageAnalyzerFactory::reset_test_create_probe();
    }
};

struct RoutingFixture {
    EnvVarGuard platform_guard{"QT_QPA_PLATFORM", "offscreen"};
    TempDir temp;
    EnvVarGuard home_guard{"HOME", temp.path().string()};
    EnvVarGuard config_guard{"AI_FILE_SORTER_CONFIG_DIR", temp.path().string()};
    QtAppContext qt_context;
    Settings settings;

    RoutingFixture(LLMChoice text_choice, std::string visual_id, bool analyze_images)
    {
        settings.set_llm_choice(text_choice);
        settings.set_chatgpt_model("gpt-5");
        settings.set_visual_model_id(visual_id);
        settings.set_analyze_images_by_content(analyze_images);
        settings.set_process_images_only(true);
        settings.set_rename_images_only(true);
        settings.set_offer_rename_images(true);
        settings.set_categorize_files(false);

        if (visual_id.rfind("custom:", 0) == 0) {
            const std::string custom_id = visual_id.substr(std::string("custom:").size());
            settings.set_active_custom_llm_id(
                settings.upsert_custom_llm(CustomLLM{
                    custom_id,
                    "Routing local visual model",
                    "",
                    (temp.path() / "local-visual.gguf").string(),
                    (temp.path() / "local-mmproj.gguf").string()}));
            write_gguf_file(temp.path() / "local-visual.gguf");
            write_gguf_file(temp.path() / "local-mmproj.gguf");
        } else if (text_choice == LLMChoice::Custom) {
            settings.set_active_custom_llm_id(
                settings.upsert_custom_llm(CustomLLM{
                    "routing-local",
                    "Routing local model",
                    "",
                    (temp.path() / "local.gguf").string(),
                    ""}));
        }
        REQUIRE(settings.save());
    }
};

TEST_CASE("MainApp routes all text and visual backend combinations")
{
    SECTION("ChatGPT text with ChatGPT vision shares MainApp runtime")
    {
        RoutingFixture fixture(LLMChoice::Remote_ChatGPT,
                               std::string(kChatGptVisualBackendId),
                               true);
        MainApp app(fixture.settings, false, true);

        auto text_client = app.make_llm_client();
        auto workflow = app.make_analysis_workflow_context();
        auto visual_analyzer = workflow.make_remote_image_analyzer();

        auto* codex_client = dynamic_cast<CodexClient*>(text_client.get());
        auto* codex_analyzer = dynamic_cast<CodexImageAnalyzer*>(visual_analyzer.get());
        REQUIRE(codex_client != nullptr);
        REQUIRE(codex_analyzer != nullptr);
        CHECK(codex_client->runtime_ == app.codex_runtime_);
        CHECK(codex_analyzer->runtime_ == app.codex_runtime_);
    }

    SECTION("ChatGPT text with local vision keeps local visual routing")
    {
        RoutingFixture fixture(LLMChoice::Remote_ChatGPT, "custom:routing-local-vision", true);
        MainApp app(fixture.settings, false, true);

        auto text_client = app.make_llm_client();
        auto workflow = app.make_analysis_workflow_context();

        CHECK(dynamic_cast<CodexClient*>(text_client.get()) != nullptr);
        CHECK(workflow.make_remote_image_analyzer() == nullptr);
    }

    SECTION("Local text with ChatGPT vision mixes local text and remote visual")
    {
        RoutingFixture fixture(LLMChoice::Custom,
                               std::string(kChatGptVisualBackendId),
                               true);
        MainApp app(fixture.settings, false, true);

        std::string local_model_path;
        app.local_llm_client_factory_override_ = [&local_model_path](const std::string& path) {
            local_model_path = path;
            return std::make_unique<StubTextClient>();
        };
        auto text_client = app.make_llm_client();

        auto workflow = app.make_analysis_workflow_context();
        auto visual_analyzer = workflow.make_remote_image_analyzer();

        CHECK(app.using_local_llm);
        CHECK(dynamic_cast<StubTextClient*>(text_client.get()) != nullptr);
        CHECK(local_model_path.ends_with("local.gguf"));
        CHECK(dynamic_cast<CodexImageAnalyzer*>(visual_analyzer.get()) != nullptr);
    }

    SECTION("Local text with local vision remains local")
    {
        RoutingFixture fixture(LLMChoice::Custom, "local-model", true);
        MainApp app(fixture.settings, false, true);

        std::string local_model_path;
        app.local_llm_client_factory_override_ = [&local_model_path](const std::string& path) {
            local_model_path = path;
            return std::make_unique<StubTextClient>();
        };
        auto text_client = app.make_llm_client();

        auto workflow = app.make_analysis_workflow_context();

        CHECK(app.using_local_llm);
        CHECK(dynamic_cast<StubTextClient*>(text_client.get()) != nullptr);
        CHECK(local_model_path.ends_with("local.gguf"));
        CHECK(workflow.make_remote_image_analyzer() == nullptr);
    }
}

TEST_CASE("AnalysisCoordinator uses the existing local visual analyzer path for local vision")
{
    ImageAnalyzerFactoryProbeGuard probe_guard;
    for (const LLMChoice text_choice : {LLMChoice::Remote_ChatGPT, LLMChoice::Custom}) {
        SECTION(text_choice == LLMChoice::Remote_ChatGPT
                    ? "ChatGPT text with local vision"
                    : "local text with local vision")
        {
            RoutingFixture fixture(text_choice, "custom:routing-local-vision", true);
            const auto folder = fixture.temp.path() / "images";
            REQUIRE(std::filesystem::create_directories(folder));
            const auto image_path = folder / "input.png";
            write_one_pixel_png(image_path);
            MainApp app(fixture.settings, false, true);

            int factory_calls = 0;
            int analyzer_calls = 0;
            std::string resolved_backend_id;
            ImageAnalyzerFactory::set_test_create_probe(
                [&factory_calls, &analyzer_calls, &resolved_backend_id](
                    const VisualLlmRuntime::Backend& backend,
                    const ImageAnalyzerSettings&) {
                    ++factory_calls;
                    resolved_backend_id = backend.descriptor ? backend.descriptor->id : "";
                    return std::make_unique<StubImageAnalyzer>(analyzer_calls);
                });

            auto workflow = app.make_analysis_workflow_context();
            configure_single_image_workflow(workflow, folder);
            const AnalysisRunResult result = AnalysisCoordinator(std::move(workflow)).execute();

            CHECK(result.status == AnalysisRunStatus::Completed);
            CHECK(factory_calls == 1);
            CHECK(analyzer_calls == 1);
            CHECK(resolved_backend_id == "custom");
        }
    }
}

TEST_CASE("ChatGPT vision factory bypasses local GGUF resolution")
{
    RoutingFixture fixture(LLMChoice::Custom,
                           std::string(kChatGptVisualBackendId),
                           true);
    MainApp app(fixture.settings, false, true);

    auto workflow = app.make_analysis_workflow_context();
    auto analyzer = workflow.make_remote_image_analyzer();
    CHECK(analyzer != nullptr);
    CHECK(dynamic_cast<CodexImageAnalyzer*>(analyzer.get()) != nullptr);
}

TEST_CASE("AnalysisCoordinator uses ChatGPT vision factory before local resolution")
{
    RoutingFixture fixture(LLMChoice::Custom,
                           std::string(kChatGptVisualBackendId),
                           true);
    const auto folder = fixture.temp.path() / "images";
    REQUIRE(std::filesystem::create_directories(folder));
    const auto image_path = folder / "input.png";
    constexpr unsigned char png_bytes[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
        0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41,
        0x54, 0x78, 0x9c, 0x63, 0x60, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0xe2, 0x21, 0xbc, 0x33, 0x00,
        0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
        0x42, 0x60, 0x82};
    std::ofstream image_file(image_path, std::ios::binary);
    REQUIRE(image_file.good());
    image_file.write(reinterpret_cast<const char*>(png_bytes), sizeof(png_bytes));
    REQUIRE(image_file.good());
    image_file.close();
    MainApp app(fixture.settings, false, true);

    auto workflow = app.make_analysis_workflow_context();
    workflow.core_logger = spdlog::default_logger();
    workflow.get_folder_path = [&folder]() { return folder.string(); };
    workflow.tr = [](const char* text) { return QString::fromUtf8(text); };
    workflow.should_abort_analysis = []() { return false; };
    workflow.prune_empty_cached_entries_for = [](const std::string&) {};
    workflow.log_cached_highlights = []() {};
    workflow.log_pending_queue = []() {};
    workflow.effective_scan_options = []() { return FileScanOptions::Files; };
    workflow.filter_file_entries = [](std::vector<FileEntry>&) {};
    workflow.notify_review_preview_changed = []() {};
    workflow.notify_recategorization_reset = [](const CategorizedFile&, const std::string&) {};
    workflow.append_progress = [](const std::string&) {};
    workflow.configure_progress_stages = [](const std::vector<AnalysisWorkflowContext::StagePlan>&) {};
    workflow.set_progress_stage_items = [](AnalysisWorkflowContext::StageId,
                                           const std::vector<FileEntry>&) {};
    workflow.set_progress_active_stage = [](AnalysisWorkflowContext::StageId) {};
    workflow.mark_progress_stage_item_in_progress = [](AnalysisWorkflowContext::StageId,
                                                       const FileEntry&) {};
    workflow.mark_progress_stage_item_completed = [](AnalysisWorkflowContext::StageId,
                                                     const FileEntry&) {};
    workflow.mark_progress_stage_item_skipped = [](AnalysisWorkflowContext::StageId,
                                                   const FileEntry&) {};
    int analyzer_calls = 0;
    workflow.make_remote_image_analyzer = [&analyzer_calls]() {
        return std::make_unique<StubImageAnalyzer>(analyzer_calls);
    };
    const AnalysisRunResult result = AnalysisCoordinator(std::move(workflow)).execute();
    CHECK(result.status == AnalysisRunStatus::Completed);
    CHECK(analyzer_calls == 1);
}

TEST_CASE("Remote visual inference failure asks before falling back to filenames")
{
    RoutingFixture fixture(LLMChoice::Custom,
                           std::string(kChatGptVisualBackendId),
                           true);
    const auto folder = fixture.temp.path() / "images";
    REQUIRE(std::filesystem::create_directories(folder));
    const auto image_path = folder / "input.png";
    constexpr unsigned char png_bytes[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
        0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41,
        0x54, 0x78, 0x9c, 0x63, 0x60, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0xe2, 0x21, 0xbc, 0x33, 0x00,
        0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
        0x42, 0x60, 0x82};
    std::ofstream image_file(image_path, std::ios::binary);
    REQUIRE(image_file.good());
    image_file.write(reinterpret_cast<const char*>(png_bytes), sizeof(png_bytes));
    REQUIRE(image_file.good());
    image_file.close();

    MainApp app(fixture.settings, false, true);
    auto workflow = app.make_analysis_workflow_context();
    workflow.core_logger = spdlog::default_logger();
    workflow.get_folder_path = [&folder]() { return folder.string(); };
    workflow.tr = [](const char* text) { return QString::fromUtf8(text); };
    workflow.should_abort_analysis = []() { return false; };
    workflow.prune_empty_cached_entries_for = [](const std::string&) {};
    workflow.log_cached_highlights = []() {};
    workflow.log_pending_queue = []() {};
    workflow.effective_scan_options = []() { return FileScanOptions::Files; };
    workflow.filter_file_entries = [](std::vector<FileEntry>&) {};
    workflow.notify_review_preview_changed = []() {};
    workflow.notify_recategorization_reset = [](const CategorizedFile&, const std::string&) {};
    workflow.append_progress = [](const std::string&) {};
    workflow.configure_progress_stages = [](const std::vector<AnalysisWorkflowContext::StagePlan>&) {};
    workflow.set_progress_stage_items = [](AnalysisWorkflowContext::StageId,
                                           const std::vector<FileEntry>&) {};
    workflow.set_progress_active_stage = [](AnalysisWorkflowContext::StageId) {};
    workflow.mark_progress_stage_item_in_progress = [](AnalysisWorkflowContext::StageId,
                                                       const FileEntry&) {};
    workflow.mark_progress_stage_item_completed = [](AnalysisWorkflowContext::StageId,
                                                     const FileEntry&) {};
    workflow.mark_progress_stage_item_skipped = [](AnalysisWorkflowContext::StageId,
                                                   const FileEntry&) {};
    int prompt_calls = 0;
    workflow.prompt_continue_without_visual_analysis = [&prompt_calls](const std::string&) {
        ++prompt_calls;
        return false;
    };
    workflow.make_remote_image_analyzer = []() {
        return std::make_unique<ThrowingImageAnalyzer>();
    };

    const AnalysisRunResult result = AnalysisCoordinator(std::move(workflow)).execute();
    CHECK(result.status == AnalysisRunStatus::Cancelled);
    CHECK(prompt_calls == 1);
}

} // namespace
