#include <catch2/catch_test_macros.hpp>

#include "CodexProtocol.hpp"

#if __has_include(<jsoncpp/json/json.h>)
#include <jsoncpp/json/json.h>
#elif __has_include(<json/json.h>)
#include <json/json.h>
#else
#error "jsoncpp headers not found. Install jsoncpp development files."
#endif

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

Json::Value parse_json(const std::string& text)
{
    Json::CharReaderBuilder builder;
    Json::Value value;
    std::string errors;
    std::istringstream stream(text);
    REQUIRE(Json::parseFromStream(builder, stream, &value, &errors));
    return value;
}

} // namespace

TEST_CASE("CodexProtocol builds stable managed ChatGPT and inference parameters")
{
    const Json::Value initialize = CodexProtocol::make_initialize_params("1.2.3");
    CHECK(initialize["clientInfo"]["name"].asString() == "ai_file_sorter");
    CHECK(initialize["clientInfo"]["title"].asString() == "AI File Sorter");
    CHECK(initialize["clientInfo"]["version"].asString() == "1.2.3");
    CHECK_FALSE(initialize.isMember("jsonrpc"));
    CHECK_FALSE(initialize.isMember("capabilities"));

    const Json::Value login = CodexProtocol::make_chatgpt_login_params();
    CHECK(login["type"].asString() == "chatgpt");
    CHECK_FALSE(login.isMember("chatgptAuthTokens"));
    CHECK_FALSE(login.isMember("accessToken"));

    const CodexInferenceConfig config{
        .model = "vision-model",
        .inference_cwd = "C:/AI File Sorter/inference",
        .base_instructions = "Return supplied analysis only.",
        .developer_instructions = "Do not use tools.",
    };
    const Json::Value thread = CodexProtocol::make_thread_start_params(config);
    CHECK(thread["model"].asString() == "vision-model");
    CHECK(thread["ephemeral"].asBool());
    CHECK(thread["approvalPolicy"].asString() == "never");
    CHECK(thread["sandbox"].asString() == "read-only");
    CHECK(thread["cwd"].asString() == "C:/AI File Sorter/inference");
    CHECK(thread["baseInstructions"].asString() == "Return supplied analysis only.");
    CHECK(thread["developerInstructions"].asString() == "Do not use tools.");
    CHECK_FALSE(thread.isMember("permissions"));
    CHECK_FALSE(thread.isMember("environments"));
}

TEST_CASE("CodexProtocol serializes only text and data URL image inputs")
{
    const std::vector<CodexUserInput> inputs{
        {CodexUserInput::Kind::Text, "Describe this image."},
        {CodexUserInput::Kind::ImageDataUrl, "data:image/png;base64,iVBORw0KGgo="},
    };
    const Json::Value schema = parse_json(R"({"type":"object","properties":{"description":{"type":"string"}}})");

    const Json::Value turn = CodexProtocol::make_turn_start_params("thread-123", inputs, schema);
    REQUIRE(turn["input"].isArray());
    REQUIRE(turn["input"].size() == 2);
    CHECK(turn["threadId"].asString() == "thread-123");
    CHECK(turn["input"][0]["type"].asString() == "text");
    CHECK(turn["input"][0]["text"].asString() == "Describe this image.");
    CHECK(turn["input"][1]["type"].asString() == "image");
    CHECK(turn["input"][1]["imageUrl"].asString() == "data:image/png;base64,iVBORw0KGgo=");
    CHECK_FALSE(turn["input"][1].isMember("path"));
    CHECK(turn["outputSchema"] == schema);

    const std::vector<CodexUserInput> path_input{
        {CodexUserInput::Kind::ImageDataUrl, "C:/private/source.png"},
    };
    CHECK_THROWS_AS(CodexProtocol::make_turn_start_params("thread-123", path_input, Json::Value()), CodexError);
}

