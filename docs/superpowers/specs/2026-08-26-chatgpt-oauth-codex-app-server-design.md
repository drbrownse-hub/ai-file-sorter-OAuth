# ChatGPT OAuth via Codex App Server — Design

Date: 2026-08-26
Status: Proposed
Scope: Personal-first Windows implementation

## 1. Goal

Add a new AI File Sorter backend that allows the application to use the user's ChatGPT subscription for inference without requiring an OpenAI API key and without depending on the Microsoft Store Codex desktop application.

The implementation will use the standalone `codex.exe` runtime and its documented `app-server` JSON-RPC interface. AI File Sorter will not implement OpenAI OAuth directly and will not store ChatGPT access or refresh tokens.

## 2. Non-goals

The first implementation will not:

- distribute or auto-install `codex.exe`;
- use or require the Microsoft Store Codex app;
- implement OpenAI OAuth directly;
- copy Hermes' direct token/backend integration;
- share the user's normal Codex configuration directory;
- expose advanced Codex agent features, MCP, shell execution, or filesystem tooling as product features;
- redesign the existing categorization pipeline;
- target enterprise deployment or multi-user packaging yet.

## 3. Chosen architecture

Add a new backend:

`Remote_ChatGPT -> CodexClient : ILLMClient -> CodexAppServer -> codex.exe app-server -> ChatGPT subscription`

`CodexClient` will implement the existing `ILLMClient` interface so upstream categorization code remains unaware of OAuth or Codex protocol details.

`CodexAppServer` will own one long-lived `QProcess` running `codex.exe app-server`. Communication will use app-server's default stdio JSONL transport.

The application will use a dedicated `CODEX_HOME`, separate from `%USERPROFILE%\.codex`, so AI File Sorter does not inherit the user's ordinary Codex configuration, MCP servers, plugins, skills, or agent settings.

Suggested Windows location:

`%LOCALAPPDATA%\AI File Sorter\codex\`

The app-server client will identify itself with a distinct `clientInfo`, for example:

- name: `ai_file_sorter`
- title: `AI File Sorter`
- version: application version

## 4. Runtime discovery

When the ChatGPT backend is selected, resolve `codex.exe` in this order:

1. Explicit path stored in AI File Sorter settings.
2. `codex.exe` next to the AI File Sorter executable.
3. `codex.exe` available on `%PATH%`.
4. Otherwise show a recoverable "Codex runtime not found" state with a Browse action.

Validate the selected runtime with `codex.exe --version` before starting app-server.

The first version will not download or update Codex automatically.

## 5. App-server lifecycle

AI File Sorter will keep a single app-server process alive while the ChatGPT backend is in use.

Startup sequence:

1. Resolve `codex.exe`.
2. Create the dedicated `CODEX_HOME` directory if needed.
3. Launch `codex.exe app-server` with `QProcess` and the dedicated environment.
4. Send app-server `initialize` with AI File Sorter `clientInfo`.
5. Send the `initialized` notification.
6. Call `account/read`.
7. Enter either authenticated-ready or unauthenticated state.

The process is not restarted for each file classification.

If the process exits unexpectedly, the integration should fail the current request, transition to a disconnected state, and attempt one clean restart on the next operation. It must not loop indefinitely.

## 6. Authentication lifecycle

### Browser login

The primary login flow uses `account/login/start` with ChatGPT authentication.

Flow:

1. User clicks "Sign in with ChatGPT".
2. AI File Sorter sends `account/login/start` with the ChatGPT login type.
3. App-server returns `loginId` and `authUrl`.
4. AI File Sorter opens `authUrl` with `QDesktopServices::openUrl()`.
5. `codex.exe` owns the localhost OAuth callback.
6. AI File Sorter observes app-server login/account notifications and re-reads account state.

AI File Sorter never receives, parses, stores, refreshes, or deletes OpenAI OAuth tokens directly.

### Device-code fallback

Expose device-code login as a secondary recovery path for localhost callback problems.

### Persistence

Authentication persistence belongs to Codex under the dedicated `CODEX_HOME`. AI File Sorter persists only ordinary settings such as:

- Codex executable path;
- preferred Codex model.

No ChatGPT access or refresh token is added to AI File Sorter's `config.ini`.

### Logout

Use app-server `account/logout`; do not manually delete Codex auth files.

### Expired/revoked authentication

Treat authentication loss as a recoverable backend state. Surface a concise "Sign in again" action rather than treating it as a fatal application error.

## 7. Inference isolation

The Codex backend is used as a constrained inference transport, not as an autonomous coding agent.

For every classification:

- create a fresh ephemeral thread;
- override normal Codex instructions with minimal file-classifier instructions;
- set approval policy to `never`;
- use a dedicated empty working directory;
- use a read-only sandbox;
- disable agent/tool network access;
- do not pass actionable absolute user file paths;
- do not enable MCP, plugins, skills, or custom environments;
- end the thread after one classification turn.

App-server does not currently expose a simple stable `tools=[]` switch that removes every built-in Codex tool. Therefore isolation must be layered rather than relying on one setting.

The dedicated `CODEX_HOME`, empty working directory, read-only sandbox, disabled agent network access, no approvals, minimal instructions, and non-actionable path data together form the isolation boundary.

## 8. Prompt/data flow

AI File Sorter's existing file-reading and enrichment pipeline remains responsible for gathering context. Codex should receive text supplied by AI File Sorter rather than being asked to inspect files itself.

Conceptual request fields:

- item type;
- file name;
- non-actionable directory context;
- already-extracted document summary or image description when available;
- existing consistency hints and category constraints.

Real absolute paths should be transformed into display/context labels where possible instead of being exposed as locations the agent might try to inspect.

The existing categorization service remains unchanged at its public boundary.

## 9. Structured classification output

For `categorize_file()`, use app-server `outputSchema` to constrain the final assistant message to JSON with exactly:

- `main_category`: string
- `subcategory`: string

`CodexClient` will parse that JSON and convert it back to the existing internal text contract:

`<Main category> : <Subcategory>`

This avoids a broad rewrite of the existing response parsing/categorization pipeline while improving output reliability for the Codex backend.

For generic `complete_prompt()`, do not force the category schema. Return the final assistant text from an ephemeral constrained turn.

## 10. Model selection

Do not hard-code a specific Codex model.

Use app-server `model/list` to enumerate models currently available to the authenticated ChatGPT account. The UI should offer:

- `Auto (Codex default)`
- current visible models reported by app-server

Persist the selected model identifier. If a previously selected model is no longer available, fall back to Auto and surface a non-fatal notice.

Reasoning effort should not be exposed in the first version; use the model/server default.

## 11. UI design

Add a new LLM choice, conceptually `Remote_ChatGPT`, alongside the existing remote backends.

The ChatGPT section should contain:

- Codex executable path field;
- Browse action;
- runtime/version status;
- account status;
- "Sign in with ChatGPT" action when unauthenticated;
- "Sign out" action when authenticated;
- model dropdown populated from `model/list`;
- optional device-code fallback link/action.

Example authenticated state:

- `Codex executable: C:\...\codex.exe`
- `Connected — ChatGPT Plus`
- model selector
- Sign out

The UI should not contain an OpenAI API-key field for this backend.

## 12. Component boundaries

### `CodexAppServer`

Responsibility: protocol/process transport only.

Owns:

- `QProcess` lifecycle;
- environment setup including dedicated `CODEX_HOME`;
- JSONL framing and parsing;
- request IDs;
- pending request correlation;
- app-server initialization;
- account operations;
- model listing;
- ephemeral thread/turn operations;
- notification dispatch;
- process restart detection.

It must not know AI File Sorter category semantics.

### `CodexClient : ILLMClient`

Responsibility: adapt AI File Sorter's LLM contract onto `CodexAppServer`.

Owns:

- classifier base/developer instructions;
- conversion of existing inputs into Codex-safe prompt data;
- structured output schema;
- conversion of JSON classification output back to the existing `Main : Subcategory` return contract;
- generic prompt completion adaptation;
- prompt logging integration.

It must not manage OAuth tokens or raw process framing.

### Settings/UI

Responsibility: user-controlled backend configuration and state display.

Owns:

- new `Remote_ChatGPT` selection;
- Codex executable path;
- preferred model;
- login/logout triggers;
- visible runtime/account status.

### Existing categorization pipeline

No new OAuth/Codex responsibilities. Continue using `ILLMClient`.

## 13. Concurrency

The app-server process is shared, but requests must be serialized at the transport layer where necessary and correlated by JSON-RPC request ID.

Initial implementation should favor correctness over throughput:

- allow one active Codex classification turn at a time;
- queue concurrent requests inside `CodexAppServer` or its caller-facing wrapper;
- do not spawn multiple app-server processes for parallel classification;
- preserve existing application cancellation semantics where practical;
- on cancellation, interrupt the active turn when possible and fail the caller cleanly.

Parallel Codex classifications can be considered later after correctness and subscription/rate-limit behavior are understood.

## 14. Failure handling

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
- rate-limited/account usage exhausted;
- turn failed;
- turn timed out;
- app-server crashed.

User-facing errors should describe the action to take: browse for Codex, sign in again, switch model, retry later, or switch LLM backend.

A crashed app-server may be restarted once on the next operation. Do not automatically replay a completed-or-unknown classification request after a crash, because that could duplicate subscription usage and produce ambiguous state.

## 15. Rate limits

Use `account/rateLimits/read` as backend support when available.

For the first UI version, show only concise account/backend status. Detailed usage visualization is not required.

When a classification fails because of rate limits, propagate a dedicated recoverable error rather than treating the response as a categorization failure.

## 16. Testing strategy

No automated test should require a real ChatGPT account or make an OpenAI network request.

### Unit tests

Test protocol-independent logic such as:

- runtime discovery precedence;
- path/context sanitization;
- classifier prompt construction;
- output schema construction;
- JSON classification parsing;
- `Main : Subcategory` adaptation;
- model fallback behavior;
- error mapping.

### App-server protocol tests

Use a fake child process/test fixture that speaks JSONL and simulates app-server responses and notifications.

Cover at minimum:

- initialize/initialized handshake;
- already authenticated startup;
- unauthenticated startup;
- browser login success;
- login failure/cancellation;
- logout;
- model listing;
- structured classification success;
- malformed classification output;
- rate-limit response;
- authentication loss mid-session;
- process crash;
- one-time restart behavior;
- timeout;
- request-ID correlation;
- queued concurrent calls;
- cancellation/turn interruption where supported.

### Integration/manual Windows validation

On a Windows machine with standalone `codex.exe`:

1. Select ChatGPT backend without Microsoft Store Codex installed.
2. Point AI File Sorter at standalone `codex.exe`.
3. Confirm dedicated `CODEX_HOME` is created.
4. Complete browser OAuth.
5. Restart AI File Sorter and confirm persisted Codex authentication works.
6. Confirm model list populates.
7. Classify representative files using existing extracted document/image context.
8. Confirm Codex does not modify files or inspect arbitrary user paths.
9. Sign out and confirm the backend returns to unauthenticated state.
10. Exercise device-code login if practical.

## 17. Likely files touched

Exact paths may shift during implementation, but the change is expected to include:

- `app/include/Types.hpp` — add `Remote_ChatGPT`;
- new `app/include/CodexAppServer.hpp`;
- new `app/lib/CodexAppServer.cpp`;
- new `app/include/CodexClient.hpp`;
- new `app/lib/CodexClient.cpp`;
- `app/lib/Settings.cpp` and matching settings declarations;
- `app/lib/LLMSelectionDialog.cpp` and matching header;
- client/factory wiring that currently selects OpenAI/Gemini/local clients;
- build files for new sources;
- tests/fixtures for protocol and client behavior;
- configuration/user documentation describing standalone Codex installation and OAuth behavior.

Avoid unrelated refactors.

## 18. Security and privacy notes

- AI File Sorter does not store OpenAI OAuth tokens.
- Authentication is isolated in a dedicated Codex home.
- Codex receives only prompt data AI File Sorter explicitly sends.
- Absolute user paths should be minimized/sanitized before inference.
- Agent filesystem access is constrained with an empty working directory and read-only sandbox.
- Agent/tool network access is disabled for classification turns.
- No approvals are allowed.
- No automatic command execution is a product feature.

The first version is optimized for a trusted single-user Windows environment, not for hostile multi-user desktop environments.

## 19. Deferred work

Possible later additions:

- automatic standalone Codex download/update management;
- packaging `codex.exe` with releases if licensing/distribution allows;
- richer rate-limit UI;
- reasoning-effort controls;
- carefully evaluated parallel classification;
- stronger Windows OS-level process sandboxing;
- broader cross-platform packaging;
- public-distribution hardening and installer UX.

## 20. Acceptance criteria

The feature is successful when, on Windows without the Microsoft Store Codex app:

1. The user can point AI File Sorter at a standalone `codex.exe`.
2. The user can sign into ChatGPT through the official Codex-managed OAuth flow.
3. AI File Sorter stores no OAuth token itself.
4. Authentication survives an AI File Sorter restart through the isolated Codex home.
5. Available ChatGPT/Codex models can be selected dynamically.
6. Existing file categorization can run through ChatGPT subscription-backed inference.
7. Existing non-Codex backends continue to work unchanged.
8. Codex classification runs are ephemeral, non-writing, no-approval, and do not receive actionable absolute user paths.
9. Auth loss, rate limits, missing runtime, and process crashes produce recoverable user-facing states.
10. Automated tests require neither a real ChatGPT account nor live OpenAI network access.
