# Troubleshooting

## Logs

AI File Sorter writes three rotating log files:

- `core.log`
- `db.log`
- `ui.log`

### Log locations

- Windows: `%APPDATA%\AIFileSorter\logs`
- Linux: `$XDG_CACHE_HOME/AIFileSorter/logs`
- Linux fallback: `~/.cache/AIFileSorter/logs`

### Log rotation

Each log file rotates at `5 MiB` and keeps `3` rolled files.

That means each log stream keeps:

- the active file
- up to `3` older rotated files
- about `20 MiB` total per stream including the active file

If you are troubleshooting backend selection, model loading, or packaging issues, start with `core.log`.

## ChatGPT account and Codex runtime

### Runtime not found

The ChatGPT account backend requires the standalone Codex runtime. The
Microsoft Store app is not required. In **Settings → Select LLM**, use
**Browse…** to select the standalone `codex.exe` (or `codex` on macOS/Linux)
if discovery does not find it. Discovery checks the saved executable path,
then the executable directory, then configured `PATH` entries, with
`codex.exe` checked before `codex` at each location. AI File Sorter does not
download or update the runtime for you.

### Sign-in or account status fails

Use **Sign in with ChatGPT** and complete the browser flow. If the browser flow
cannot be completed, use **Use device-code sign-in**. Reopen **Select LLM** to
refresh account and model status. Do not copy authentication files from another
Codex profile: AI File Sorter gives the runtime an isolated home under the app
config directory and the runtime owns its authentication state there.

### Model list or ChatGPT vision is unavailable

The model list requires an authenticated, running runtime. **Auto** depends on
the default model reported by Codex. ChatGPT vision additionally requires an
image-capable selected model. If Auto has no suitable default, select an
explicit image-capable model; a text-only model is intentionally blocked rather
than silently downgraded. You may also select a local visual backend while
using ChatGPT for text, or use ChatGPT vision with a local text backend.

### Image privacy and request shape

ChatGPT vision receives a normalized inline PNG data URI. The original source
filesystem path is not part of the image-analysis request. v1 may use two
subscription-backed turns per image: one visual-description turn followed by
one text-categorization turn. Documents remain on the extracted-text path.

### Rate limits, overload, cancellation, or shutdown

ChatGPT account usage is subject to the subscription/runtime's availability and
limits. If the runtime reports a rate limit or overload, wait and retry; the
app does not busy-loop or replay a canceled turn. Canceling an active analysis
should stop the current turn cleanly. If the runtime remains unavailable after
a cancellation or app restart, close and reopen AI File Sorter, then inspect
`core.log` for the startup, turn, and shutdown error sequence.

### Isolated runtime paths

The default Codex home is `%APPDATA%\AIFileSorter\codex` on Windows,
`~/Library/Application Support/AIFileSorter/codex` on macOS, and
`~/.config/AIFileSorter/codex` on Linux, with an `inference` working directory
inside it. `AI_FILE_SORTER_CONFIG_DIR` changes the base location. The app's
`config.ini` stores the executable/model selections, not OAuth access or
refresh tokens.
