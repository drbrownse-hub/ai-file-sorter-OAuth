# Task 3 second fix-round report

## Scope

This follow-up hardens the Task 3 `CodexAppServer` transport after the
re-review of commits `6b7b252` and `2688ee7`. The approved architecture is
unchanged: one owner-thread `CodexAppServer` owns one `QProcess`, uses
synchronous-on-owner-thread RPCs with nested Qt event loops, and does not
implement Task 4 (`CodexRuntimeService`).

## Re-review findings addressed

- Notifications for the known thread are retained in a dedicated pending-turn
  queue while `turn/start` is outstanding. The generic idle queue remains
  bounded at 256 records. Once the turn response supplies the turn id, the
  pending queue is drained through the normal active-turn notification parser;
  it is then cleared.
- Cancellation or timeout during `thread/start` or `turn/start` retires the
  current app-server process. The transport cannot safely know whether a
  request reached the remote server, and there is no active turn id available
  for an interrupt during setup, so retiring the process avoids reusing an
  uncertain remote operation or replaying a setup request. `stop()` clears
  pending responses/notifications and the next public request fails with
  `ProcessCrashed`; completion-time cancellation retains the existing
  `turn/interrupt` behavior.
- Public `request()` and `notify()` reserve both `initialize` and `initialized`
  for the bootstrap handshake for the lifetime of a ready connection. Public
  attempts are rejected with `ProtocolError`; only the private bootstrap paths
  can send the appropriate handshake message during initialization.

## Strict TDD evidence

The second-round regression tests and fake-server scenarios were added before
the production changes. Against the current production at that point
(commit `2688ee7`), the focused build completed but the focused test run was
RED:

```text
/workspace/scratch/88353b2e9f1a/.tooling/cmake-site/cmake/data/bin/cmake --build build-tests --target ai_file_sorter_tests aifs_fake_codex_app_server --parallel 4
./build-tests/ai_file_sorter_tests "CodexAppServer*" --reporter compact
```

Result: build exit code 0; focused test exit code 42. The run completed 11
test cases, with 7 passed and 4 failed:

- setup cancellation left the process running and allowed the next request to
  use it, in both the thread-start and turn-start scenarios;
- public `request("initialize")` and `notify("initialized")` were accepted;
- the pending-turn flood retained only the bounded tail, so the expected 300
  deltas were reduced to 256 and the text was incomplete.

After the minimal implementation, the same focused command completed GREEN:

```text
All tests passed (35 assertions in 11 test cases)
```

The original five Task 3 transport tests remain green alongside the six
regression tests.

## Implementation and tests

Changed paths in this follow-up:

- `app/include/CodexAppServer.hpp` — pending-turn notification state and the
  setup-failure retirement helper.
- `app/lib/CodexAppServer.cpp` — thread-scoped notification retention, setup
  process retirement and queue cleanup, and permanent public handshake-name
  reservation.
- `tests/helpers/fake_codex_app_server.cpp` — a no-network scenario emitting
  300 matching deltas and completion before the `turn/start` response.
- `tests/unit/test_codex_app_server.cpp` — pending-turn flood integrity,
  setup-retirement/no-replay, and handshake-reservation regressions.
- `.superpowers/sdd/2026-08-26-chatgpt-oauth-codex-multimodal-implementation/task-3-report.md`
  — this evidence and rationale.

Owner-thread checks, newline-delimited stdout parsing, response correlation,
stderr diagnostics, nested event-loop waits, process-failure handling, and
completion-time interruption remain unchanged. No Task 4 files or APIs were
added.

## Verification

Exact reconfigure and requested complete target build:

```text
/workspace/scratch/88353b2e9f1a/.tooling/cmake-site/cmake/data/bin/cmake -S app -B build-tests -DAI_FILE_SORTER_BUILD_TESTS=ON
/workspace/scratch/88353b2e9f1a/.tooling/cmake-site/cmake/data/bin/cmake --build build-tests --target ai_file_sorter_tests aifs_fake_codex_app_server --parallel 4
```

Result: exit code 0. Both requested targets built successfully. Configuration
reported only existing environment warnings about missing XKB, Qt Linguist
tools, and ccache.

Fresh focused verification:

```text
./build-tests/ai_file_sorter_tests "CodexAppServer*" --reporter compact
```

Result: exit code 0; 11 test cases and 35 assertions passed.

Required registered CTest:

```text
/workspace/scratch/88353b2e9f1a/.tooling/cmake-site/cmake/data/bin/ctest --test-dir build-tests -R ai_file_sorter_tests --output-on-failure
```

The first complete run after the final reconfigure/build returned exit code 8
after 12.08 seconds. It completed all 470 cases: 455 passed and 15 failed
(5476 assertions, 5458 passed and 18 failed). The failures were the known
unrelated Linux baseline/environment findings, including Windows-specific
Vulkan/CUDA runtime-path expectations and translation-resource expectations.
No Task 3 CodexAppServer failure was reported.

A final rerun after the post-termination cleanup returned exit code 8 after
3.29 seconds because the same baseline suite raised SIGSEGV in the unrelated
`MainApp retranslate reflects language changes` test. At the crash point, 145
cases had run: 138 passed and 7 unrelated cases failed. The CodexAppServer
cases that ran before the crash were green. Thus the registered baseline is
not clean in this Linux environment; the focused Task 3 result above is the
relevant completion gate.

## Follow-up commit

The second fix round is committed separately from `6b7b252` and `2688ee7` with
message:

```text
fix: close Codex transport lifecycle gaps
```
