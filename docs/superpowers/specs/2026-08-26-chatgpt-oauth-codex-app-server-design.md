# ChatGPT OAuth via Codex App Server — Design

Date: 2026-08-26
Status: Proposed after self-review
Scope: Personal-first Windows implementation

## 1. Goal

Add a ChatGPT-backed AI File Sorter mode that:

- uses the user's ChatGPT subscription for inference without an OpenAI API key;
- works with the standalone Windows `codex.exe` runtime and does not require the Microsoft Store Codex desktop app;
- delegates ChatGPT OAuth, token storage, and token refresh to the documented Codex `app-server` interface;
- preserves AI File Sorter's existing multimodal behavior and adds native ChatGPT/Codex image understanding as a visual-analysis option;
- keeps existing local, OpenAI API-key, Gemini, custom API, and local visual-model paths working.

AI File Sorter will not implement OpenAI OAuth directly and will not store ChatGPT access or refresh tokens.

## 2. Non-goals

The first implementation will not:

- distribute or auto-install `codex.exe`;
- use or require the Microsoft Store Codex app;
- implement OpenAI OAuth directly;
- copy Hermes' direct token/backend integration;
- share the user's normal Codex configuration directory;
- expose Codex shell execution, MCP, plugins, skills, coding-agent workflows, or filesystem tooling as product features;
- redesign the existing categorization pipeline beyond the injection points needed for a shared Codex runtime and visual backend;
- add native raw PDF/Office/audio/video inputs to Codex in v1;
- require ChatGPT vision for image handling: existing local visual backends remain available;
- target enterprise deployment or multi-user packaging yet.

The multimodality requirement for v1 is specifically to preserve the application's current image-analysis behavior and allow images to be analyzed natively by a ChatGPT/Codex model when the selected model advertises image input support. Existing document text extraction/summarization remains the document path.

## 3. Chosen architecture

Add a new text/categorization backend:

`Remote_ChatGPT -> CodexClient : ILLMClient -> shared CodexRuntimeService -> CodexAppServer -> codex.exe app-server -> ChatGPT subscription`

Add a new optional visual backend:

`ChatGPT visual -> CodexImageAnalyzer : ImageAnalyzer -> same shared CodexRuntimeService -> codex.exe app-server`

The important ownership rule is that `CodexClient` and `CodexImageAnalyzer` are lightweight adapters. They do not each spawn their own Codex process. Both use one application-scoped `CodexRuntimeService` for the configured ChatGPT account.

This resolves an important constraint in the existing codebase: the analysis workflow can create fresh `ILLMClient` instances and invoke them from worker threads, while Qt `QProcess` has thread affinity. The single `QProcess` therefore belongs to the runtime service, not to an arbitrary per-request client object.

### 3.1 Shared runtime thread

`CodexRuntimeService` owns a dedicated Qt worker object/thread with an event loop. `CodexAppServer` and its `QProcess` live on that thread for their entire lifetime.

Callers from the UI thread or categorization worker threads communicate with the service through a thread-safe queued API. The public adapter methods may expose synchronous/future-style results as appropriate to existing call sites, but raw `QProcess` methods are never called from those caller threads.

The shared service serializes v1 inference turns while still continuously receiving asynchronous app-server notifications such as login completion, account updates, turn events, and process termination.

### 3.2 Isolated Codex home

Use a dedicated `CODEX_HOME`, separate from `%USERPROFILE%\.codex`, so AI File Sorter does not inherit the user's normal Codex configuration, MCP servers, plugins, skills, or agent settings.

Suggested Windows location:

