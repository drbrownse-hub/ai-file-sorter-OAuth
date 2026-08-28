/**
 * @file ImageAnalyzerFactory.hpp
 * @brief Factory for creating analyzer instances from resolved visual backends.
 */
#pragma once

#include "ImageAnalyzer.hpp"
#include "VisualLlmRuntime.hpp"

#include <functional>
#include <memory>

/**
 * @brief Creates image analyzers for resolved visual model backends.
 */
class ImageAnalyzerFactory {
public:
    /**
     * @brief Create an analyzer for the resolved visual backend.
     * @param backend Resolved backend descriptor and artifact paths.
     * @param settings Analyzer settings to apply.
     * @return Analyzer instance ready for inference.
     */
    static std::unique_ptr<ImageAnalyzer> create(const VisualLlmRuntime::Backend& backend,
                                                 ImageAnalyzerSettings settings = {});

#ifdef AI_FILE_SORTER_TEST_BUILD
    /**
     * @brief Test-only override for avoiding native visual model startup.
     * @param probe Callback used instead of constructing the native analyzer.
     */
    using TestCreateProbe = std::function<std::unique_ptr<ImageAnalyzer>(
        const VisualLlmRuntime::Backend&, const ImageAnalyzerSettings&)>;

    static void set_test_create_probe(TestCreateProbe probe);
    static void reset_test_create_probe();
#endif
};
