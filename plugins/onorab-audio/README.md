# Onorab VST3 plugin (Phase 1)

Windows 64-bit VST3 named exactly **Onorab**, manufacturer **Potorik Track**,
bundle identifier `com.potoriktrack.onorab`. Built with C++20, JUCE 8.0.8
(pinned via FetchContent, tag `8.0.8` — never an unversioned branch), CMake.

## What this folder contains

| File | Role |
|---|---|
| `CMakeLists.txt` | JUCE FetchContent (8.0.8) + `juce_add_plugin` (VST3 only) |
| `Source/PluginProcessor.h/.cpp` | Audio pass-through, lock-free capture, WAV writer thread, settings/state |
| `Source/PluginEditor.h/.cpp` | 4 tabs: Cloud Library, Projects, Transfers, Settings; preview transport; drag in/out |
| `Source/HubClient.h/.cpp` | Localhost HTTP client for the Hub (background thread only, errors surfaced in UI) |
| `Source/CaptureFifo.h` | Lock-free SPSC ring buffer around `juce::AbstractFifo` |
| `Source/WaveformView.h/.cpp` | Waveform preview from a local file via `AudioThumbnail` |

## Build (Windows CI — the supported path)

The user does **not** need Visual Studio installed locally. GitHub Actions
builds this plugin on a Windows runner (see the repo's `.github/workflows/`,
owned by the parent task) and uploads a ZIP containing the `.vst3`.

Developer build on a Windows machine with Visual Studio 2022:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

Result: `build/Onorab_artefacts/Release/VST3/Onorab.vst3/`

> This plugin was **not** built on the Linux development VM (no JUCE headers,
> no Windows toolchain there). The code is written to compile on Windows CI
> with JUCE 8.0.8; do not claim a local build until CI reports one.

## Load it in FL Studio

1. Start Onorab Hub first (it must listen on `http://127.0.0.1:18741`;
   the Hub is a separate component — see the repo README).
2. Copy `Onorab.vst3` to `C:\Program Files\Common Files\VST3\`.
3. In FL Studio: **Options → Manage plugins → Find more plugins**, rescan,
   then verify **Onorab** appears under VST3 effects.
4. Load it on a Mixer Track (Mixer → select a track → slot → Onorab).
5. The Cloud Library tab shows the Hub sync status. If it says
   "Disconnected", the Hub is not running — start it and press **Refresh**.
6. **Preview:** select a file, press **Download**, then **Play** (Pause/Stop
   and the Preview gain slider are in the same row).
7. **Capture from FL:** play audio through that Mixer Track, press
   **Capture from FL**, then **Stop capture**. The WAV lands in the cache
   folder (default `%LOCALAPPDATA%\Onorab\cache`); press
   **Upload last capture** to send it to the Hub.
8. **Drag to FL:** download a file, then drag the violet **Drag to FL** box
   into FL Studio (Playlist / Channel Rack). This hands FL Studio a real
   file path via JUCE external drag-and-drop.
9. **Drop to upload:** drag audio files from Explorer onto the plugin
   window to queue them for upload to the Hub.

## Hub HTTP contract used here (Phase 1)

- `GET /health`, `GET /files`, `GET /transfers`, `GET /downloads/{id}`
- `POST /uploads` — `multipart/form-data`, single field named **`file`**
  (filename = the file name). The Hub must accept that field name.
- **No WebSocket client in Phase 1.** The editor polls `GET /transfers`
  and `GET /health` every 2 seconds. A WebSocket `/events` client is a
  Phase 2 item; polling is the honest Phase 1 behaviour.

## Real-time safety (hard rules in this code)

- `processBlock` does **no allocation, no locks, no file I/O, no network,
  no logging**. It only interleaves samples into a pre-allocated scratch
  buffer and pushes them into the lock-free `CaptureFifo`.
- A background writer thread drains the FIFO and writes the WAV file.
  If it falls behind, excess samples are counted as dropped and reported
  when capture stops — never silently lost.
- Audio passes through the Mixer Track untouched; the plugin observes
  audio for capture, it does not alter it.
- Waveform drawing and preview playback use `AudioThumbnail` /
  `AudioTransportSource` on the message thread, never on the audio thread.

## Security notes

- No credentials, tokens, or OAuth secrets are compiled into this plugin
  or stored in its state. `getStateInformation` persists only the Hub URL
  and cache folder, so an FL Studio project file can never leak a token.
- Google Drive OAuth lives entirely in the Hub. The Hub runs on localhost
  only; this plugin only ever talks to the configured Hub URL (default
  `http://127.0.0.1:18741`, editable in Settings).
- File names from the Hub are sanitised before they touch the cache folder
  (no `../` escapes).

## What is NOT done yet (Phase 1 honesty list)

- **No WebSocket `/events` client** — transfers/health are polled every 2 s.
- **No real Google Drive in this plugin** — Drive sync is Hub-side; with
  the Hub's Mock Drive Provider, the library shows mock audio files.
- **Projects are a placeholder** — names are local notes only; no project
  model is synced to the Hub yet.
- **Drag-out depends on the host.** The plugin provides the file path via
  JUCE external drag; FL Studio must accept the drop (its Playlist /
  Channel Rack accept audio files, but the plugin cannot force acceptance).
- **Upload progress is coarse** (sent/done) because JUCE's HTTP upload
  gives no byte-level progress callback; download progress is byte-based.
- No payments, subscriptions, collaboration, OneDrive, Dropbox, or
  advanced user accounts (out of Phase 1 scope by design).
