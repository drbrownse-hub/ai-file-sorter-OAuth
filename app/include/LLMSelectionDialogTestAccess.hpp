#pragma once

#ifdef AI_FILE_SORTER_TEST_BUILD

#include "CodexRuntimeService.hpp"

#include <optional>
#include <string>
#include <vector>

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QWidget>
class LLMDownloader;
class LLMSelectionDialog;

class LLMSelectionDialogTestAccess {
public:
    struct VisualEntryRefs {
        QLabel* status_label{nullptr};
        QPushButton* download_button{nullptr};
        QProgressBar* progress_bar{nullptr};
        LLMDownloader* downloader{nullptr};
    };

    struct CodexControlRefs {
        QRadioButton* account_radio{nullptr};
        QWidget* account_group{nullptr};
        QLineEdit* executable_edit{nullptr};
        QLabel* runtime_status_label{nullptr};
        QLabel* account_status_label{nullptr};
        QLabel* model_status_label{nullptr};
        QComboBox* model_combo{nullptr};
        QPushButton* sign_in_button{nullptr};
        QPushButton* sign_out_button{nullptr};
        QPushButton* device_login_button{nullptr};
        QWidget* openai_inputs{nullptr};
    };

    static VisualEntryRefs llava_model_entry(LLMSelectionDialog& dialog);
    static VisualEntryRefs llava_mmproj_entry(LLMSelectionDialog& dialog);
    static void refresh_visual_downloads(LLMSelectionDialog& dialog);
    static void update_llava_model_entry(LLMSelectionDialog& dialog);
    static void start_llava_model_download(LLMSelectionDialog& dialog);
    static VisualEntryRefs visual_entry_for_env_var(LLMSelectionDialog& dialog, const std::string& env_var);
    static std::string selected_visual_model_id(const LLMSelectionDialog& dialog);
    static std::string selected_visual_model_label(const LLMSelectionDialog& dialog);
    /**
     * @brief Returns the active built-in local model downloader.
     * @param dialog Selection dialog instance under test.
     * @return Downloader used by the local LLM download panel, or nullptr.
     */
    static LLMDownloader* local_downloader(LLMSelectionDialog& dialog);
    /**
     * @brief Returns the visible built-in local model labels in top-to-bottom dialog order.
     * @param dialog Selection dialog instance under test.
     * @return Ordered list of built-in local model labels.
     */
    static std::vector<std::string> local_builtin_labels(const LLMSelectionDialog& dialog);
    static void select_visual_backend(LLMSelectionDialog& dialog, const std::string& backend_id);
    static void set_network_available_override(LLMSelectionDialog& dialog, std::optional<bool> value);
    static CodexControlRefs codex_controls(LLMSelectionDialog& dialog);
    static void refresh_codex_controls(LLMSelectionDialog& dialog);
    static void apply_codex_executable_path(LLMSelectionDialog& dialog);
    static void set_codex_runtime_snapshot_override(
        LLMSelectionDialog& dialog,
        std::optional<CodexRuntimeSnapshot> snapshot);
    static bool visual_backend_enabled(const LLMSelectionDialog& dialog,
                                       const std::string& backend_id);
    static std::string visual_backend_unavailable_reason(const LLMSelectionDialog& dialog,
                                                         const std::string& backend_id);
    static void accept_dialog(LLMSelectionDialog& dialog);
};

#endif // AI_FILE_SORTER_TEST_BUILD
