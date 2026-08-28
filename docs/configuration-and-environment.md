# Configuration and Environment

This page collects the configuration surfaces that are most useful for
contributors, packagers, and integrators. The main README can stay focused on
normal setup and usage.

## Local settings and storage

- `config.ini` lives under the app config directory.
- `AI_FILE_SORTER_CONFIG_DIR` overrides the base config directory.
- `AI_FILE_SORTER_LLM_STORAGE_DIR` overrides where downloaded local model files
  are stored.
- `AI_FILE_SORTER_LLM_DIR` is the legacy alias for
  `AI_FILE_SORTER_LLM_STORAGE_DIR`.
- `CATEGORIZATION_CACHE_FILE` overrides the categorization SQLite filename
  inside the config directory.

## Runtime backend selection

- The Windows and Linux launchers support `--cuda={on|off}` and
  `--vulkan={on|off}`.
- `AI_FILE_SORTER_GPU_BACKEND` selects `auto`, `cuda`, `vulkan`, or `cpu`.
- `AI_FILE_SORTER_N_GPU_LAYERS` overrides llama.cpp GPU offload selection.
- `AI_FILE_SORTER_CTX_TOKENS` overrides local LLM context length.
- `AI_FILE_SORTER_GGML_DIR` points to a custom ggml runtime directory when the
  packaged/default discovery path is not the right one.

## ChatGPT account backend (Codex subscription)

The ChatGPT account backend uses the standalone Codex app-server runtime over
its local stdio protocol. It is not the OpenAI API-key backend and does not
require an API key. The Microsoft Store Codex app is not required; configure a
standalone `codex.exe` on Windows (or the equivalent standalone `codex`
executable on another platform).

### Codex runtime discovery

AI File Sorter checks runtime locations in this order:

1. The executable path explicitly saved as `CodexExecutablePath`.
2. `codex.exe`, then `codex`, beside the AI File Sorter executable.
3. Each configured `PATH` entry, checking `codex.exe`, then `codex`.

Use **Settings → Select LLM → Browse…** when automatic discovery does not
find the standalone runtime. AI File Sorter never downloads or updates
`codex.exe` automatically.

### Isolated Codex home and inference directory

AI File Sorter gives the Codex runtime an isolated home under the app's config
directory and gives each app installation an isolated inference working
directory:

- Windows: `%APPDATA%\AIFileSorter\codex` and its `inference` child.
- macOS: `~/Library/Application Support/AIFileSorter/codex` and its `inference`
  child.
- Linux: `~/.config/AIFileSorter/codex` and its `inference` child.

When `AI_FILE_SORTER_CONFIG_DIR` is set, the corresponding `AIFileSorter`
config directory under that override is used instead. The app creates these
directories as needed. Do not copy Codex authentication files from another
profile; the runtime owns its authentication state in this isolated home.

### Sign-in and stored settings

In **Settings → Select LLM**, choose **ChatGPT account (Codex subscription)**,
then use **Sign in with ChatGPT** for browser sign-in or **Use device-code
sign-in** for the device-code fallback. **Sign out** clears the authenticated
runtime state used by the app-server.

AI File Sorter persists the selected executable path, ChatGPT model, and visual
backend ID in `config.ini`. It does not persist OAuth access or refresh tokens
in that file and does not read or copy token files from another Codex profile.

For example, the ChatGPT-related portion of `config.ini` contains selections
like these; authentication remains runtime-owned and is intentionally absent:

```ini
[Settings]
CodexExecutablePath=C:\Tools\codex.exe
ChatGptModel=gpt-5-codex
VisualModelId=chatgpt
```

Leave `ChatGptModel` empty to use **Auto**. The executable path may be left
empty when discovery should be used.

### Text and visual model selection

The model list comes from the authenticated Codex runtime. **Auto** uses the
runtime-reported default model. ChatGPT vision requires a selected model whose
capabilities include image input; if Auto has no image-capable default, choose
an explicit image-capable model. A text-only model is rejected for ChatGPT
vision rather than silently degraded.

The text and visual choices are independent. Supported combinations include
ChatGPT text with a local visual model and local text with ChatGPT vision. A
ChatGPT visual request sends a normalized inline PNG data URI, not the source
filesystem path. v1 can use two subscription-backed turns per image because
the visual description and text categorization are separate serialized turns.

Documents continue to use the existing text-extraction pipeline and are sent
as extracted text; the Codex backend does not use native document uploads.

## Visual-model configuration

- `LLAVA_MODEL_URL` and `LLAVA_MMPROJ_URL` override the built-in LLaVA download
  URLs.
- `GEMMA3_4B_MODEL_URL` and `GEMMA3_4B_MMPROJ_URL` override the built-in Gemma
  visual-model download URLs.
- `AI_FILE_SORTER_VISUAL_USE_GPU=0` forces the visual encoder to stay on CPU.

## Timeouts, pacing, and logs

- `AI_FILE_SORTER_LOCAL_LLM_TIMEOUT`
- `AI_FILE_SORTER_REMOTE_LLM_TIMEOUT`
- `AI_FILE_SORTER_CUSTOM_LLM_TIMEOUT`
- `AI_FILE_SORTER_REMOTE_REQUESTS_PER_MINUTE`
- `AI_FILE_SORTER_LLAMA_LOGS`

These knobs are most useful when diagnosing slow providers, rate-limited
providers, or local runtime issues.

Codex runtime startup, authentication, model-list, turn, and shutdown failures
are reported through the normal application logs. The Codex runtime is
account-backed and requires internet access, but the deterministic feature
tests use a fake app-server and do not require an account or network.

## Headless setting overlays

Headless callers can pass `--settings-overrides-file <json-file>` to inject a
non-persistent settings overlay for one run. This is the preferred way to steer
integration-specific behavior without rewriting the user's saved settings.

## Windows naming and migration note

New registry/settings/integration paths should use `HFStudio`. Compatibility
code may still need to read or clean legacy `Quicknode` locations during
migration or uninstall flows.

## Related references

- [Headless runtime contract](headless-runtime-contract.md)
- [Updater contract](updater-contract.md)
- [Windows release builds](windows-release-builds.md)
