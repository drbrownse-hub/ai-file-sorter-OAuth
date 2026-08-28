#if __has_include(<jsoncpp/json/json.h>)
#include <jsoncpp/json/json.h>
#elif __has_include(<json/json.h>)
#include <json/json.h>
#else
#error "jsoncpp headers not found. Install jsoncpp development files."
#endif

#include <QCoreApplication>
#include <QTimer>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

void append_event(const std::string& event)
{
    const char* path = std::getenv("AIFS_FAKE_CODEX_EVENT_LOG");
    if (path == nullptr || *path == '\0') {
        return;
    }
    std::ofstream stream(path, std::ios::app);
    stream << event << '\n';
}

bool is_runtime_scenario(const std::string& scenario)
{
    return scenario == "runtime-authenticated" || scenario == "serialize-turns" ||
           scenario == "crash-once" || scenario == "crash-always" || scenario == "delayed-turn" ||
           scenario == "auth-loss" || scenario == "auth-loss-delayed-turn" ||
           scenario == "failed-turn" || scenario == "login-flows" || scenario == "login-failure" ||
           scenario == "image-analyzer" || scenario == "image-analyzer-malformed-missing" ||
           scenario == "image-analyzer-malformed-nonstring" || scenario == "image-analyzer-malformed-extra" ||
           scenario == "client-category" || scenario == "client-generic" ||
           scenario == "client-rate-limited" || scenario == "client-invalid-missing" ||
           scenario == "client-invalid-nonstring" || scenario == "client-invalid-extra";
}

class FakeCodexAppServer final : public QObject {
public:
    explicit FakeCodexAppServer(std::string scenario)
        : scenario_(std::move(scenario)),
          authenticated_(scenario_ != "login-flows" && scenario_ != "login-failure" && scenario_ != "auth-required")
    {
        append_event("app-server-start cwd=" + std::filesystem::current_path().string() +
                     " codex-home=" + (std::getenv("CODEX_HOME") == nullptr ? std::string() :
                                           std::getenv("CODEX_HOME")));
    }

