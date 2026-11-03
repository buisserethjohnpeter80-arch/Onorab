# Onorab — Phase 1 MVP

Onorab is a universal bridge between desktop software and mobile phones.
The architecture supports future connectors for FL Studio, Logic Pro,
Blender, Photoshop, Premiere Pro, Maya, and other software.

**Phase 1 vertical slice (only):** FL Studio on Windows ↔ Google Drive
(Mock Drive Provider) ↔ Android phone, via a localhost Hub and a VST3
plugin. No payments, subscriptions, collaboration, OneDrive, or Dropbox.

## What is in this repo

```
plugins/onorab-audio/   C++20, JUCE 8.0.8, CMake, VST3 named "Onorab"
apps/onorab-hub/        Go (stdlib only), localhost HTTP + WebSocket, Mock Drive
apps/onorab-mobile/     Ionic + Angular + Capacitor, Android first, mock mode
packages/protocol/      Shared JSON schemas (files, transfers, events, errors, capabilities)
docs/                   Architecture, setup, security, testing, release
.github/workflows/      Windows (VST3 + Hub) and Android (debug APK) CI
```

## Quick start — test it in 5 minutes (no Visual Studio needed locally)

1. **Start the Hub (Mock Drive):**
   ```powershell
   cd apps/onorab-hub
   go run .
   curl http://127.0.0.1:18741/health
   curl http://127.0.0.1:18741/files
   ```
   You should see 3 mock audio files. Download one:
   ```powershell
   curl http://127.0.0.1:18741/downloads/mock_voice_memo -o voice-memo.wav
   ```
2. **VST3:** download `onorab-windows.zip` from a GitHub Actions run
   (see `docs/release.md`), copy `Onorab.vst3` to your VST3 folder,
   load it on an FL Studio Mixer Track, and follow the manual checklist
   in `docs/testing.md`. The plugin talks to the Hub on
   `http://127.0.0.1:18741` and polls its library every 2 seconds in
   Phase 1 (WebSocket `/events` exists on the Hub; the plugin's WS
   client is Phase 2).
3. **Mobile (mock mode):** see `apps/onorab-mobile/README.md`. The app
   runs with mock data by default; real Android OAuth and real Drive
   upload from the phone are Phase 2.

## Configuration (no secrets in the repo)

Copy the examples and fill in your own local values only:

```powershell
copy .env.example .env
copy config.example.json config.json
```

- `ONORAB_HUB_PORT` (default `18741`), `ONORAB_DRIVE_PROVIDER=mock`
  (Phase 1 only — any other value fails fast at startup),
  `ONORAB_CACHE_DIR`, `ONORAB_HUB_URL`.
- **Phase 2 only:** `ONORAB_CREDENTIALS_PATH` points to your Desktop
  OAuth JSON on disk. That file is **never** compiled into the VST3 or
  the Hub, and **never** committed (see `.gitignore`).
- The Android app must use its **own Android OAuth client** in Phase 2.
  Never reuse the Desktop OAuth client inside the Android app.

## Build status — read this before claiming anything works

- ✅ Hub: `go test ./...` passes on this machine (see report in the
  delivery message; the exact count is printed by the test run).
- ✅ Protocol schemas parse as JSON; Hub responses match them.
- ⚠️ VST3 and Android builds have **not** run yet: they require the
  first GitHub Actions run (Windows runner for JUCE/VS 2022, Ubuntu
  runner for the APK). The workflows are written and YAML-valid, but
  no `onorab-windows.zip` and no APK exist until that first CI run.
- ⚠️ The manual FL Studio checklist in `docs/testing.md` has not been
  run against a real FL Studio session yet — it is a checklist, not a
  passed result.

## Docs

- `docs/architecture.md` — design, RT-thread rules, connector model
- `docs/setup.md` — exact Windows / mobile setup
- `docs/security.md` — OAuth PKCE, `drive.file` scope, keychain rules
- `docs/testing.md` — Hub tests + manual FL Studio checklist
- `docs/release.md` — versioning and CI artifacts
