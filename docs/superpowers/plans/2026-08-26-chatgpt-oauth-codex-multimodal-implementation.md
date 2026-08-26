# ChatGPT OAuth + Codex Multimodal Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Windows-first ChatGPT-subscription backend to AI File Sorter through standalone `codex.exe app-server`, with official Codex-managed OAuth, one shared thread-safe runtime, dynamic model/capability discovery, and optional native ChatGPT image analysis without breaking existing local/API-key backends.

**Architecture:** Keep the existing `ILLMClient` and `ImageAnalyzer` contracts. Add `CodexClient` for text/categorization and `CodexImageAnalyzer` for native image understanding; both depend on one application-scoped `CodexRuntimeService`. That service owns a dedicated Qt thread and a single `CodexAppServer`/`QProcess`, so analysis worker threads never touch `QProcess` directly. ChatGPT vision is represented as an additional visual-backend id and is injected into `AnalysisCoordinator`; local visual analysis keeps the existing `VisualLlmRuntime`/`ImageAnalyzerFactory` path.

**Tech Stack:** C++20, Qt 6 (`QObject`, `QThread`, `QProcess`, `QEventLoop`, `QTimer`, `QImageReader`, `QBuffer`, `QDesktopServices`), JsonCpp, Catch2 v3, CMake, Codex app-server JSONL/JSON-RPC over stdio.

**Spec:** `docs/superpowers/specs/2026-08-26-chatgpt-oauth-codex-app-server-design.md`

## Global Constraints

- Follow strict TDD for every production behavior: write one focused test, run it and verify the expected failure, add the minimum production code, run the focused test again, then run the broader test target before committing.
- Do not implement or store OpenAI access/refresh tokens. Only use app-server `account/login/start`, `account/read`, `account/logout`, and Codex-owned auth persistence under the dedicated `CODEX_HOME`.
- Do not use the unstable/internal `chatgptAuthTokens` login mode.
- Use one app-server process for the application. `CodexClient`, `CodexImageAnalyzer`, dialogs, and analysis workers share it.
- The `QProcess` must live and die on its owning Qt runtime thread.
- For v1, serialize Codex inference turns across text and image requests.
- Use fresh ephemeral Codex threads for classifier/vision inference.
- Keep approval policy `never`, an empty dedicated inference cwd, read-only sandbox, no selected environments/MCP/plugins/skills, and no actionable source file path in model input.
- Native image analysis sends a normalized inline image data URI; do not use app-server `localImage` for source images.
- Preserve mixed configurations: ChatGPT text + ChatGPT vision, ChatGPT text + local vision, and local/API text + ChatGPT vision.
- Never silently downgrade requested ChatGPT visual analysis when the selected model lacks image input; expose an actionable unavailable/error state.
- No automated test may require a real ChatGPT account or an OpenAI network request.
- Do not auto-download or redistribute `codex.exe` in this implementation.
- Keep raw PDF/Office/audio/video Codex-native ingestion out of scope; existing document extraction remains unchanged.

---

## File / Responsibility Map

### Existing files to modify

- `app/include/Types.hpp` — add `LLMChoice::Remote_ChatGPT`; include it in remote-choice logic.
- `app/include/Settings.hpp`, `app/lib/Settings.cpp` — persist Codex executable path, ChatGPT model selection, `Remote_ChatGPT`, and the reserved ChatGPT visual-backend id.
- `app/include/AnalysisWorkflowContext.hpp` — add the remote image-analyzer injection point.
- `app/lib/AnalysisCoordinator.cpp` — branch ChatGPT visual analysis before local `VisualLlmRuntime` resolution while preserving the existing local GPU/CPU fallback path.
- `app/include/MainApp.hpp`, `app/lib/MainApp.cpp` — own the shared runtime; create `CodexClient`/`CodexImageAnalyzer`; validate ChatGPT readiness before analysis; update status text; pass the runtime to the selection dialog.
- `app/include/LLMSelectionVisualBackendModel.hpp`, `app/lib/LLMSelectionVisualBackendModel.cpp` — add a synthetic ChatGPT visual item without pretending it is a local `VisualModelDescriptor`.
- `app/include/LLMSelectionDialog.hpp`, `app/lib/LLMSelectionDialog.cpp` — add ChatGPT subscription text choice plus shared Codex runtime/account/model controls.
- `app/include/LLMSelectionDialogTestAccess.hpp` — expose only the new UI state needed by unit tests.
- `app/CMakeLists.txt` — register new unit tests and fake app-server helper; app `.cpp/.hpp` files are already globbed into production/test targets.
- `README.md`, `docs/configuration-and-environment.md`, `TESTS.md`, `TROUBLESHOOTING.md` — document setup, auth, multimodality, isolation, expected two-turn image usage, testing, and common failures.

### New production files

- `app/include/CodexBackendIds.hpp` — stable ids such as `chatgpt` for visual selection.
- `app/include/CodexProtocol.hpp`, `app/lib/CodexProtocol.cpp` — pure JsonCpp builders/parsers and typed protocol/result/error structures.
- `app/include/CodexAppServer.hpp`, `app/lib/CodexAppServer.cpp` — stdio JSONL framing, request correlation, notification capture, process lifecycle. Called only on its owning runtime thread.
- `app/include/CodexRuntimeService.hpp`, `app/lib/CodexRuntimeService.cpp` — app-scoped worker thread, runtime discovery, shared auth/model state, serialized inference API, cancellation/restart policy.
- `app/include/CodexClient.hpp`, `app/lib/CodexClient.cpp` — `ILLMClient` adapter with category schema and prompt/path sanitization.
- `app/include/CodexImageAnalyzer.hpp`, `app/lib/CodexImageAnalyzer.cpp` — `ImageAnalyzer` adapter using inline PNG data URLs and structured visual output.

### New tests/helpers

