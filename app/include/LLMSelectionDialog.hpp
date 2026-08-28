#ifndef LLMSELECTIONDIALOG_HPP
#define LLMSELECTIONDIALOG_HPP

#include "LLMDownloader.hpp"
#include "CodexRuntimeService.hpp"
#include "Types.hpp"
#include "LLMSelectionVisualBackendModel.hpp"
#include "VisualModelCatalog.hpp"

#include <QCoreApplication>
#include <QDialog>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QLabel;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QDialogButtonBox;
class QWidget;
class QString;
class QComboBox;
class QListWidget;
class QLineEdit;
class QCheckBox;
class QToolButton;
class QScrollArea;
class QShowEvent;

class Settings;

#ifdef AI_FILE_SORTER_TEST_BUILD
class LLMSelectionDialogTestAccess;
#endif

class LLMSelectionDialog : public QDialog
{
    Q_DECLARE_TR_FUNCTIONS(LLMSelectionDialog)
public:
    explicit LLMSelectionDialog(Settings& settings, QWidget* parent = nullptr);
    LLMSelectionDialog(Settings& settings,
                       std::shared_ptr<CodexRuntimeService> codex_runtime,
                       QWidget* parent = nullptr);
    ~LLMSelectionDialog() override;

    LLMChoice get_selected_llm_choice() const;
    std::string get_selected_custom_llm_id() const;
    /**
     * @brief Return the active custom API endpoint id.
     */
    std::string get_selected_custom_api_id() const;
    std::string get_codex_executable_path() const;
    std::string get_chatgpt_model() const;
    std::string get_openai_api_key() const;
    std::string get_openai_model() const;
    std::string get_gemini_api_key() const;
    std::string get_gemini_model() const;
    bool get_llm_downloads_expanded() const;
    /**
     * @brief Return the selected local LLM storage directory override.
     * @return Directory path, or empty when platform defaults should be used.
     */
    std::string get_llm_storage_dir() const;
    /**
     * @brief Return the selected visual model backend id.
     * @return Stable visual model identifier.
     */
    std::string get_selected_visual_model_id() const;

private:
#ifdef AI_FILE_SORTER_TEST_BUILD
    friend class LLMSelectionDialogTestAccess;
#endif

    struct VisualLlmDownloadEntry {
        const VisualModelDescriptor* backend_descriptor{nullptr};
        const VisualModelArtifactDescriptor* artifact_descriptor{nullptr};
        std::string env_var;
        std::string backend_id;
        std::string display_name;
        QWidget* container{nullptr};
        QLabel* title_label{nullptr};
        QLabel* remote_url_label{nullptr};
        QLabel* local_path_label{nullptr};
        QLabel* file_size_label{nullptr};
        QLabel* status_label{nullptr};
        QProgressBar* progress_bar{nullptr};
        QPushButton* download_button{nullptr};
        QPushButton* delete_button{nullptr};
        std::unique_ptr<LLMDownloader> downloader;
        std::optional<std::filesystem::path> resolved_artifact_path;
        std::atomic<bool> is_downloading{false};
    };

