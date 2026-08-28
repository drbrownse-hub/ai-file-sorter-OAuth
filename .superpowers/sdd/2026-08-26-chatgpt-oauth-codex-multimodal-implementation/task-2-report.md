# Task 2 report: pure Codex protocol builders and parsers

## Status

Complete with an environment baseline concern. The focused Codex protocol suite is green. The full Linux test target still has the documented 15 unrelated translation/Windows-runtime failures, and subsequent random/declaration-order full runs also exposed a pre-existing Qt retranslation segfault before the target could finish.

## Changes

- Added pure JsonCpp `CodexProtocol` builders for initialize, managed ChatGPT login, isolated ephemeral threads, and text/data-URL image turns.
- Added typed account, model, inference, user-input, response, account-notification, usage, turn-completion, and error data.
- Encoded the Task 2 isolation values: `ephemeral`, `approvalPolicy: never`, `sandbox: read-only`, `cwd`, base instructions, and developer instructions.
- Image input accepts only inline `data:image/...;base64,...` URLs and serializes as app-server `imageUrl`; no local source path field is created or accepted.
- Kept initialization on the stable API surface and added no `chatgptAuthTokens`, OAuth-token fields, Qt classes, filesystem, or process code.
- Parsed managed ChatGPT account state, model image capabilities, `account/updated`, `account/login/completed`, `account/rateLimits/updated`, agent-message deltas, completed-turn status/usage, and RPC responses.
- Mapped JSON-RPC error `-32001` to `CodexErrorKind::RateLimited` with a recoverable retry message.
- Registered `tests/unit/test_codex_protocol.cpp` in `app/CMakeLists.txt`.

## TDD evidence

1. Wrote `tests/unit/test_codex_protocol.cpp` and registered it before creating `CodexProtocol.hpp` or `CodexProtocol.cpp`.
2. The prescribed command was attempted first:

   ```text
   cmake --build build-tests --target ai_file_sorter_tests
   ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
   ```

   It could not run because `cmake` is not on `PATH` in this environment.
3. The provisioned CMake binary ran the same target and produced the expected RED failure:

   ```text
   fatal error: CodexProtocol.hpp: No such file or directory
   ```

4. After the minimal builder/parser implementation, focused execution passed:

   ```text
   ./build-tests/ai_file_sorter_tests "CodexProtocol*" --reporter console
   All tests passed (62 assertions in 6 test cases)
   ```

5. Official app-server protocol review established that image input uses `imageUrl`, not `dataUrl`. The assertion was changed first and failed as expected:

   ```text
   "" == "data:image/png;base64,iVBORw0KGgo="
   ```

   Updating only the serializer field made the focused suite green again.
6. Self-review identified that `account/*` required more than `account/updated`. Tests for `account/login/completed` and `account/rateLimits/updated` were added first; compilation failed because the notification kind/login/rate-limit members did not yet exist. The smallest typed notification extension made the final focused run pass:

   ```text
   ./build-tests/ai_file_sorter_tests "CodexProtocol*" --reporter console
   All tests passed (71 assertions in 6 test cases)
   ```

## Broader test evidence

The first post-implementation CTest run completed:

```text
test cases:  459 |  444 passed | 15 failed
assertions: 5432 | 5414 passed | 18 failed
```

Those 15 failures are the pre-existing Linux baseline: translation-resource expectations and Windows-only CUDA/runtime-path checks. All Codex protocol tests ran in that target.

After the final account-notification test addition, focused tests remained green. Re-running the required CTest command hit an environment-sensitive `MainApp retranslate reflects language changes` `SIGSEGV` at different randomized Catch2 orders (after 129 cases, then after 7 cases). A declaration-order run reached the same pre-existing Qt test and segfaulted after 160 cases. The protocol layer has no Qt dependencies, and no unrelated tests were changed. This prevents a final completed full-suite count in this environment; the earlier completed 15-failure run remains the accurate comparable baseline evidence.

## Self-review

- `CodexProtocol` is pure C++/JsonCpp: no `QObject`, `QProcess`, OAuth-token handling, filesystem access, or process access.
- Login construction contains only managed `type: chatgpt`; no `chatgptAuthTokens`, access token, or refresh token fields are serialized.
- Data URL input and typed error/notification paths are separate from future category and vision output adapters.
- The diff is limited to the four Task 2 implementation/test files and this required report.
- `git diff --check` and a final focused build/test run were performed before commit.
