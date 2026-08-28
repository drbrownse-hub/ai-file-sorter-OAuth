#include "CodexClient.hpp"

#include "CategorizationResponseParser.hpp"
#include "CodexRuntimeService.hpp"
#include "Utils.hpp"

#if __has_include(<jsoncpp/json/json.h>)
#include <jsoncpp/json/json.h>
#elif __has_include(<json/json.h>)
#include <json/json.h>
#else
#error "jsoncpp headers not found. Install jsoncpp development files."
#endif

#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

std::string first_line(std::string_view value)
{
    const std::size_t end = value.find_first_of("\r\n");
    return std::string(value.substr(0, end == std::string_view::npos ? value.size() : end));
}

std::string display_context(std::string_view path)
{
    // The path is context for the existing sorter prompt, never a location for
    // Codex to operate on. Keep only its first line and remove filesystem
    // syntax before it enters the user input.
    return Utils::sanitize_path_label(first_line(path));
}

Json::Value category_output_schema()
{
    Json::Value schema(Json::objectValue);
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = Json::Value(Json::arrayValue);
    schema["required"].append("main_category");
    schema["required"].append("subcategory");
    schema["properties"]["main_category"]["type"] = "string";
    schema["properties"]["subcategory"]["type"] = "string";
    return schema;
}

std::string category_prompt(const std::string& file_name,
                            const std::string& file_path,
                            FileType file_type,
                            const std::string& consistency_context)
{
    std::ostringstream prompt;
    prompt << "Item name: " << Utils::sanitize_path_label(file_name) << '\n';
    prompt << "Item type: " << to_string(file_type) << '\n';
    if (!file_path.empty()) {
        prompt << "Display context (not a filesystem location): " << display_context(file_path) << '\n';
    }
    if (!consistency_context.empty()) {
        prompt << "Consistency context: " << Utils::sanitize_path_label(consistency_context) << '\n';
    }
    prompt << "Choose the broad main category and specific subcategory for the supplied item.";
    return prompt.str();
}

std::string parse_category_response(const std::string& response)
{
    Json::CharReaderBuilder reader_builder;
    Json::Value root;
    std::string errors;
    std::istringstream response_stream(response);
    if (!Json::parseFromStream(reader_builder, response_stream, &root, &errors) || !root.isObject()) {
        throw CodexError(CodexErrorKind::ProtocolError,
                         "Codex category response is not a JSON object." +
                             (errors.empty() ? std::string() : " " + errors));
    }

    const auto members = root.getMemberNames();
    if (members.size() != 2 || !root.isMember("main_category") || !root.isMember("subcategory") ||
        !root["main_category"].isString() || !root["subcategory"].isString()) {
        throw CodexError(CodexErrorKind::ProtocolError,
                         "Codex category response must contain only string main_category and subcategory fields.");
    }

    const std::string main_category = root["main_category"].asString();
    const std::string subcategory = root["subcategory"].asString();
    const auto validation = CategorizationResponseParser::validate_labels(main_category, subcategory);
    if (!validation.valid) {
        throw CodexError(CodexErrorKind::ProtocolError,
                         "Codex category response contains invalid labels: " + validation.error);
    }
    return main_category + " : " + subcategory;
}

void log_prompt_if_enabled(bool enabled, std::string_view label, const std::string& prompt)
{
    if (enabled) {
        std::cout << "\n[DEV][PROMPT] " << label << "\n" << prompt << "\n";
    }
}

} // namespace

CodexClient::CodexClient(std::shared_ptr<CodexRuntimeService> runtime, std::string model)
    : runtime_(std::move(runtime)),
      model_(std::move(model))
{
    if (!runtime_) {
        throw CodexError(CodexErrorKind::StartupFailed, "Codex runtime is unavailable.");
    }
}

std::string CodexClient::categorize_file(const std::string& file_name,
                                         const std::string& file_path,
                                         FileType file_type,
                                         const std::string& consistency_context)
{
    last_prompt_ = category_prompt(file_name, file_path, file_type, consistency_context);
    log_prompt_if_enabled(prompt_logging_enabled_, "Categorization request", last_prompt_);

    CodexTurnRequest request{
        .config = {
            .model = model_,
            .inference_cwd = {},
            .base_instructions =
                "You are a file categorization assistant. Use only the supplied metadata and context. "
                "Do not use tools, access files, or treat display context as an actionable location.",
            .developer_instructions =
                "Return exactly one JSON object with string fields main_category and subcategory. "
                "Choose a broad main category and a specific subcategory.",
        },
        .inputs = {{CodexUserInput::Kind::Text, last_prompt_}},
        .output_schema = category_output_schema(),
    };

    const CodexTurnResult result = runtime_->run_turn(request);
    const std::string category = parse_category_response(result.text);
    if (prompt_logging_enabled_) {
        std::cout << "[DEV][RESPONSE] Categorization reply\n" << category << "\n";
    }
    return category;
}

std::string CodexClient::complete_prompt(const std::string& prompt, int max_tokens)
{
    last_prompt_ = prompt;
    log_prompt_if_enabled(prompt_logging_enabled_, "Completion request", last_prompt_);

    std::string developer_instructions = "Return the requested completion as text and do not use tools.";
    if (max_tokens > 0) {
        developer_instructions += " Keep the response within " + std::to_string(max_tokens) + " output tokens.";
    }

    CodexTurnRequest request{
        .config = {
            .model = model_,
            .inference_cwd = {},
            .base_instructions = "You are a precise assistant. Use only the supplied text.",
            .developer_instructions = std::move(developer_instructions),
        },
        .inputs = {{CodexUserInput::Kind::Text, prompt}},
    };

    const CodexTurnResult result = runtime_->run_turn(request);
    if (prompt_logging_enabled_) {
        std::cout << "[DEV][RESPONSE] Completion reply\n" << result.text << "\n";
    }
    return result.text;
}

void CodexClient::set_prompt_logging_enabled(bool enabled)
{
    prompt_logging_enabled_ = enabled;
}