    void handle_line(const std::string& line)
    {
        Json::CharReaderBuilder reader_builder;
        Json::Value request;
        std::string errors;
        std::istringstream stream(line);
        if (!Json::parseFromStream(reader_builder, stream, &request, &errors)) {
            return;
        }

        const std::string method = request["method"].asString();
        append_event("request " + method);
        if (method == "turn/start") {
            Json::StreamWriterBuilder writer_builder;
            writer_builder["indentation"] = "";
            append_event("turn-request " + Json::writeString(writer_builder, request));
        }
        if (scenario_ == "malformed-json" && method == "initialize") {
            std::cout << "{not json\n" << std::flush;
            return;
        }

        if (method == "initialized") {
            ++initialized_count_;
            if (initialized_count_ > 1) {
                write_error(request["id"], -32600, "initialized sent more than once");
            }
            return;
        }
        if (!request.isMember("id")) {
            return;
        }

        if (method == "initialize") {
            if (scenario_ == "crash-initialize") {
                std::cerr << "fake child crashed during initialization" << std::flush;
                std::_Exit(23);
            }
            if (scenario_ == "delayed-initialize") {
                const Json::Value request_id = request["id"];
                QTimer::singleShot(50, this, [this, request_id] {
                    write_result(request_id, Json::Value(Json::objectValue));
                });
                return;
            }
            write_result(request["id"], Json::Value(Json::objectValue));
            return;
        }
        if (scenario_ == "notification-flood" && method == "account/read") {
            for (int index = 0; index < 1024; ++index) {
                Json::Value params(Json::objectValue);
                params["index"] = index;
                write_notification("test/noise", params);
            }
            Json::Value result(Json::objectValue);
            result["scenario"] = scenario_;
            write_result(request["id"], result);
            return;
        }
        if (method == "account/read" && is_runtime_scenario(scenario_)) {
            Json::Value result(Json::objectValue);
            if (authenticated_) {
                result["account"]["type"] = "chatgpt";
                result["account"]["email"] = "runtime@example.com";
                result["account"]["planType"] = "plus";
            }
            write_result(request["id"], result);
            if ((scenario_ == "auth-loss" || scenario_ == "auth-loss-delayed-turn") &&
                !auth_loss_scheduled_) {
                auth_loss_scheduled_ = true;
                QTimer::singleShot(15, this, [this] {
                    authenticated_ = false;
                    Json::Value params(Json::objectValue);
                    params["authMode"] = "none";
                    write_notification("account/updated", params);
                });
            }
            return;
        }
        if (method == "model/list" && is_runtime_scenario(scenario_)) {
            Json::Value result(Json::objectValue);
            Json::Value text_model(Json::objectValue);
            text_model["id"] = "text-only";
            text_model["displayName"] = "Text only";
            text_model["isDefault"] = false;
            text_model["inputModalities"].append("text");
            result["data"].append(text_model);

            Json::Value image_model(Json::objectValue);
            image_model["id"] = "image-default";
            image_model["displayName"] = "Image default";
            image_model["isDefault"] = true;
            image_model["inputModalities"].append("text");
            image_model["inputModalities"].append("image");
            result["data"].append(image_model);
            write_result(request["id"], result);
            return;
        }
        if (scenario_ == "authenticated" && method == "test/outer") {
            outer_id_ = request["id"];
            write_notification("test/queued", Json::Value(Json::objectValue));
            return;
        }
        if (scenario_ == "authenticated" && method == "test/inner") {
            Json::Value inner(Json::objectValue);
            inner["sequence"] = "inner";
            write_result(request["id"], inner);
            Json::Value outer(Json::objectValue);
            outer["sequence"] = "outer";
            write_result(outer_id_, outer);
            return;
        }
        if (scenario_ == "rate-limited") {
            write_error(request["id"], -32001, "Server overloaded; retry later.");
            return;
        }
        if (method == "thread/start") {
            if (scenario_ == "slow-thread-start") {
                return;
            }
            append_event("thread-start model=" + request["params"]["model"].asString() +
                         " cwd=" + request["params"]["cwd"].asString());
            Json::Value result(Json::objectValue);
            ++turn_sequence_;
            result["thread"]["id"] = scenario_ == "turn-notification-flood-before-response"
                                             ? "fake-thread"
                                             : "fake-thread-" + std::to_string(turn_sequence_);
            write_result(request["id"], result);
            return;
        }
        if (method == "turn/start") {
            if (scenario_ == "slow-turn-start") {
                return;
            }
            if (scenario_ == "turn-notification-flood-before-response") {
                for (int index = 0; index < 300; ++index) {
                    Json::Value delta(Json::objectValue);
                    delta["threadId"] = "fake-thread";
                    delta["turnId"] = "fake-turn";
                    delta["delta"] = "x";
                    write_notification("item/agentMessage/delta", delta);
                }
                Json::Value completed(Json::objectValue);
                completed["threadId"] = "fake-thread";
                completed["turn"]["id"] = "fake-turn";
                completed["turn"]["status"] = "completed";
                completed["turn"]["usage"]["inputTokens"] = 3;
                completed["turn"]["usage"]["outputTokens"] = 5;
                write_notification("turn/completed", completed);

                Json::Value result(Json::objectValue);
                result["turn"]["id"] = "fake-turn";
                write_result(request["id"], result);
                return;
            }
            if (scenario_ == "client-rate-limited") {
                write_error(request["id"], -32001, "Server overloaded; retry later.");
                return;
            }
            const std::string thread_id = request["params"]["threadId"].asString();
            const std::string turn_id = "fake-turn-" + std::to_string(turn_sequence_);
            append_event("turn-start " + turn_id);
            Json::Value result(Json::objectValue);
            result["turn"]["id"] = turn_id;
            write_result(request["id"], result);

            if (scenario_ == "crash-after-turn-start") {
                std::cerr << "fake child crashed after turn start" << std::flush;
                QTimer::singleShot(5, [] { std::_Exit(17); });
                return;
            }
            if (scenario_ == "crash-once" || scenario_ == "crash-always") {
                const char* marker_path = std::getenv("AIFS_FAKE_CODEX_CRASH_MARKER");
                if (scenario_ == "crash-always" ||
                    (marker_path != nullptr && !std::filesystem::exists(marker_path))) {
                    if (marker_path != nullptr && scenario_ == "crash-once") {
                        std::ofstream(marker_path) << "crashed\n";
                    }
                    append_event("turn-crash " + turn_id);
                    std::cerr << "fake runtime crashed after turn start" << std::flush;
                    QTimer::singleShot(5, [] { std::_Exit(19); });
                    return;
                }
            }
            if (scenario_ == "slow-turn") {
                return;
            }
            bool has_image = false;
            for (const Json::Value& input : request["params"]["input"]) {
                has_image = has_image || input["type"].asString() == "image";
            }
            const int delay = scenario_ == "serialize-turns" ? 40
                              : (scenario_ == "delayed-turn" || scenario_ == "auth-loss-delayed-turn") ? 80
                                                                                                         : 5;
            QTimer::singleShot(delay, this, [this, thread_id, turn_id, has_image] {
                emit_turn_notifications(thread_id, turn_id, has_image);
            });
            return;
        }
        if (method == "account/login/start" &&
            (scenario_ == "login-success" || scenario_ == "login-flows" || scenario_ == "login-failure")) {
            Json::Value result(Json::objectValue);
            result["loginId"] = "fake-login";
            const std::string type = request["params"]["type"].asString();
            if (type == "chatgpt") {
                result["authUrl"] = "https://example.invalid/fake-login";
            } else if (type == "chatgptDeviceCode") {
                result["verificationUrl"] = "https://example.invalid/device";
                result["userCode"] = "ABCD-EFGH";
            } else {
                write_error(request["id"], -32602, "unexpected login type");
                return;
            }
            write_result(request["id"], result);
            QTimer::singleShot(5, this, [this] {
                authenticated_ = scenario_ != "login-failure";
                Json::Value params(Json::objectValue);
                params["loginId"] = "fake-login";
                params["success"] = scenario_ != "login-failure";
                if (scenario_ == "login-failure") {
                    params["error"] = "browser login was cancelled";
                }
                write_notification("account/login/completed", params);
            });
            return;
        }
        if (method == "account/logout" && is_runtime_scenario(scenario_)) {
            authenticated_ = false;
            write_result(request["id"], Json::Value(Json::objectValue));
            Json::Value params(Json::objectValue);
            params["authMode"] = "none";
            write_notification("account/updated", params);
            return;
        }

        Json::Value result(Json::objectValue);
        result["scenario"] = scenario_;
        write_result(request["id"], result);
    }

private:
    void emit_turn_notifications(const std::string& thread_id, const std::string& turn_id, bool has_image)
    {
        Json::Value delta(Json::objectValue);
        delta["threadId"] = thread_id;
        delta["turnId"] = turn_id;
        delta["delta"] = completion_text(has_image);
        write_notification("item/agentMessage/delta", delta);

        Json::Value completed(Json::objectValue);
        completed["threadId"] = thread_id;
        completed["turn"]["id"] = turn_id;
        completed["turn"]["status"] = scenario_ == "failed-turn" ? "failed" : "completed";
        if (scenario_ == "failed-turn") {
            completed["turn"]["error"]["message"] = "fake turn failed";
        }
        completed["turn"]["usage"]["inputTokens"] = 3;
        completed["turn"]["usage"]["outputTokens"] = 5;
        write_notification("turn/completed", completed);
        append_event("turn-complete " + turn_id);
    }

