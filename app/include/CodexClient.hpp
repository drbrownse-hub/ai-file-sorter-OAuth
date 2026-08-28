#pragma once

#include "ILLMClient.hpp"

#include <memory>
#include <string>

class CodexRuntimeService;

class CodexClient final : public ILLMClient {
public:
    CodexClient(std::shared_ptr<CodexRuntimeService> runtime, std::string model);
    ~CodexClient() override = default;

    std::string categorize_file(const std::string& file_name,
                                const std::string& file_path,
                                FileType file_type,
                                const std::string& consistency_context) override;
    std::string complete_prompt(const std::string& prompt,
                                int max_tokens) override;
    void set_prompt_logging_enabled(bool enabled) override;

private:
    std::shared_ptr<CodexRuntimeService> runtime_;
    std::string model_;
    bool prompt_logging_enabled_{false};
    std::string last_prompt_;
};