- `tests/unit/test_chatgpt_settings.cpp`
- `tests/unit/test_codex_protocol.cpp`
- `tests/helpers/fake_codex_app_server.cpp`
- `tests/unit/test_codex_app_server.cpp`
- `tests/unit/test_codex_runtime_service.cpp`
- `tests/unit/test_codex_client.cpp`
- `tests/unit/test_codex_image_analyzer.cpp`
- extend `tests/unit/test_llm_selection_visual_backend_model.cpp`
- extend `tests/unit/test_llm_selection_dialog_local.cpp` or add `tests/unit/test_llm_selection_dialog_chatgpt.cpp`
- extend `tests/unit/test_main_app_visual_fallback.cpp` / analysis-focused tests for mixed backend routing.

---

## Task 1: Add stable backend identifiers and settings persistence

**Files:**
- Create: `app/include/CodexBackendIds.hpp`
- Modify: `app/include/Types.hpp`
- Modify: `app/include/Settings.hpp`
- Modify: `app/lib/Settings.cpp`
- Create: `tests/unit/test_chatgpt_settings.cpp`
- Modify: `app/CMakeLists.txt` (test source list only)

**Interfaces:**
- **Consumes:** existing `Settings` config storage and `LLMChoice` serialization.
- **Produces:** `LLMChoice::Remote_ChatGPT`, `kChatGptVisualBackendId`, Codex executable/model getters and setters, round-trippable config values.

- [ ] **Step 1: Write the failing settings tests.**

Add tests that require all of these behaviors:

```cpp
TEST_CASE("ChatGPT backend settings round-trip without OAuth secrets") {
    QtAppContext qt;
    TempDir config_dir;
    EnvVarGuard guard("AI_FILE_SORTER_CONFIG_DIR", config_dir.path().string());

    {
        Settings settings;
        settings.load();
        settings.set_llm_choice(LLMChoice::Remote_ChatGPT);
        settings.set_codex_executable_path("C:/Tools/codex.exe");
        settings.set_chatgpt_model("gpt-5-codex");
        settings.set_visual_model_id(kChatGptVisualBackendId);
        REQUIRE(settings.save());
    }

    Settings reloaded;
    REQUIRE(reloaded.load());
    CHECK(reloaded.get_llm_choice() == LLMChoice::Remote_ChatGPT);
    CHECK(reloaded.get_codex_executable_path() == "C:/Tools/codex.exe");
    CHECK(reloaded.get_chatgpt_model() == "gpt-5-codex");
    CHECK(reloaded.get_visual_model_id() == kChatGptVisualBackendId);
}

TEST_CASE("ChatGPT account backend is a remote LLM choice") {
    CHECK(is_remote_choice(LLMChoice::Remote_ChatGPT));
}
```

Also inspect the written config and assert there is no new key containing `AccessToken`, `RefreshToken`, or `OAuthToken`.

- [ ] **Step 2: Run the focused test and verify RED.**

```bash
cmake -S app -B build-tests -DAI_FILE_SORTER_BUILD_TESTS=ON
cmake --build build-tests --target ai_file_sorter_tests
ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
```

Expected failure: compile/test failure because `Remote_ChatGPT`, Codex settings accessors, and `kChatGptVisualBackendId` do not exist.

- [ ] **Step 3: Add the minimum backend/settings implementation.**

Create `CodexBackendIds.hpp`:

```cpp
#pragma once

#include <string_view>

inline constexpr std::string_view kChatGptVisualBackendId = "chatgpt";
```

Add `Remote_ChatGPT` to `LLMChoice` and `is_remote_choice()`.

Add settings API:

```cpp
std::string get_codex_executable_path() const;
void set_codex_executable_path(const std::string& path);
std::string get_chatgpt_model() const;
void set_chatgpt_model(const std::string& model);
```

Persist these ordinary settings under `Settings`:

```text
CodexExecutablePath=<path-or-empty>
ChatGptModel=<model-id-or-empty-for-auto>
```

Teach `llm_choice_to_string()` / `parse_llm_choice()` about `Remote_ChatGPT`. Teach visual-id normalization to preserve exactly `chatgpt`; do not synthesize a local `VisualModelDescriptor` for it.

- [ ] **Step 4: Run focused and full unit tests; verify GREEN.**

```bash
cmake --build build-tests --target ai_file_sorter_tests
ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
```

- [ ] **Step 5: Commit.**

```bash
git add app/include/CodexBackendIds.hpp app/include/Types.hpp app/include/Settings.hpp app/lib/Settings.cpp tests/unit/test_chatgpt_settings.cpp app/CMakeLists.txt
git commit -m "feat: persist ChatGPT Codex backend settings"
```

---

## Task 2: Implement pure Codex protocol builders/parsers

**Files:**
- Create: `app/include/CodexProtocol.hpp`
- Create: `app/lib/CodexProtocol.cpp`
- Create: `tests/unit/test_codex_protocol.cpp`
- Modify: `app/CMakeLists.txt`

**Interfaces:**
- **Consumes:** JsonCpp and current documented app-server v2 protocol.
- **Produces:** typed account/model/turn/error data plus pure request builders and response/notification parsers. No `QObject` or process code.

Use current app-server names: `initialize`, `initialized`, `account/read`, `account/login/start`, `account/login/cancel`, `account/logout`, `account/rateLimits/read`, `model/list`, `thread/start`, `turn/start`, `turn/interrupt`. The wire is newline-delimited JSON and omits the `jsonrpc` member.

- [ ] **Step 1: Write failing builder/parser tests.**

Define the wished-for API in tests first:

```cpp
const Json::Value login = CodexProtocol::make_chatgpt_login_params();
CHECK(login["type"].asString() == "chatgpt");

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
CHECK_FALSE(models[0].accepts_image);
CHECK(models[1].accepts_image);
CHECK(models[1].is_default);
```

Also require builders for:

```cpp
Json::Value make_initialize_params(std::string_view version);
Json::Value make_thread_start_params(const CodexInferenceConfig&);
Json::Value make_turn_start_params(std::string_view thread_id,
                                   const std::vector<CodexUserInput>& inputs,
                                   const Json::Value& output_schema);
```