    std::string completion_text(bool has_image) const
    {
        if (scenario_ == "image-analyzer" || scenario_ == "image-analyzer-malformed-missing" ||
            scenario_ == "image-analyzer-malformed-nonstring" || scenario_ == "image-analyzer-malformed-extra") {
            Json::Value visual(Json::objectValue);
            visual["description"] = "a red square";
            visual["suggested_name"] = "red-square.png";
            if (scenario_ == "image-analyzer-malformed-missing") {
                visual.removeMember("suggested_name");
            } else if (scenario_ == "image-analyzer-malformed-nonstring") {
                visual["suggested_name"] = 42;
            } else if (scenario_ == "image-analyzer-malformed-extra") {
                visual["unexpected"] = "not allowed";
            }
            Json::StreamWriterBuilder writer_builder;
            writer_builder["indentation"] = "";
            return Json::writeString(writer_builder, visual);
        }
        if (has_image || scenario_ == "vision-turn") {
            return "image description";
        }
        if (scenario_ == "client-category") {
            Json::Value category(Json::objectValue);
            category["main_category"] = "Research article";
            category["subcategory"] = "Microbiology";
            Json::StreamWriterBuilder writer_builder;
            writer_builder["indentation"] = "";
            return Json::writeString(writer_builder, category);
        }
        if (scenario_ == "client-generic") {
            return "generic completion";
        }
        if (scenario_ == "client-invalid-missing") {
            Json::Value category(Json::objectValue);
            category["main_category"] = "Research article";
            Json::StreamWriterBuilder writer_builder;
            writer_builder["indentation"] = "";
            return Json::writeString(writer_builder, category);
        }
        if (scenario_ == "client-invalid-nonstring") {
            Json::Value category(Json::objectValue);
            category["main_category"] = "Research article";
            category["subcategory"] = 7;
            Json::StreamWriterBuilder writer_builder;
            writer_builder["indentation"] = "";
            return Json::writeString(writer_builder, category);
        }
        if (scenario_ == "client-invalid-extra") {
            Json::Value category(Json::objectValue);
            category["main_category"] = "Research article";
            category["subcategory"] = "Microbiology";
            category["unexpected"] = "not allowed";
            Json::StreamWriterBuilder writer_builder;
            writer_builder["indentation"] = "";
            return Json::writeString(writer_builder, category);
        }
        return "fake completion";
    }