    void setup_ui();
    void connect_signals();
    void showEvent(QShowEvent* event) override;
    void accept() override;
    void update_codex_controls();
    void refresh_codex_model_combo();
    void apply_codex_executable_path();
    void browse_codex_executable();
    void begin_codex_login();
    void begin_codex_device_login();
    void logout_codex();
    void update_chatgpt_acceptance_state();
    void update_ui_for_choice();
    void update_legacy_local_3b_visibility();
    void update_radio_selection();
    void update_custom_choice_ui();
    /**
     * @brief Update UI state for the custom API selection.
     */
    void update_custom_api_choice_ui();
    void update_openai_fields_state();
    void update_gemini_fields_state();
    bool openai_inputs_valid() const;
    bool gemini_inputs_valid() const;
    /**
     * @brief Enable or disable built-in local model selectors while a download is active.
     */
    void update_local_download_choice_enabled_state();
    void update_local_choice_ui();
    void update_download_info();
    void start_download();
    /**
     * @brief Delete the downloaded local LLM file after confirmation.
     */
    void handle_delete_download();
    void refresh_downloader();
    void set_status_message(const QString& message);
    void refresh_custom_lists();
    /**
     * @brief Refresh the custom API dropdown list.
     */
    void refresh_custom_api_lists();
    void handle_add_custom();
    void handle_edit_custom();
    void handle_delete_custom();
    void update_custom_buttons();
    void select_custom_by_id(const std::string& id);
    /**
     * @brief Open the dialog to add a custom API entry.
     */
    void handle_add_custom_api();
    /**
     * @brief Open the dialog to edit the selected custom API entry.
     */
    void handle_edit_custom_api();
    /**
     * @brief Remove the selected custom API entry.
     */
    void handle_delete_custom_api();
    /**
     * @brief Update enabled states for custom API controls.
     */
    void update_custom_api_buttons();
    /**
     * @brief Select a custom API entry by id.
     */
    void select_custom_api_by_id(const std::string& id);
    /**
     * @brief Recalculate the dialog size based on visible content.
     */
    void adjust_dialog_size();
    /**
     * @brief Refresh built-in and custom visual backend choices.
     */
    void refresh_visual_backend_combo();
    /**
     * @brief Apply the dialog's current model storage directory to path resolution.
     */
    void apply_model_storage_dir_override();
    /**
     * @brief Open a directory picker for the local LLM storage directory.
     */
    void browse_model_storage_dir();
    /**
     * @brief Reset the local LLM storage directory to platform defaults.
     */
    void reset_model_storage_dir();
    void setup_visual_llm_download_entry(VisualLlmDownloadEntry& entry,
                                     QWidget* parent,
                                     const VisualModelDescriptor& backend,
                                     const VisualModelArtifactDescriptor& artifact);
    void refresh_visual_llm_download_entry(VisualLlmDownloadEntry& entry);
    void update_visual_llm_download_entry(VisualLlmDownloadEntry& entry);
    void update_visual_llm_downloads();
    void start_visual_llm_download(VisualLlmDownloadEntry& entry);
    /**
     * @brief Delete a downloaded visual LLM bundle after confirmation.
     * @param entry Visual LLM entry to delete.
     */
    void handle_delete_visual_download(VisualLlmDownloadEntry& entry);
    void set_visual_status_message(VisualLlmDownloadEntry& entry, const QString& message);
    void update_visual_backend_selection();
    bool chatgpt_controls_needed() const;
    CodexRuntimeSnapshot current_codex_snapshot() const;
    LLMSelectionVisualBackendModel::ChatGptVisualOption chatgpt_visual_option() const;
    bool chatgpt_text_selection_valid(QString* reason = nullptr) const;
    const VisualModelDescriptor* selected_visual_model_descriptor() const;
    VisualLlmDownloadEntry* find_visual_download_entry(std::string_view backend_id,
                                                       VisualModelArtifactKind kind);
    const VisualLlmDownloadEntry* find_visual_download_entry(std::string_view backend_id,
                                                             VisualModelArtifactKind kind) const;
    VisualLlmDownloadEntry* find_visual_download_entry_by_env_var(std::string_view env_var);
    const VisualLlmDownloadEntry* find_visual_download_entry_by_env_var(std::string_view env_var) const;
    bool legacy_local_3b_available() const;

    Settings& settings;
    std::shared_ptr<CodexRuntimeService> codex_runtime_;
    LLMChoice selected_choice{LLMChoice::Unset};
    std::string selected_custom_id;
    std::string selected_custom_api_id;
    std::string selected_visual_model_id_;
    std::string openai_api_key;
    std::string openai_model;
    std::string gemini_api_key;
    std::string gemini_model;
    std::string chatgpt_model_;
    bool downloads_expanded_{true};
    std::string model_storage_dir_;
    std::string original_model_storage_dir_;
    bool accepted_{false};