Test that a text input and image-data input serialize distinctly and that no source filesystem path is accepted by the image-data input type.

- [ ] **Step 2: Run and verify RED.**

```bash
cmake --build build-tests --target ai_file_sorter_tests
ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
```

Expected failure: missing `CodexProtocol` types/functions.

- [ ] **Step 3: Implement the smallest typed protocol layer.**

Use types similar to:

```cpp
enum class CodexErrorKind {
    RuntimeNotFound,
    StartupFailed,
    ProtocolError,
    AuthenticationRequired,
    RateLimited,
    ModelUnavailable,
    ImageUnsupported,
    TurnFailed,
    Timeout,
    ProcessCrashed,
    Cancelled
};

class CodexError final : public std::runtime_error {
public:
    CodexError(CodexErrorKind kind, std::string message);
    CodexErrorKind kind() const noexcept;
private:
    CodexErrorKind kind_;
};

struct CodexAccountInfo {
    bool authenticated{false};
    std::string email;
    std::string plan_type;
};

struct CodexModelInfo {
    std::string id;
    std::string display_name;
    bool is_default{false};
    bool accepts_image{false};
};

struct CodexUserInput {
    enum class Kind { Text, ImageDataUrl };
    Kind kind{Kind::Text};
    std::string value;
};
```

`make_thread_start_params()` must set the stable v1 isolation values:

```cpp
params["ephemeral"] = true;
params["approvalPolicy"] = "never";
params["sandbox"] = "read-only";
params["cwd"] = inference_cwd;
params["baseInstructions"] = base_instructions;
params["developerInstructions"] = developer_instructions;
```

Do not turn on experimental API capabilities just to access optional features. Do not serialize `chatgptAuthTokens`.

For turn output, provide parsers for:
- request response ids/errors;
- `account/*` notifications;
- `item/agentMessage/delta` text accumulation;
- `turn/completed` status and usage;
- explicit JSON-RPC error `-32001` -> retryable/rate-style overload error.

- [ ] **Step 4: Run tests and verify GREEN.**

```bash
cmake --build build-tests --target ai_file_sorter_tests
ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
```

- [ ] **Step 5: Commit.**

```bash
git add app/include/CodexProtocol.hpp app/lib/CodexProtocol.cpp tests/unit/test_codex_protocol.cpp app/CMakeLists.txt
git commit -m "feat: add Codex app-server protocol layer"
```

---

## Task 3: Build a deterministic fake app-server and the `CodexAppServer` transport

**Files:**
- Create: `tests/helpers/fake_codex_app_server.cpp`
- Create: `app/include/CodexAppServer.hpp`
- Create: `app/lib/CodexAppServer.cpp`
- Create: `tests/unit/test_codex_app_server.cpp`
- Modify: `app/CMakeLists.txt`

**Interfaces:**
- **Consumes:** executable path + `CODEX_HOME`; JsonCpp request/response objects from `CodexProtocol`.
- **Produces:** one connected initialized stdio app-server session with correlated synchronous RPC helpers and turn-notification collection. Must only be called on owner Qt thread.

- [ ] **Step 1: Add a fake child executable before production process code.**

Register a small test helper target in CMake:

```cmake
aifs_add_windows_test_executable(aifs_fake_codex_app_server
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/helpers/fake_codex_app_server.cpp"
)
target_link_libraries(aifs_fake_codex_app_server PRIVATE Qt6::Core JsonCpp::JsonCpp)
add_dependencies(ai_file_sorter_tests aifs_fake_codex_app_server)
target_compile_definitions(ai_file_sorter_tests PRIVATE
    AIFS_FAKE_CODEX_APP_SERVER_PATH="$<TARGET_FILE:aifs_fake_codex_app_server>"
)
```

The helper must accept the same launch shape used by tests, read one JSON object per stdin line, and reply deterministically. Control scenarios with `AIFS_FAKE_CODEX_SCENARIO`, including:

```text
authenticated
unauthenticated
login-success
text-turn
vision-turn
rate-limited
malformed-json
crash-after-turn-start
slow-turn
```

It must never access the network.

- [ ] **Step 2: Write failing transport tests against the fake process.**

Test at minimum:

```cpp
TEST_CASE("CodexAppServer initializes and correlates out-of-order responses") { /* ... */ }
TEST_CASE("CodexAppServer keeps reading notifications while waiting for a turn") { /* ... */ }
TEST_CASE("CodexAppServer maps malformed JSON to protocol error") { /* ... */ }
TEST_CASE("CodexAppServer reports child crash without replaying the request") { /* ... */ }
TEST_CASE("CodexAppServer times out a non-completing turn") { /* ... */ }
```

The fake server should deliberately emit a notification between request and response so the test proves that nested waiting still services `readyReadStandardOutput`.

- [ ] **Step 3: Run and verify RED.**

```bash
cmake --build build-tests --target ai_file_sorter_tests aifs_fake_codex_app_server
ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
```

- [ ] **Step 4: Implement `CodexAppServer` with owner-thread-only process access.**

Use a simple synchronous-on-runtime-thread API; keep the Qt event loop alive with a local `QEventLoop` while waiting:

```cpp
class CodexAppServer final : public QObject {
    Q_OBJECT
public:
    explicit CodexAppServer(QObject* parent = nullptr);

    void start(const CodexLaunchConfig& config);
    void stop();
    bool is_running() const;

    Json::Value request(std::string_view method,
                        const Json::Value& params,
                        std::chrono::milliseconds timeout);
    void notify(std::string_view method, const Json::Value& params = {});

    CodexTurnResult run_ephemeral_turn(const CodexTurnRequest& request,
                                       std::chrono::milliseconds timeout,
                                       const std::function<bool()>& cancelled);
    void interrupt_turn(std::string_view thread_id, std::string_view turn_id);
};
```