TEST_CASE("CodexProtocol parses managed account and model capabilities")
{
    const auto account = CodexProtocol::parse_account_read_response(parse_json(R"({
      "account": {"type":"chatgpt","email":"me@example.com","planType":"plus"}
    })"));
    REQUIRE(account.authenticated);
    CHECK(account.email == "me@example.com");
    CHECK(account.plan_type == "plus");

    const auto models = CodexProtocol::parse_model_list_response(parse_json(R"({
      "data":[
        {"id":"text","displayName":"Text","inputModalities":["text"],"isDefault":false},
        {"id":"vision","displayName":"Vision","inputModalities":["text","image"],"isDefault":true}
      ],
      "nextCursor":null
    })"));
    REQUIRE(models.size() == 2);
    CHECK_FALSE(models[0].accepts_image);
    CHECK(models[1].accepts_image);
    CHECK(models[1].is_default);
}

TEST_CASE("CodexProtocol parses responses and account notifications without OAuth tokens")
{
    const auto response = CodexProtocol::parse_response(parse_json(R"({
      "id":42,"result":{"thread":{"id":"thread-123"}}
    })"));
    REQUIRE(response.id == 42);
    CHECK(response.result["thread"]["id"].asString() == "thread-123");

    const auto update = CodexProtocol::parse_account_notification(parse_json(R"({
      "method":"account/updated","params":{"authMode":"chatgpt","planType":"pro"}
    })"));
    REQUIRE(update.has_value());
    CHECK(update->authenticated);
    CHECK(update->plan_type == "pro");

    CHECK_FALSE(CodexProtocol::parse_account_notification(parse_json(R"({
      "method":"turn/started","params":{}
    })")).has_value());

    const auto login = CodexProtocol::parse_account_notification(parse_json(R"({
      "method":"account/login/completed","params":{"loginId":"login-123","success":true,"error":null}
    })"));
    REQUIRE(login.has_value());
    CHECK(login->kind == CodexAccountNotificationKind::LoginCompleted);
    CHECK(login->login_id == "login-123");
    CHECK(login->success);

    const auto rate_limits = CodexProtocol::parse_account_notification(parse_json(R"({
      "method":"account/rateLimits/updated","params":{"rateLimits":{"limitId":"codex"}}
    })"));
    REQUIRE(rate_limits.has_value());
    CHECK(rate_limits->kind == CodexAccountNotificationKind::RateLimitsUpdated);
    CHECK(rate_limits->rate_limits["limitId"].asString() == "codex");
}

TEST_CASE("CodexProtocol accumulates message deltas and parses completed turn usage")
{
    std::string text;
    CHECK(CodexProtocol::append_agent_message_delta(parse_json(R"({
      "method":"item/agentMessage/delta",
      "params":{"threadId":"thread-123","turnId":"turn-123","delta":"Hello "}
    })"), "thread-123", "turn-123", text));
    CHECK(CodexProtocol::append_agent_message_delta(parse_json(R"({
      "method":"item/agentMessage/delta",
      "params":{"threadId":"thread-123","turnId":"turn-123","delta":"world"}
    })"), "thread-123", "turn-123", text));
    CHECK(text == "Hello world");
    CHECK_FALSE(CodexProtocol::append_agent_message_delta(parse_json(R"({
      "method":"item/agentMessage/delta",
      "params":{"threadId":"other","turnId":"turn-123","delta":"ignored"}
    })"), "thread-123", "turn-123", text));

    const auto completion = CodexProtocol::parse_turn_completed_notification(parse_json(R"({
      "method":"turn/completed",
      "params":{"turn":{"id":"turn-123","status":"completed","usage":{"inputTokens":12,"outputTokens":34}}}
    })"));
    REQUIRE(completion.has_value());
    CHECK(completion->turn_id == "turn-123");
    CHECK(completion->status == CodexTurnStatus::Completed);
    CHECK(completion->usage.input_tokens == 12);
    CHECK(completion->usage.output_tokens == 34);
}

TEST_CASE("CodexProtocol maps overloaded RPC errors to recoverable rate limits")
{
    const Json::Value overload = parse_json(R"({
      "id":9,"error":{"code":-32001,"message":"Server overloaded; retry later."}
    })");

    try {
        static_cast<void>(CodexProtocol::parse_response(overload));
        FAIL("Expected CodexError");
    } catch (const CodexError& error) {
        CHECK(error.kind() == CodexErrorKind::RateLimited);
        CHECK(std::string(error.what()).find("retry") != std::string::npos);
    }
}