`%LOCALAPPDATA%\AI File Sorter\codex\`

The app-server client identifies itself with distinct `clientInfo`, for example:

- name: `ai_file_sorter`
- title: `AI File Sorter`
- version: application version

## 4. Runtime discovery

When ChatGPT functionality is first required, resolve `codex.exe` in this order:

1. Explicit path stored in AI File Sorter settings.
2. `codex.exe` next to the AI File Sorter executable.
3. `codex.exe` available on `%PATH%`.
4. Otherwise expose a recoverable "Codex runtime not found" state with a Browse action.

Validate the selected runtime with `codex.exe --version` before starting app-server.

The first version will not download or update Codex automatically.

## 5. App-server lifecycle

AI File Sorter keeps one app-server process alive while the shared ChatGPT runtime is active.

Startup sequence:

1. Resolve `codex.exe`.
2. Create the dedicated `CODEX_HOME` and empty inference working directory if needed.
3. Start the runtime worker thread.
4. Launch `codex.exe app-server` from that thread with `QProcess` and the dedicated environment.
5. Send app-server `initialize` with AI File Sorter `clientInfo`.
6. Send the `initialized` notification.
7. Call `account/read`.
8. Enter authenticated-ready or unauthenticated state.
9. When authenticated, call `model/list` and cache the current visible model catalog and capabilities.

The process is not restarted for each classification or image analysis.

If the process exits unexpectedly, fail the current operation, transition the service to a disconnected state, and allow one clean restart on the next operation. Do not loop indefinitely and do not automatically replay an operation whose completion is uncertain.

## 6. Authentication lifecycle

### 6.1 Browser login

The primary login flow uses `account/login/start` with ChatGPT authentication.

1. User clicks "Sign in with ChatGPT".
2. UI asks the shared `CodexRuntimeService` to begin login.
3. The service sends `account/login/start` with the ChatGPT login type.
4. App-server returns `loginId` and `authUrl`.
5. The UI opens `authUrl` with `QDesktopServices::openUrl()`.
6. `codex.exe` owns the localhost OAuth callback.
7. The shared service observes app-server login/account notifications and refreshes `account/read`.

AI File Sorter never receives, parses, stores, refreshes, or deletes OpenAI OAuth tokens directly.

### 6.2 Device-code fallback

Expose device-code login as a secondary recovery path for localhost callback problems.

### 6.3 Persistence

Authentication persistence belongs to Codex under the dedicated `CODEX_HOME`. AI File Sorter persists only ordinary settings such as:

- Codex executable path;
- preferred ChatGPT/Codex text model;
- chosen visual backend, including ChatGPT visual when enabled.

No ChatGPT access or refresh token is added to AI File Sorter's `config.ini`.

### 6.4 Logout

Use app-server `account/logout`; do not manually delete Codex auth files.

### 6.5 Expired or revoked authentication

Treat authentication loss as a recoverable shared-backend state. Both text classification and ChatGPT visual analysis should surface the same concise "Sign in again" action rather than independently entering contradictory auth states.

## 7. Inference isolation

The Codex backend is used as a constrained inference transport, not as an autonomous coding agent.

For classification and visual-analysis turns:

- create a fresh ephemeral thread for each logical inference request;
- override normal Codex instructions with task-specific minimal instructions;
- set approval policy to `never`;
- use a dedicated empty working directory;
- use a read-only sandbox;
- disable agent/tool network access;
- do not expose actionable absolute user paths to the model;
- do not enable MCP, plugins, skills, or custom environments;
- end the thread after the inference turn.

App-server does not currently expose a simple stable `tools=[]` switch that removes every built-in Codex tool. Isolation is therefore layered rather than relying on one setting.

The dedicated `CODEX_HOME`, empty working directory, read-only sandbox, disabled agent network access, no approvals, minimal instructions, and controlled input data together form the v1 isolation boundary.

## 8. Text categorization data flow

AI File Sorter's existing file-reading and enrichment pipeline remains responsible for gathering text context. Codex should receive supplied text rather than being asked to locate or inspect the source file itself.

Conceptual classification input:

- item type;
- file name;
- non-actionable directory context;
- existing extracted document summary when available;
- image description from the selected visual backend when available;
- existing consistency hints and category constraints.

Real absolute paths should be reduced to non-actionable display/context labels where possible instead of being exposed as filesystem locations.

The existing categorization service remains unchanged at its public `ILLMClient` boundary.

## 9. Multimodality and image analysis

Multimodality is a first-class compatibility requirement, not deferred work.

AI File Sorter already has a separate `ImageAnalyzer` stage that produces an image description and optional suggested name before categorization. The ChatGPT integration should participate in that existing abstraction rather than bypassing it or reducing images to filename-only classification.

### 9.1 `CodexImageAnalyzer : ImageAnalyzer`

Add a ChatGPT-backed `ImageAnalyzer` implementation that shares `CodexRuntimeService` with `CodexClient`.

For an image selected for ChatGPT visual analysis:

1. AI File Sorter reads the image itself using the existing image decode utilities.
2. Normalize/resize according to the visual-analysis limits used by the application; encode a normalized image as PNG bytes.
3. Base64-encode the PNG and build an inline `data:image/png;base64,...` URL.
4. Send a multimodal `turn/start` containing task text plus app-server `UserInput` of type `image` with that inline data URL.
5. Do not use app-server `localImage` for ordinary AI File Sorter image analysis, because that would disclose the real source path to Codex tooling.
6. Use structured output for at least `description` and `suggested_name`.
7. Return the result through the existing `ImageAnalysisResult` contract.
8. The existing pipeline may then include that description in the subsequent category-classification prompt exactly as it does for local visual backends.

This intentionally retains the current two-stage image workflow:

`image pixels -> visual description/suggested name -> normal categorization`

When both ChatGPT visual analysis and ChatGPT categorization are selected, an image may therefore consume two subscription-backed turns. This is acceptable for v1 and must be documented because it affects rate-limit usage. A future optimization may combine vision and category selection into one turn, but not at the cost of complicating the first implementation or changing existing semantics.

### 9.2 Preserve local visual models

ChatGPT vision is an additional visual backend, not a replacement. Users must remain able to choose an existing local/custom visual model while using ChatGPT for text categorization, and vice versa where existing UI behavior permits it.

The visual-backend selection should therefore include a ChatGPT option only when the shared runtime is authenticated and the effective model can accept image input; existing local visual choices remain present.

### 9.3 Model capability gating

Use `model/list` metadata, including each model's advertised `inputModalities`, to determine whether a selected model supports image input.

If the selected ChatGPT model does not advertise image input:

- do not silently downgrade ChatGPT visual analysis to filename-only classification;
- disable or mark the ChatGPT visual choice unavailable;
- let the user choose an image-capable ChatGPT model or an existing local visual backend.

If `Auto (Codex default)` is selected, resolve the current default model from `model/list` when evaluating visual capability. If the current runtime cannot provide reliable modality metadata, treat ChatGPT visual capability as unavailable rather than guessing.

### 9.4 Documents and other modalities

Existing document files continue through AI File Sorter's current text extraction/summarization path and that text is supplied to the model. v1 does not reinterpret arbitrary PDFs or Office files as Codex-native file inputs.

Codex app-server may expose additional modalities such as audio in its protocol, but adding raw audio/video ingestion is outside this spec. This avoids expanding scope while preserving the application's existing multimodal image behavior.

## 10. Structured outputs

### 10.1 Category classification

For `categorize_file()`, use app-server `outputSchema` to constrain the final assistant message to JSON with exactly:

- `main_category`: string
- `subcategory`: string

`CodexClient` parses that JSON and converts it back to the existing internal text contract:

`<Main category> : <Subcategory>`

This avoids a broad rewrite of the existing response parser/categorization pipeline while improving output reliability for the Codex backend.

### 10.2 Visual analysis

For `CodexImageAnalyzer`, use a separate schema with at least:

- `description`: string
- `suggested_name`: string

The adapter maps this into the existing `ImageAnalysisResult` fields.

### 10.3 Generic completion

For `complete_prompt()`, do not force the category schema. Return the final assistant text from an ephemeral constrained turn.

## 11. Model selection

Do not hard-code a specific Codex model.

Use app-server `model/list` to enumerate models currently available to the authenticated ChatGPT account. The UI should offer:

- `Auto (Codex default)`;
- current visible models reported by app-server.

Persist the selected model identifier. If a previously selected model is no longer available, fall back to Auto and surface a non-fatal notice.

For v1, the selected ChatGPT/Codex model is also the model used by `CodexImageAnalyzer` when ChatGPT visual analysis is selected. A separate remote vision-model picker is deferred unless implementation evidence shows it is necessary.

Reasoning effort should not be exposed in the first version; use the model/server default.

## 12. UI design

Add a new LLM choice, conceptually `Remote_ChatGPT`, alongside existing remote backends.

The ChatGPT section should contain:

- Codex executable path field;
- Browse action;
- runtime/version status;
- account status;
- "Sign in with ChatGPT" action when unauthenticated;
- "Sign out" action when authenticated;
- model dropdown populated from `model/list`;
- optional device-code fallback action.

Example authenticated state:

- `Codex executable: C:\...\codex.exe`
- `Connected — ChatGPT Plus`
- model selector
- Sign out

The UI should not contain an OpenAI API-key field for this backend.

The existing visual-backend UI should gain a ChatGPT visual option rather than being replaced. Its availability reflects authentication plus image-input capability of the selected model. If unavailable, the UI should explain why rather than silently changing visual behavior.

## 13. Component boundaries

### 13.1 `CodexAppServer`

Responsibility: raw app-server protocol/process transport, used only from its owning runtime thread.

Owns:

- `QProcess` lifecycle;
- JSONL framing and parsing;
- request IDs;
- pending request correlation;
- initialization handshake;
- account/model/thread/turn RPCs;
- notification decoding;
- process termination detection.

It does not know AI File Sorter category or image-analysis semantics.

### 13.2 `CodexRuntimeService`

Responsibility: application-scoped, thread-safe ownership and orchestration of the Codex connection.

Owns:

- dedicated Qt worker thread/event loop;
- one `CodexAppServer`/`QProcess` instance;
- environment setup including dedicated `CODEX_HOME` and empty inference working directory;
- runtime discovery/version validation;
- shared authentication/account state;
- model catalog and capability cache;
- serialized v1 inference queue;
- cancellation/turn interruption routing;
- restart state and one-time restart policy;
- thread-safe request surface for UI and analysis workers.

It does not construct file-category prompts or interpret `ImageAnalysisResult`.

### 13.3 `CodexClient : ILLMClient`

Responsibility: adapt AI File Sorter's text LLM contract onto `CodexRuntimeService`.

Owns:

- classifier base/developer instructions;
- conversion of existing inputs into Codex-safe text prompt data;
- classification output schema;
- conversion of JSON classification output to the existing `Main : Subcategory` contract;
- generic prompt completion adaptation;
- prompt logging integration.

It holds/reference-shares the application runtime service; it does not own a `QProcess` or OAuth tokens.

### 13.4 `CodexImageAnalyzer : ImageAnalyzer`

Responsibility: adapt AI File Sorter's existing visual-analysis contract onto native Codex image input.

Owns:

- image normalization/encoding orchestration using existing image utilities;
- inline image-data payload construction;
- visual-analysis task instructions and schema;
- conversion into `ImageAnalysisResult`;
- validation that the effective model supports image input.

It shares the same runtime service as `CodexClient` and does not own OAuth/process state.

### 13.5 Analysis workflow integration

The existing workflow currently has explicit creation points for LLM clients and visual analyzers. Introduce the smallest injection point needed so analysis can request a ChatGPT-backed `ImageAnalyzer` without making `AnalysisCoordinator` own OAuth or app-server state.

A preferred shape is an application-supplied image-analyzer factory/callback in the workflow context:

- ChatGPT visual selected -> construct `CodexImageAnalyzer` with shared runtime;
- local/custom visual selected -> keep the existing `ImageAnalyzerFactory` path.

Do not refactor unrelated analysis logic.

### 13.6 Settings/UI

Responsibility: user-controlled configuration and presentation of shared runtime/account/model state.

The UI talks to `CodexRuntimeService`; it must not create a second app-server solely for login or model listing.

## 14. Concurrency and cancellation

Initial implementation favors correctness over throughput:

- one shared app-server process;
- one active Codex inference turn at a time across both text classification and ChatGPT visual analysis;
- concurrent requests queue in `CodexRuntimeService`;
- JSON-RPC requests remain correlated by request ID;
- the runtime thread continues reading notifications while callers wait;
- no `QProcess` access from arbitrary `std::thread` categorization workers;
- on cancellation, interrupt the active turn when the protocol supports it and fail the caller cleanly;
- do not spawn multiple app-server processes for parallel classification.

Serializing both text and image turns intentionally makes shared account/rate-limit behavior deterministic in v1. Controlled parallelism can be evaluated later.

## 15. Failure handling

Define explicit recoverable error classes/states for:

- runtime not found;
- runtime version check failed;
- app-server startup failed;
- protocol initialization failed;
- malformed JSONL/protocol response;
- authentication required;
- authentication expired/revoked;
- login flow failed/cancelled;
- selected model unavailable;
- selected model does not support image input;
- image normalization/encoding failure;
- malformed visual-analysis output;
- rate-limited/account usage exhausted;
- turn failed;
- turn timed out;
- app-server crashed.

User-facing errors should describe an action: browse for Codex, sign in again, select another model/visual backend, retry later, or switch LLM backend.

A crashed app-server may be restarted once on the next operation. Do not automatically replay a completed-or-unknown inference request after a crash, because that could duplicate subscription usage and produce ambiguous state.

If ChatGPT visual analysis fails, do not silently classify the image as if no visual analysis were requested. Surface the visual-analysis failure or follow the application's existing explicit fallback policy if one exists and is visible to the user.

## 16. Rate limits

Use `account/rateLimits/read` as backend support when available.

For the first UI version, show only concise account/backend status. Detailed usage visualization is not required.

When an operation fails because of rate limits, propagate a dedicated recoverable error rather than treating the response as a classification or visual-analysis failure.

Documentation should note that an image analyzed and then categorized through ChatGPT can consume two turns in v1.

## 17. Testing strategy

No automated test should require a real ChatGPT account or make an OpenAI network request.

### 17.1 Unit tests

Test protocol-independent logic including:

- runtime discovery precedence;
- path/context sanitization;
- classifier prompt construction;
- classification output schema and parsing;
- `Main : Subcategory` adaptation;
- model fallback behavior;
- image-capability gating from `model/list` metadata;
- image normalization and PNG/base64 data-URL construction;
- guarantee that the source absolute path is not embedded in the multimodal request;
- visual output schema and `ImageAnalysisResult` mapping;
- error mapping.

### 17.2 Runtime/protocol tests

Use a fake child process/test fixture that speaks JSONL and simulates app-server responses and notifications.

Cover at minimum:

- initialize/initialized handshake;
- already authenticated startup;
- unauthenticated startup;
- browser login success;
- login failure/cancellation;
- logout;
- model listing and `inputModalities` parsing;
- structured text classification success;
- native image-input request success;
- image-incompatible model handling;
- malformed classification output;
- malformed visual-analysis output;
- rate-limit response;
- authentication loss mid-session;
- process crash;
- one-time restart behavior;
- timeout;
- request-ID correlation;
- queued calls shared between `CodexClient` and `CodexImageAnalyzer`;
- cancellation/turn interruption where supported.

### 17.3 Threading tests

Exercise the architecture that the existing application actually uses:

- runtime service/QProcess remains on its owning Qt thread;
- a categorization call originating from a worker `std::thread` can safely use `CodexClient`;
- UI account/model operations and worker inference share one runtime without deadlock;
- app-server notifications are processed while a worker is awaiting a result;
- shutdown does not destroy `QProcess` from the wrong thread.

### 17.4 Integration/manual Windows validation

On Windows with standalone `codex.exe` and without the Microsoft Store Codex app:

1. Select ChatGPT backend and point AI File Sorter at standalone `codex.exe`.
2. Confirm dedicated `CODEX_HOME` is created.
3. Complete browser OAuth.
4. Restart AI File Sorter and confirm persisted authentication works.
5. Confirm model list and input-modality capabilities populate.
6. Classify ordinary text/document-context files through ChatGPT.
7. Select ChatGPT visual analysis and analyze an image natively from pixels.
8. Confirm the visual description/suggested name feeds the existing categorization workflow.
9. Confirm the original source image path is not serialized into the Codex multimodal input.
10. Select an existing local visual model while retaining ChatGPT text categorization and confirm that path still works.
11. Select a non-image-capable ChatGPT model and confirm ChatGPT visual analysis is visibly unavailable rather than silently degraded.
12. Confirm Codex does not modify user files or inspect arbitrary user paths.
13. Sign out and confirm both ChatGPT text and visual backends return to unauthenticated state.
14. Exercise device-code login if practical.

## 18. Likely files touched

Exact paths may shift during implementation, but the change is expected to include:

- `app/include/Types.hpp` — add `Remote_ChatGPT`;
- new `app/include/CodexAppServer.hpp`;
- new `app/lib/CodexAppServer.cpp`;
- new `app/include/CodexRuntimeService.hpp`;
- new `app/lib/CodexRuntimeService.cpp`;
- new `app/include/CodexClient.hpp`;
- new `app/lib/CodexClient.cpp`;
- new `app/include/CodexImageAnalyzer.hpp`;
- new `app/lib/CodexImageAnalyzer.cpp`;
- `app/include/AnalysisWorkflowContext.hpp` — image-analyzer injection/shared runtime wiring as needed;
- `app/lib/AnalysisCoordinator.cpp` — only the minimal visual-backend creation integration;
- visual-backend model/UI files so ChatGPT can be selected without removing existing local visual models;
- `app/lib/Settings.cpp` and matching settings declarations;
- `app/lib/LLMSelectionDialog.cpp` and matching header;
- client/factory wiring that currently selects OpenAI/Gemini/local clients;
- build files for new sources;
- tests/fixtures for protocol, threading, text client, and image analyzer behavior;
- configuration/user documentation describing standalone Codex installation, OAuth, multimodality, and expected subscription usage.

Avoid unrelated refactors.

## 19. Security and privacy notes

- AI File Sorter does not store OpenAI OAuth tokens.
- Authentication is isolated in a dedicated Codex home.
- One shared runtime prevents inconsistent duplicate auth sessions.
- Codex receives only prompt/image data AI File Sorter explicitly sends.
- Absolute user paths are minimized/sanitized before inference.
- Native image analysis uses an inline normalized image data URL instead of giving Codex a `localImage` source path.
- Agent filesystem access is constrained with an empty working directory and read-only sandbox.
- Agent/tool network access is disabled for inference turns.
- No approvals are allowed.
- No automatic command execution is a product feature.

The first version is optimized for a trusted single-user Windows environment, not for hostile multi-user desktop environments.

## 20. Deferred work

Possible later additions:

- automatic standalone Codex download/update management;
- packaging `codex.exe` with releases if licensing/distribution allows;
- richer rate-limit UI;
- reasoning-effort controls;
- separate ChatGPT vision-model selection if evidence justifies it;
- one-turn fusion of image understanding and category selection;
- raw audio/video or additional native file modalities;
- carefully evaluated parallel inference;
- stronger Windows OS-level process sandboxing;
- broader cross-platform packaging;
- public-distribution hardening and installer UX.

## 21. Acceptance criteria

The feature is successful when, on Windows without the Microsoft Store Codex app:

1. The user can point AI File Sorter at standalone `codex.exe`.
2. The user can sign into ChatGPT through the official Codex-managed OAuth flow.
3. AI File Sorter stores no OAuth token itself.
4. Authentication survives an AI File Sorter restart through the isolated Codex home.
5. One application-scoped Codex runtime safely serves UI operations and worker-thread inference.
6. Available ChatGPT/Codex models and their input modalities are obtained dynamically.
7. Existing file categorization can run through ChatGPT subscription-backed inference.
8. Image multimodality is preserved: the user can either keep existing local visual analysis or select native ChatGPT image analysis.
9. ChatGPT image analysis sends normalized inline image data, not the actionable source filesystem path.
10. Existing document text extraction/summarization continues to feed categorization.
11. Existing non-Codex and local visual backends continue to work unchanged.
12. Codex inference runs are ephemeral, non-writing, no-approval, and do not receive actionable absolute user paths.
13. Auth loss, rate limits, missing runtime, incompatible visual model, and process crashes produce recoverable user-facing states.
14. Automated tests require neither a real ChatGPT account nor live OpenAI network access.