Implementation rules:
- connect `readyReadStandardOutput`, `readyReadStandardError`, and `finished` before `start()`;
- buffer stdout and split only complete newline-delimited JSON records;
- monotonically increment integer request ids;
- maintain response map keyed by id;
- route messages with `method` and no `id` as notifications;
- accumulate `item/agentMessage/delta` only for the active thread/turn;
- complete only on matching `turn/completed`;
- retain stderr for diagnostics but never parse it as protocol JSON;
- no automatic request replay after child exit;
- send `initialize`, wait for its response, then send `initialized` exactly once per process connection.

- [ ] **Step 5: Run focused/full tests and verify GREEN.**

```bash
cmake --build build-tests --target ai_file_sorter_tests aifs_fake_codex_app_server
ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
```

- [ ] **Step 6: Commit.**

```bash
git add tests/helpers/fake_codex_app_server.cpp app/include/CodexAppServer.hpp app/lib/CodexAppServer.cpp tests/unit/test_codex_app_server.cpp app/CMakeLists.txt
git commit -m "feat: add Codex app-server process transport"
```

---

## Task 4: Add the application-scoped `CodexRuntimeService`

**Files:**
- Create: `app/include/CodexRuntimeService.hpp`
- Create: `app/lib/CodexRuntimeService.cpp`
- Create: `tests/unit/test_codex_runtime_service.cpp`
- Modify: `app/CMakeLists.txt`

**Interfaces:**
- **Consumes:** Settings-derived runtime path/model preferences, `CodexAppServer`, fake app-server in tests.
- **Produces:** one shared thread-safe runtime with cached account/models, async UI auth operations, blocking worker-safe inference, restart/cancellation policy.

- [ ] **Step 1: Write failing runtime-discovery and threading tests.**

Cover discovery precedence:

```text
explicit configured executable
-> executable beside AI File Sorter
-> executable found on PATH
-> RuntimeNotFound
```

Inject search roots/PATH in tests; do not depend on a machine-installed Codex.

Add threading assertions:

```cpp
TEST_CASE("Codex runtime serves inference from a std thread while QProcess stays on runtime QThread") { /* ... */ }
TEST_CASE("UI account refresh and worker inference share one process") { /* ... */ }
TEST_CASE("runtime serializes text and image turns") { /* ... */ }
TEST_CASE("runtime restarts only on the next operation after a crash") { /* ... */ }
```

- [ ] **Step 2: Run and verify RED.**

```bash
cmake --build build-tests --target ai_file_sorter_tests aifs_fake_codex_app_server
ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
```

- [ ] **Step 3: Implement runtime state and worker ownership.**

Public facade shape:

```cpp
struct CodexRuntimeSnapshot {
    bool runtime_found{false};
    bool running{false};
    bool authenticated{false};
    std::string runtime_version;
    CodexAccountInfo account;
    std::vector<CodexModelInfo> models;
    std::string last_error;
};

class CodexRuntimeService final : public QObject {
    Q_OBJECT
public:
    explicit CodexRuntimeService(QObject* parent = nullptr);
    ~CodexRuntimeService() override;

    void configure(CodexRuntimeConfig config);
    CodexRuntimeSnapshot snapshot() const;

    void start_or_refresh_async();
    void begin_chatgpt_login_async();
    void begin_device_code_login_async();
    void logout_async();

    CodexTurnResult run_turn(const CodexTurnRequest& request,
                             const std::function<bool()>& cancelled = {});
    bool selected_model_accepts_images(std::string_view configured_model) const;

signals:
    void stateChanged();
    void loginUrlReady(const QUrl& url);
    void deviceCodeReady(const QUrl& verification_url, const QString& user_code);
};
```

Internally create `CodexRuntimeWorker : QObject`, move it to one `QThread`, then create/own `CodexAppServer` on that thread. Use queued lambdas for async UI operations and `Qt::BlockingQueuedConnection` only for worker-thread inference calls. Guard against a blocking call originating on the runtime thread itself.

For model selection:
- empty configured id = Auto;
- resolve Auto to the `isDefault` model from current catalog;
- if a stored id disappears, use Auto and report a non-fatal state message;
- image support is true only when the resolved model advertises image in `inputModalities`.

For auth:
- `account/read` determines initial state;
- browser login uses `{"type":"chatgpt"}` and emits returned `authUrl`;
- device fallback uses `{"type":"chatgptDeviceCode"}` and emits verification URL/code;
- logout uses `account/logout`;
- after login/account notification, refresh account and model list;
- never expose tokens in the facade.

For launch environment:

```cpp
QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
env.insert("CODEX_HOME", QString::fromStdString(config.codex_home));
```

Create a dedicated empty inference cwd below the same AI File Sorter runtime area. Validate runtime first with `codex.exe --version` using a separate short-lived probe process; the persistent worker then launches `codex.exe app-server`.

- [ ] **Step 4: Run tests and verify GREEN.**

```bash
cmake --build build-tests --target ai_file_sorter_tests aifs_fake_codex_app_server
ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
```

- [ ] **Step 5: Commit.**

```bash
git add app/include/CodexRuntimeService.hpp app/lib/CodexRuntimeService.cpp tests/unit/test_codex_runtime_service.cpp app/CMakeLists.txt
git commit -m "feat: add shared Codex runtime service"
```

---

## Task 5: Implement `CodexClient : ILLMClient`

**Files:**
- Create: `app/include/CodexClient.hpp`
- Create: `app/lib/CodexClient.cpp`
- Create: `tests/unit/test_codex_client.cpp`
- Modify: `app/CMakeLists.txt`

**Interfaces:**
- **Consumes:** `std::shared_ptr<CodexRuntimeService>`, selected model id, existing `ILLMClient` arguments.
- **Produces:** existing `Main : Subcategory` categorization result and generic text completion.

- [ ] **Step 1: Write failing client tests using a fake/runtime test fixture.**

Require:

```cpp
TEST_CASE("CodexClient adapts structured category JSON to existing category contract") {
    // fake turn result: {"main_category":"Research article","subcategory":"Microbiology"}
    CHECK(client.categorize_file("paper.pdf", sanitized_context, FileType::File, "")
          == "Research article : Microbiology");
}

TEST_CASE("CodexClient does not send actionable absolute source path") { /* inspect fake server request */ }
TEST_CASE("CodexClient generic completion does not attach category output schema") { /* ... */ }
TEST_CASE("CodexClient maps auth and rate-limit errors without treating them as categories") { /* ... */ }
```

- [ ] **Step 2: Run and verify RED.**

- [ ] **Step 3: Implement the minimum client.**

```cpp
class CodexClient final : public ILLMClient {
public:
    CodexClient(std::shared_ptr<CodexRuntimeService> runtime, std::string model);

    std::string categorize_file(const std::string& file_name,
                                const std::string& file_path,
                                FileType file_type,
                                const std::string& consistency_context) override;
    std::string complete_prompt(const std::string& prompt, int max_tokens) override;
    void set_prompt_logging_enabled(bool enabled) override;
};
```

Category turn requirements:
- use a fresh ephemeral thread;
- put file/category instructions in `baseInstructions`/`developerInstructions`;
- send only controlled textual context as user input;
- constrain final output with schema equivalent to:

```json
{
  "type": "object",
  "additionalProperties": false,
  "required": ["main_category", "subcategory"],
  "properties": {
    "main_category": {"type": "string"},
    "subcategory": {"type": "string"}
  }
}
```

Do not derive or expose the absolute path as an agent-operable location. Treat incoming `file_path` as existing AI File Sorter prompt/context text and sanitize path-looking first-line material into a display context before constructing the Codex user message.

`complete_prompt()` sends only text and no category schema.

- [ ] **Step 4: Run focused and full tests; verify GREEN.**

```bash
cmake --build build-tests --target ai_file_sorter_tests aifs_fake_codex_app_server
ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
```

- [ ] **Step 5: Commit.**

```bash
git add app/include/CodexClient.hpp app/lib/CodexClient.cpp tests/unit/test_codex_client.cpp app/CMakeLists.txt
git commit -m "feat: add ChatGPT Codex LLM client"
```

---

## Task 6: Implement native multimodal `CodexImageAnalyzer`

**Files:**
- Create: `app/include/CodexImageAnalyzer.hpp`
- Create: `app/lib/CodexImageAnalyzer.cpp`
- Create: `tests/unit/test_codex_image_analyzer.cpp`
- Modify: `app/CMakeLists.txt`

**Interfaces:**
- **Consumes:** source image path locally, shared runtime/model capability.
- **Produces:** existing `ImageAnalysisResult { description, suggested_name }`; Codex sees pixels as data URI but not source path.

- [ ] **Step 1: Write failing image encoding/privacy tests.**

Generate a tiny image fixture in the test with `QImage` instead of committing a binary asset.

Require:

```cpp
TEST_CASE("Codex image analyzer sends normalized PNG data URL and no source path") {
    // create temp/private-name.png
    const ImageAnalysisResult result = analyzer.analyze(image_path);
    CHECK(result.description == "a red square");
    CHECK(result.suggested_name == "red-square.png");

    const std::string captured = fake_server.last_turn_json();
    CHECK(captured.find("data:image/png;base64,") != std::string::npos);
    CHECK(captured.find(image_path.string()) == std::string::npos);
}

TEST_CASE("Codex image analyzer refuses model without image modality") { /* expect ImageUnsupported */ }
TEST_CASE("Codex image analyzer rejects unreadable image before making a turn") { /* ... */ }
```

- [ ] **Step 2: Run and verify RED.**

- [ ] **Step 3: Implement normalization and structured visual request.**

Use Qt's image stack, not OCR:

```cpp
QImageReader reader(QString::fromStdString(Utils::path_to_utf8(image_path)));
reader.setAutoTransform(true);
QImage image = reader.read();
if (image.isNull()) {
    throw CodexError(CodexErrorKind::TurnFailed, "Could not decode image.");
}

constexpr int kMaxImageDimension = 2048;
if (std::max(image.width(), image.height()) > kMaxImageDimension) {
    image = image.scaled(kMaxImageDimension,
                         kMaxImageDimension,
                         Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
}

QByteArray png;
QBuffer buffer(&png);
buffer.open(QIODevice::WriteOnly);
if (!image.save(&buffer, "PNG")) { /* throw */ }
const QByteArray data_url = "data:image/png;base64," + png.toBase64();
```

Send two user inputs: task text plus image-data URI. Do not create a `localImage` input.

Use a separate output schema:

```json
{
  "type": "object",
  "additionalProperties": false,
  "required": ["description", "suggested_name"],
  "properties": {
    "description": {"type": "string"},
    "suggested_name": {"type": "string"}
  }
}
```

Leave `ImageAnalysisDiagnostics.available == false` for remote ChatGPT vision in v1; do not fake local GPU timing fields.

- [ ] **Step 4: Run focused/full tests; verify GREEN.**

```bash
cmake --build build-tests --target ai_file_sorter_tests aifs_fake_codex_app_server
ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
```

- [ ] **Step 5: Commit.**

```bash
git add app/include/CodexImageAnalyzer.hpp app/lib/CodexImageAnalyzer.cpp tests/unit/test_codex_image_analyzer.cpp app/CMakeLists.txt
git commit -m "feat: add native ChatGPT image analyzer"
```

---

## Task 7: Integrate ChatGPT text and vision into the analysis workflow

**Files:**
- Modify: `app/include/AnalysisWorkflowContext.hpp`
- Modify: `app/lib/AnalysisCoordinator.cpp`
- Modify: `app/include/MainApp.hpp`
- Modify: `app/lib/MainApp.cpp`
- Extend: analysis/main-app unit tests (`tests/unit/test_main_app_visual_fallback.cpp` and/or a new `tests/unit/test_chatgpt_analysis_routing.cpp`)
- Modify: `app/CMakeLists.txt` if a new test file is added.