    QRadioButton* openai_radio{nullptr};
    QRadioButton* chatgpt_account_radio{nullptr};
    QRadioButton* gemini_radio{nullptr};
    QRadioButton* custom_api_radio{nullptr};
    QRadioButton* local3_radio{nullptr};
    QRadioButton* local3_legacy_radio{nullptr};
    QRadioButton* local7_radio{nullptr};
    QRadioButton* local7_gemma_radio{nullptr};
    QRadioButton* custom_radio{nullptr};
    QToolButton* download_toggle_button{nullptr};
    QScrollArea* scroll_area_{nullptr};
    QComboBox* custom_combo{nullptr};
    QPushButton* add_custom_button{nullptr};
    QPushButton* edit_custom_button{nullptr};
    QPushButton* delete_custom_button{nullptr};
    QComboBox* custom_api_combo{nullptr};
    QPushButton* add_custom_api_button{nullptr};
    QPushButton* edit_custom_api_button{nullptr};
    QPushButton* delete_custom_api_button{nullptr};
    QLabel* remote_url_label{nullptr};
    QLabel* local_path_label{nullptr};
    QLabel* file_size_label{nullptr};
    QLabel* status_label{nullptr};
    QLabel* local3_legacy_desc{nullptr};
    QProgressBar* progress_bar{nullptr};
    QPushButton* download_button{nullptr};
    QPushButton* delete_download_button{nullptr};
    QPushButton* ok_button{nullptr};
    QDialogButtonBox* button_box{nullptr};
    QWidget* downloads_container{nullptr};
    QLineEdit* model_storage_dir_edit{nullptr};
    QPushButton* browse_model_storage_dir_button{nullptr};
    QPushButton* reset_model_storage_dir_button{nullptr};
    QWidget* download_section{nullptr};
    QWidget* visual_llm_download_section{nullptr};
    QComboBox* visual_backend_combo{nullptr};
    QWidget* openai_inputs{nullptr};
    QWidget* gemini_inputs{nullptr};
    QLineEdit* openai_api_key_edit{nullptr};
    QLineEdit* openai_model_edit{nullptr};
    QLineEdit* gemini_api_key_edit{nullptr};
    QLineEdit* gemini_model_edit{nullptr};
    QCheckBox* show_openai_api_key_checkbox{nullptr};
    QCheckBox* show_gemini_api_key_checkbox{nullptr};
    QLabel* openai_help_label{nullptr};
    QLabel* openai_link_label{nullptr};
    QLabel* gemini_help_label{nullptr};
    QLabel* gemini_link_label{nullptr};

    QWidget* codex_account_group_{nullptr};
    QLineEdit* codex_executable_edit_{nullptr};
    QPushButton* browse_codex_executable_button_{nullptr};
    QLabel* codex_runtime_status_label_{nullptr};
    QLabel* codex_account_status_label_{nullptr};
    QLabel* codex_model_status_label_{nullptr};
    QComboBox* codex_model_combo_{nullptr};
    QPushButton* codex_sign_in_button_{nullptr};
    QPushButton* codex_sign_out_button_{nullptr};
    QPushButton* codex_device_login_button_{nullptr};

    std::unique_ptr<LLMDownloader> downloader;
    std::atomic<bool> is_downloading{false};
    std::mutex download_mutex;

    std::vector<std::unique_ptr<VisualLlmDownloadEntry>> visual_download_entries_;

#if defined(AI_FILE_SORTER_TEST_BUILD)
    bool use_network_available_override_{false};
    bool network_available_override_{true};
    std::optional<CodexRuntimeSnapshot> codex_runtime_snapshot_override_;
#endif
};

#endif // LLMSELECTIONDIALOG_HPP
