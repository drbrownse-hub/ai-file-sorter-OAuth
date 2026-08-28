#include "CodexImageAnalyzer.hpp"

#include "CodexRuntimeService.hpp"
#include "Utils.hpp"

#if __has_include(<jsoncpp/json/json.h>)
#include <jsoncpp/json/json.h>
#elif __has_include(<json/json.h>)
#include <json/json.h>
#else
#error "jsoncpp headers not found. Install jsoncpp development files."
#endif

#include <QBuffer>
#include <QImageReader>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr int kMaxImageDimension = 2048;

Json::Value visual_output_schema()
{
    Json::Value schema(Json::objectValue);
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = Json::Value(Json::arrayValue);
    schema["required"].append("description");
    schema["required"].append("suggested_name");
    schema["properties"]["description"]["type"] = "string";
    schema["properties"]["suggested_name"]["type"] = "string";
    return schema;
}

std::string parse_visual_response(const std::string& response, std::string& suggested_name)
{
    Json::CharReaderBuilder reader_builder;
    Json::Value root;
    std::string errors;
    std::istringstream response_stream(response);
    if (!Json::parseFromStream(reader_builder, response_stream, &root, &errors) || !root.isObject()) {
        throw CodexError(CodexErrorKind::ProtocolError,
                         "Codex visual response is not a JSON object." +
                             (errors.empty() ? std::string() : " " + errors));
    }

    const auto members = root.getMemberNames();
    if (members.size() != 2 || !root.isMember("description") || !root.isMember("suggested_name") ||
        !root["description"].isString() || !root["suggested_name"].isString()) {
        throw CodexError(CodexErrorKind::ProtocolError,
                         "Codex visual response must contain only string description and suggested_name fields.");
    }

    suggested_name = root["suggested_name"].asString();
    return root["description"].asString();
}

std::string encode_png_data_url(const std::filesystem::path& image_path)
{
    QImageReader reader(QString::fromStdString(Utils::path_to_utf8(image_path)));
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull()) {
        throw CodexError(CodexErrorKind::TurnFailed, "Could not decode image for Codex visual analysis.");
    }

    if (std::max(image.width(), image.height()) > kMaxImageDimension) {
        image = image.scaled(kMaxImageDimension,
                             kMaxImageDimension,
                             Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }

    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
        throw CodexError(CodexErrorKind::TurnFailed, "Could not encode image for Codex visual analysis.");
    }
    return "data:image/png;base64," + QByteArray(png.toBase64()).toStdString();
}

} // namespace

CodexImageAnalyzer::CodexImageAnalyzer(std::shared_ptr<CodexRuntimeService> runtime, std::string model)
    : runtime_(std::move(runtime)), model_(std::move(model))
{
    if (!runtime_) {
        throw CodexError(CodexErrorKind::StartupFailed, "Codex runtime is unavailable.");
    }
}

ImageAnalysisResult CodexImageAnalyzer::analyze(const std::filesystem::path& image_path)
{
    if (!runtime_->selected_model_accepts_images(model_)) {
        throw CodexError(CodexErrorKind::ImageUnsupported,
                         "The selected Codex model does not support image input.");
    }

    const std::string image_data_url = encode_png_data_url(image_path);
    CodexTurnRequest request{
        .config = {
            .model = model_,
            .inference_cwd = {},
            .base_instructions =
                "You are a visual file-analysis assistant. Use only the pixels supplied in the image input. "
                "Do not use tools, access files, or infer an actionable source location.",
            .developer_instructions =
                "Return exactly one JSON object with string fields description and suggested_name. "
                "Make the description concise and the filename safe and descriptive.",
        },
        .inputs = {
            {CodexUserInput::Kind::Text,
             "Analyze the supplied image and return its description and a concise suggested filename."},
            {CodexUserInput::Kind::ImageDataUrl, image_data_url},
        },
        .output_schema = visual_output_schema(),
    };

    const CodexTurnResult result = runtime_->run_turn(request);
    std::string suggested_name;
    const std::string description = parse_visual_response(result.text, suggested_name);
    return {
        .description = description,
        .suggested_name = std::move(suggested_name),
        .diagnostics = {},
    };
}