**Interfaces:**
- **Consumes:** Settings selection + shared runtime service.
- **Produces:** correct mixed-backend routing without changing `ILLMClient` or `ImageAnalyzer` public interfaces.

- [ ] **Step 1: Write failing routing tests first.**

Prove these four combinations:

```text
Remote_ChatGPT text + chatgpt vision -> CodexClient + CodexImageAnalyzer, same runtime
Remote_ChatGPT text + local vision   -> CodexClient + existing local analyzer
local text + chatgpt vision          -> LocalLLMClient + CodexImageAnalyzer
local text + local vision            -> unchanged behavior
```

Also test that selecting ChatGPT vision does not execute `VisualLlmRuntime::resolve_active_backend("chatgpt", ...)` and therefore does not require GGUF/mmproj artifacts.

- [ ] **Step 2: Run and verify RED.**

- [ ] **Step 3: Add the smallest workflow seam.**

Forward-declare `ImageAnalyzer` in `AnalysisWorkflowContext.hpp` and add:

```cpp
std::function<std::unique_ptr<ImageAnalyzer>()> make_remote_image_analyzer;
```

In `AnalysisCoordinator`, branch before local backend resolution:

```cpp
const bool use_chatgpt_vision =
    app_.settings.get_visual_model_id() == kChatGptVisualBackendId;

if (use_chatgpt_vision) {
    if (!app_.make_remote_image_analyzer) {
        throw std::runtime_error("ChatGPT visual analyzer factory is unavailable.");
    }
    analyzer = app_.make_remote_image_analyzer();
} else {
    // Existing VisualLlmRuntime resolution, diagnostics, GPU preflight,
    // and CPU fallback path stays intact.
}
```

Do not offer the local GPU->CPU retry branch for `CodexImageAnalyzer`; remote vision errors should flow into the existing explicit “continue without visual analysis?” decision instead of silently becoming filename-only analysis.

- [ ] **Step 4: Own one runtime in `MainApp` and wire factories.**

Add a long-lived member:

```cpp
std::shared_ptr<CodexRuntimeService> codex_runtime_;
```

Create it once with the app and shut it down before Qt object teardown.

Add `Remote_ChatGPT` to `make_llm_client()`:

```cpp
if (choice == LLMChoice::Remote_ChatGPT) {
    auto client = std::make_unique<CodexClient>(codex_runtime_, settings.get_chatgpt_model());
    client->set_prompt_logging_enabled(should_log_prompts());
    return client;
}
```

Pass a remote visual factory in `make_analysis_workflow_context()`:

```cpp
[this]() -> std::unique_ptr<ImageAnalyzer> {
    if (settings.get_visual_model_id() != kChatGptVisualBackendId) {
        return {};
    }
    return std::make_unique<CodexImageAnalyzer>(codex_runtime_, settings.get_chatgpt_model());
},
```

- [ ] **Step 5: Fix pre-analysis readiness checks.**

Current `on_analyze_clicked()` uses `using_local_llm` as a proxy for all remote needs. Replace that assumption with two explicit booleans:

```cpp
const bool chatgpt_text = settings.get_llm_choice() == LLMChoice::Remote_ChatGPT;
const bool chatgpt_vision = settings.get_analyze_images_by_content() &&
                            settings.get_visual_model_id() == kChatGptVisualBackendId;
const bool needs_chatgpt = chatgpt_text || chatgpt_vision;
const bool needs_remote_network = is_remote_choice(settings.get_llm_choice()) || chatgpt_vision;
```

Rules:
- network check if `needs_remote_network`;
- existing `CategorizationService::ensure_remote_credentials()` only for OpenAI API/Gemini/custom API choices;
- if `needs_chatgpt`, require runtime installed + authenticated + chosen model valid before starting analysis;
- if ChatGPT vision is selected, additionally require image capability;
- local text + ChatGPT vision must therefore authenticate even though `using_local_llm == true`.

Add `Remote_ChatGPT` backend status text to `current_backend_status_text()`.

- [ ] **Step 6: Run full tests and verify GREEN.**

```bash
cmake --build build-tests --target ai_file_sorter_tests aifs_fake_codex_app_server
ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
```

- [ ] **Step 7: Commit.**

```bash
git add app/include/AnalysisWorkflowContext.hpp app/lib/AnalysisCoordinator.cpp app/include/MainApp.hpp app/lib/MainApp.cpp tests/unit app/CMakeLists.txt
git commit -m "feat: route analysis through shared ChatGPT backends"
```

---

## Task 8: Add ChatGPT visual item to the visual-backend model

**Files:**
- Modify: `app/include/LLMSelectionVisualBackendModel.hpp`
- Modify: `app/lib/LLMSelectionVisualBackendModel.cpp`
- Modify: `tests/unit/test_llm_selection_visual_backend_model.cpp`

**Interfaces:**
- **Consumes:** local visual descriptors/custom LLMs plus ChatGPT availability/capability state supplied by the dialog.
- **Produces:** a selectable synthetic `chatgpt` item without treating it as a local model descriptor.

- [ ] **Step 1: Write failing model tests.**

Change the builder API to accept explicit ChatGPT availability:

```cpp
struct ChatGptVisualOption {
    bool visible{true};
    bool enabled{false};
    QString unavailable_reason;
};
```

Require that:
- ChatGPT item can be included alongside every existing local/custom visual item;
- `choose_visual_backend_id("chatgpt", items)` preserves it when present;
- local `selected_visual_model_descriptor("chatgpt")` is never called as the authoritative path; return `nullptr` for the synthetic remote id rather than the default local descriptor;
- canonical id remains exactly `chatgpt`.

- [ ] **Step 2: Run and verify RED.**

- [ ] **Step 3: Implement the synthetic item.**

Keep the item simple:

```cpp
items.push_back({QObject::tr("ChatGPT vision (subscription)"),
                 std::string(kChatGptVisualBackendId)});
```

Store enabled/unavailable state in the item model if the combo population code needs it; do not overload `VisualModelDescriptor` with remote semantics.

- [ ] **Step 4: Run full tests and verify GREEN.**

- [ ] **Step 5: Commit.**

```bash
git add app/include/LLMSelectionVisualBackendModel.hpp app/lib/LLMSelectionVisualBackendModel.cpp tests/unit/test_llm_selection_visual_backend_model.cpp
git commit -m "feat: expose ChatGPT as visual backend option"
```

---

## Task 9: Build the shared ChatGPT account/model UI

**Files:**
- Modify: `app/include/LLMSelectionDialog.hpp`
- Modify: `app/lib/LLMSelectionDialog.cpp`
- Modify: `app/include/LLMSelectionDialogTestAccess.hpp`
- Modify: `app/lib/MainApp.cpp` (`show_llm_selection_dialog()` call and saved values)
- Create: `tests/unit/test_llm_selection_dialog_chatgpt.cpp`
- Modify: `app/CMakeLists.txt`

**Interfaces:**
- **Consumes:** `Settings` + shared `CodexRuntimeService` state/signals.
- **Produces:** new ChatGPT subscription text selection and shared runtime/account/model controls usable even when the text backend is local.

- [ ] **Step 1: Write failing dialog tests.**

Require UI behavior:
- existing `ChatGPT (OpenAI API key)` radio remains unchanged;
- new radio label is unambiguous, e.g. `ChatGPT account (Codex subscription)`;
- Codex executable/account/model controls exist outside the API-key-only widget;
- selecting local text + ChatGPT visual still exposes/enables ChatGPT account controls;
- authenticated state shows plan/account status and model list;
- unauthenticated state shows Sign in and no fake API-key field;
- image-incapable selected model disables ChatGPT visual item with a reason;
- accepting dialog persists path/model/visual choice but no tokens.

- [ ] **Step 2: Run and verify RED.**

- [ ] **Step 3: Extend dialog construction to receive the shared runtime.**

Preferred constructor:

```cpp
LLMSelectionDialog(Settings& settings,
                   std::shared_ptr<CodexRuntimeService> codex_runtime,
                   QWidget* parent = nullptr);
```

Update `MainApp::show_llm_selection_dialog()` to pass the existing application runtime; do not create a second runtime inside the dialog.

- [ ] **Step 4: Add the controls and signal wiring.**

Add:
- Codex executable path `QLineEdit` + Browse;
- runtime/version label;
- account status label;
- Sign in with ChatGPT button;
- Sign out button;
- `QComboBox` with `Auto (Codex default)` plus `model/list` results;
- device-code fallback action/link.

Browser flow:

```cpp
connect(codex_runtime_.get(), &CodexRuntimeService::loginUrlReady,
        this, [](const QUrl& url) { QDesktopServices::openUrl(url); });
```

Keep ChatGPT controls visible whenever either:

```cpp
selected_choice == LLMChoice::Remote_ChatGPT ||
selected_visual_model_id_ == kChatGptVisualBackendId
```

It is acceptable to keep the shared account group always visible if that results in a clearer UI; it must not be hidden solely because the text LLM is local.

- [ ] **Step 5: Handle capability changes without silent downgrade.**

On model/account state refresh:
- rebuild ChatGPT model combo;
- resolve Auto to current default;
- if selected model lacks `image`, keep the stored visual choice visible but disabled/flagged and block dialog acceptance with an actionable message until the user selects an image-capable model or a local visual backend;
- if logged out, preserve settings but mark ChatGPT text/vision unavailable until login.

- [ ] **Step 6: Run full tests and verify GREEN.**

```bash
cmake --build build-tests --target ai_file_sorter_tests aifs_fake_codex_app_server
ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
```

- [ ] **Step 7: Commit.**

```bash
git add app/include/LLMSelectionDialog.hpp app/lib/LLMSelectionDialog.cpp app/include/LLMSelectionDialogTestAccess.hpp app/lib/MainApp.cpp tests/unit/test_llm_selection_dialog_chatgpt.cpp app/CMakeLists.txt
git commit -m "feat: add ChatGPT Codex account controls"
```

---

## Task 10: Complete error, cancellation, rate-limit, and shutdown behavior

**Files:**
- Modify: `app/include/CodexRuntimeService.hpp`
- Modify: `app/lib/CodexRuntimeService.cpp`
- Modify: `app/lib/CodexAppServer.cpp`
- Modify: `app/lib/MainApp.cpp`
- Extend: `tests/unit/test_codex_app_server.cpp`
- Extend: `tests/unit/test_codex_runtime_service.cpp`
- Extend: analysis cancellation tests as appropriate.

**Interfaces:**
- **Consumes:** existing `stop_analysis`, app-server notifications/errors/process exit.
- **Produces:** deterministic recoverable errors, `turn/interrupt`, one-next-operation restart, clean shutdown.

- [ ] **Step 1: Write failing behavior tests.**

Test each condition independently:

```text
runtime not found -> RuntimeNotFound + browse action text
not authenticated -> AuthenticationRequired
model removed -> Auto fallback + nonfatal notice
rate limit/usage exhausted -> RateLimited, never parsed as category
server overload -32001 -> retryable error; no immediate busy loop
active analysis cancellation -> turn/interrupt sent
process crash during turn -> current request fails, not replayed
next operation after crash -> one clean restart attempt
second restart failure -> stable disconnected state
application shutdown -> worker drains/stops and QProcess destroyed on runtime thread
```

- [ ] **Step 2: Run and verify RED.**

- [ ] **Step 3: Implement minimal cancellation and error mapping.**

`run_turn()` should periodically consult the caller cancellation predicate while waiting for turn completion. On first cancellation:

```cpp
server_->interrupt_turn(active_thread_id, active_turn_id);
throw CodexError(CodexErrorKind::Cancelled, "Codex inference cancelled.");
```