    void write_result(const Json::Value& id, const Json::Value& result)
    {
        Json::Value response(Json::objectValue);
        response["id"] = id;
        response["result"] = result;
        write_message(response);
    }

    void write_error(const Json::Value& id, int code, const std::string& message)
    {
        Json::Value response(Json::objectValue);
        response["id"] = id;
        response["error"]["code"] = code;
        response["error"]["message"] = message;
        write_message(response);
    }

    void write_notification(const std::string& method, const Json::Value& params)
    {
        Json::Value notification(Json::objectValue);
        notification["method"] = method;
        notification["params"] = params;
        write_message(notification);
    }

    static void write_message(const Json::Value& message)
    {
        Json::StreamWriterBuilder writer_builder;
        writer_builder["indentation"] = "";
        std::cout << Json::writeString(writer_builder, message) << '\n' << std::flush;
    }

    std::string scenario_;
    Json::Value outer_id_;
    int initialized_count_{0};
    int turn_sequence_{0};
    bool authenticated_{false};
    bool auth_loss_scheduled_{false};
};

} // namespace

int main(int argc, char* argv[])
{
    const char* scenario_value = std::getenv("AIFS_FAKE_CODEX_SCENARIO");
    const std::string scenario = scenario_value == nullptr ? "authenticated" : scenario_value;
    if (argc > 1 && std::string(argv[1]) == "--version") {
        append_event("version " + std::filesystem::absolute(argv[0]).string());
        if (scenario == "version-fail") {
            std::cerr << "fake version probe failed\n";
            return 9;
        }
        std::cout << "fake-codex 1.0 " << std::filesystem::absolute(argv[0]).string() << '\n';
        return 0;
    }

    QCoreApplication application(argc, argv);
    auto* server = new FakeCodexAppServer(scenario);

    std::thread reader([server] {
        std::string line;
        while (std::getline(std::cin, line)) {
            QMetaObject::invokeMethod(server, [server, line] { server->handle_line(line); }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(qApp, [] { QCoreApplication::quit(); }, Qt::QueuedConnection);
    });
    reader.detach();

    return application.exec();
}
