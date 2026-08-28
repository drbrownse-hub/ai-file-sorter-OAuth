#pragma once

#include "ImageAnalyzer.hpp"

#include <filesystem>
#include <memory>
#include <string>

class CodexRuntimeService;

class CodexImageAnalyzer final : public ImageAnalyzer {
public:
    CodexImageAnalyzer(std::shared_ptr<CodexRuntimeService> runtime, std::string model);
    ~CodexImageAnalyzer() override = default;

    ImageAnalysisResult analyze(const std::filesystem::path& image_path) override;

private:
    std::shared_ptr<CodexRuntimeService> runtime_;
    std::string model_;
};