Do not replay a turn after a crash or timeout.

Use `account/rateLimits/read` to refresh concise state when available; it is support/status information, not a prerequisite for each turn.

- [ ] **Step 4: Implement deterministic shutdown.**

In `CodexRuntimeService::~CodexRuntimeService()`:
- queue worker stop onto the runtime thread;
- terminate gracefully, then kill only after a bounded timeout;
- destroy `CodexAppServer`/`QProcess` on the runtime thread;
- quit and wait for `QThread`;
- never leave the analysis thread waiting on a destroyed worker.

- [ ] **Step 5: Run full tests and verify GREEN.**

```bash
cmake --build build-tests --target ai_file_sorter_tests aifs_fake_codex_app_server
ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
```

- [ ] **Step 6: Commit.**

```bash
git add app/include/CodexRuntimeService.hpp app/lib/CodexRuntimeService.cpp app/lib/CodexAppServer.cpp app/lib/MainApp.cpp tests/unit
git commit -m "fix: harden Codex cancellation and recovery"
```

---

## Task 11: Documentation and Windows manual integration validation

**Files:**
- Modify: `README.md`
- Modify: `docs/configuration-and-environment.md`
- Modify: `TESTS.md`
- Modify: `TROUBLESHOOTING.md`

**Interfaces:**
- **Consumes:** completed feature behavior.
- **Produces:** reproducible user setup and maintainer validation instructions.

- [ ] **Step 1: Write documentation assertions/checklist before prose.**

Make a checklist in the working notes and ensure docs explicitly cover all of these:

```text
standalone codex.exe required; Microsoft Store app not required
runtime discovery order
isolated CODEX_HOME location
Sign in with ChatGPT flow and device-code fallback
no OAuth tokens in AI File Sorter config.ini
Auto/model selection and image capability
ChatGPT text + local vision and local text + ChatGPT vision
inline pixel upload, not source path
images may consume two subscription-backed turns in v1
documents remain text-extracted
rate-limit/auth/runtime troubleshooting
no automatic codex.exe download/update
```

- [ ] **Step 2: Update README/configuration/troubleshooting docs.**

Include a concise example configuration section but never show token files or suggest copying Codex auth state from another profile.

- [ ] **Step 3: Document automated test commands in `TESTS.md`.**

Linux/macOS generic form:

```bash
cmake -S app -B build-tests -DAI_FILE_SORTER_BUILD_TESTS=ON
cmake --build build-tests --target ai_file_sorter_tests aifs_fake_codex_app_server
ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
```

Windows must retain the repository's existing Qt/vcpkg configure prerequisites.

- [ ] **Step 4: Run final automated verification.**

```bash
cmake --build build-tests --target ai_file_sorter_tests aifs_fake_codex_app_server
ctest --test-dir build-tests --output-on-failure
```

Do not enable live-LLM tests; these feature tests must remain account/network independent.

- [ ] **Step 5: Perform the Windows manual acceptance pass.**

On a Windows machine with standalone `codex.exe` and no Microsoft Store Codex app required:

1. Select ChatGPT account text backend; browse to standalone `codex.exe`.
2. Complete browser OAuth and verify account/plan status.
3. Restart AI File Sorter and verify isolated Codex auth persists.
4. Verify dynamic model list appears and Auto resolves.
5. Classify ordinary files with ChatGPT text.
6. Select ChatGPT vision; analyze a representative PNG/JPEG and verify description + suggested name feed categorization.
7. Verify a non-image-capable model blocks ChatGPT vision instead of silently degrading.
8. Verify ChatGPT text + local vision.
9. Verify local text + ChatGPT vision.
10. Verify original image path is absent from development prompt/protocol logs; only the inline data URI is sent.
11. Cancel an active Codex analysis and verify it stops cleanly.
12. Sign out and verify both ChatGPT text and vision transition to unauthenticated state.
13. If possible, exercise device-code login fallback.
14. Confirm existing OpenAI API-key, Gemini, custom API, local text, and local visual paths still operate.

- [ ] **Step 6: Commit documentation.**

```bash
git add README.md docs/configuration-and-environment.md TESTS.md TROUBLESHOOTING.md
git commit -m "docs: document ChatGPT Codex backend"
```

---

## Task 12: Final verification, review, and integration readiness

**Files:**
- No new production files expected; only fixes backed by new failing regression tests if verification finds a problem.

**Interfaces:**
- **Consumes:** all prior tasks.
- **Produces:** reviewable branch with evidence that existing and new behavior is green.

- [ ] **Step 1: Run formatting/static checks used by the repository.**

Use the repository's established formatting/build commands for changed C++ files. Do not mass-format unrelated code.

- [ ] **Step 2: Run the complete configured test suite.**

```bash
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

If any failure is discovered, first add or isolate a failing regression test, then fix it under TDD; do not patch production code without a red test.

- [ ] **Step 3: Inspect the diff for security/privacy regressions.**

Specifically search changed code for:

```text
access_token
refresh_token
chatgptAuthTokens
localImage
source absolute path in Codex JSON
second CodexRuntimeService creation
QProcess access outside runtime worker
```

Expected results:
- no AI File Sorter token persistence;
- no internal token-login mode;
- no source-image `localImage` use;
- exactly one application-owned runtime;
- all persistent app-server `QProcess` access isolated to the runtime thread.

- [ ] **Step 4: Request code review using the Superpowers review workflow.**

Review against both this plan and the approved spec, with special attention to:
- app-server protocol compatibility;
- Qt thread affinity/deadlocks;
- path/privacy isolation;
- mixed text/vision backend combinations;
- account/rate-limit UX;
- unchanged legacy/local backends.

- [ ] **Step 5: Resolve review findings with TDD and re-run the full suite.**

- [ ] **Step 6: Final commit only if review-driven fixes were needed.**

```bash
git add <review-fix-files>
git commit -m "fix: address ChatGPT backend review findings"
```
