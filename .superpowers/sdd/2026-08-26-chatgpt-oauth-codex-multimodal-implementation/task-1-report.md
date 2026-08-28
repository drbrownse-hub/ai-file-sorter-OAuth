# Task 1 report: stable backend identifiers and settings persistence

## Changes

- Added `LLMChoice::Remote_ChatGPT` and included it in `is_remote_choice()`.
- Added `app/include/CodexBackendIds.hpp` with `kChatGptVisualBackendId = "chatgpt"`.
- Added `Settings` getters/setters for the Codex executable path and ChatGPT model.
- Persisted `CodexExecutablePath` and `ChatGptModel` in the existing `Settings` section. Empty values are preserved; an empty ChatGPT model means automatic selection.
- Added `Remote_ChatGPT` serialization and parsing.
- Updated visual-model normalization to preserve `chatgpt` exactly without creating a local visual-model descriptor.
- Added focused round-trip and remote-choice tests, including config inspection for `AccessToken`, `RefreshToken`, and `OAuthToken` field names.
- Added the new test source to `app/CMakeLists.txt`.

No OAuth/access/refresh token settings or Codex process/runtime code were added.

## TDD evidence

1. Wrote `tests/unit/test_chatgpt_settings.cpp` before the production implementation.
2. The mandated initial command was attempted:

   ```text
   cmake -S app -B build-tests -DAI_FILE_SORTER_BUILD_TESTS=ON
   cmake --build build-tests --target ai_file_sorter_tests
   ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
   ```

   The shell reported `cmake: command not found`, so the expected feature-level RED compile failure could not be observed with that command in this environment.
3. Using the already configured generated build tree and its available CMake binaries, `make -C build-tests ai_file_sorter_tests -j2` compiled the new test and implementation successfully.
4. Focused execution:

   ```text
   ./build-tests/ai_file_sorter_tests "ChatGPT*" --reporter console
   All tests passed (11 assertions in 2 test cases)
   ```

## Broader test evidence

The configured target was run with:

```text
/workspace/scratch/88353b2e9f1a/.tooling/cmake-site/cmake/data/bin/ctest \
  --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
```

Result:

```text
test cases:  453 |  438 passed | 15 failed
assertions: 5370 | 5352 passed | 18 failed
```

The 15 failures are the documented Linux baseline: translation-resource expectations and Windows-only CUDA/runtime-path expectations. The new ChatGPT tests passed in this broader run. No unrelated failures were changed.

## Self-review

- `git diff --check` passed.
- The diff is limited to the six task files plus the required report.
- Existing remote choices and legacy settings serialization remain intact.
- Config persistence contains no new OAuth/access/refresh token fields.

## Fix round 1: TDD RED evidence

Created disposable worktree `/workspace/scratch/88353b2e9f1a/task1-red-evidence` at
base `6895870`:

```text
git worktree add --detach /workspace/scratch/88353b2e9f1a/task1-red-evidence 6895870
```

Applied only the committed `tests/unit/test_chatgpt_settings.cpp` and its
`app/CMakeLists.txt` registration. Because the base worktree lacked its nested
`llama.cpp` checkout, a temporary symlink to the already-present dependency was
used only inside the disposable worktree so CMake could configure. The final
worktree was not changed.

Configure command:

```text
/workspace/scratch/88353b2e9f1a/.tooling/cmake-site/cmake/data/bin/cmake \
  -S app -B build-red -DAI_FILE_SORTER_BUILD_TESTS=ON
```

It completed with `-- Configuring done`, `-- Generating done`, and generated
`.../task1-red-evidence/build-red`. The test target build was started with:

```text
/workspace/scratch/88353b2e9f1a/.tooling/cmake-site/cmake/data/bin/cmake \
  --build build-red --target ai_file_sorter_tests -j2
```

To reach the new test compile rule without building unrelated final link objects,
the generated compile command was rerun with a temporary one-file
`CodexBackendIds.hpp` overlay. The base `Types.hpp` and `Settings.hpp` remained
first in the include path. It returned exit code `1` with these expected errors:

```text
error: ‘Remote_ChatGPT’ is not a member of ‘LLMChoice’
error: ‘class Settings’ has no member named ‘set_codex_executable_path’
error: ‘class Settings’ has no member named ‘set_chatgpt_model’
error: ‘class Settings’ has no member named ‘get_codex_executable_path’
error: ‘class Settings’ has no member named ‘get_chatgpt_model’
```

The disposable worktree was removed safely with:

```text
git worktree remove --force /workspace/scratch/88353b2e9f1a/task1-red-evidence
```

The final Task 1 implementation remained unchanged. The temporary header overlay
was also removed.

Post-cleanup verification on the final worktree:

```text
make -C build-tests ai_file_sorter_tests -j2
./build-tests/ai_file_sorter_tests "ChatGPT*" --reporter console
All tests passed (11 assertions in 2 test cases)
git diff --check
passed
```

The 15 broader Linux/environment failures remain baseline and unrelated: nine
translation-resource expectation failures and six Windows-only CUDA/runtime-path
ordering failures. They remain unchanged; the broader result is still 438 passed
and 15 failed out of 453 test cases.
