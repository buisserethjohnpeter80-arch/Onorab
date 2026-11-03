# Onorab — Setup (Phase 1 MVP)

> Phase 1 runs the Hub in **Mock Drive mode** and the mobile app in
> **mock mode**. No Google credentials are needed for Phase 1. Do not
> create real OAuth clients until Phase 2.

## 1. What you need

| Item | Required for Phase 1? |
|------|------------------------|
| Windows 10/11 64-bit PC with FL Studio installed | Yes, to test the plugin |
| Android phone or emulator (for the mobile UI) | Yes, to test the mobile UI |
| GitHub account (to download CI artifacts) | Yes — see §2 |
| Visual Studio Community installed locally | **No — CI builds the VST3 and Hub** |
| Google Cloud project / OAuth credentials | **No in Phase 1 (Mock mode)** |

The "no local Visual Studio" rule is deliberate: the Windows workflow
(`.github/workflows/windows.yml`) builds the VST3 with the Visual
Studio 2022 toolchain on a GitHub-hosted runner. You download the
finished ZIP; you do not compile the plugin yourself.

## 2. Get a Phase 1 build (CI artifacts)

1. Open the Onorab repository on GitHub -> **Actions**.
2. Run (or open the latest run of) the **Windows** workflow.
3. Download the artifact named `onorab-windows` and unzip it. It
   contains:
   - `Onorab.vst3` (the VST3 plugin directory)
   - `onorab-hub.exe` (the Hub)
4. For Android, open the latest run of the **Android** workflow and
   download the artifact named `onorab-android-debug`. It contains an
   **unsigned debug APK** for testing only — it is not a release build
   and is not signed for distribution.

If no artifact exists yet, the workflows have not run successfully;
see `release.md` and the workflow run logs. Do not build by hand and
call it a CI build.

## 3. Environment variables

All Hub configuration is via environment variables. Defaults are
Mock-friendly so a bare run works.

| Variable | Default | Meaning |
|----------|---------|---------|
| `ONORAB_HUB_PORT` | `18741` | Localhost port the Hub listens on. |
| `ONORAB_DRIVE_PROVIDER` | `mock` | Drive provider. Phase 1: `mock` only. `drive` is reserved for Phase 2 (real Google Drive). |
| `ONORAB_CACHE_DIR` | OS user cache dir + `onorab` (e.g. `%LOCALAPPDATA%\onorab\cache`) | Local cache for downloaded audio files. Contains audio files only — never tokens (see `security.md`). |
| `ONORAB_CREDENTIALS_PATH` | (unset) | Path to the Desktop OAuth client JSON. **Phase 2 only.** Unused and ignored in Mock mode. Never point this at a file inside the repo. |
| `ONORAB_HUB_URL` | `http://127.0.0.1:18741` | URL the plugin (and any local tooling) uses to reach the Hub. Must stay loopback. |

Example (Windows Command Prompt, Mock mode):

```cmd
set ONORAB_HUB_PORT=18741
set ONORAB_DRIVE_PROVIDER=mock
onorab-hub.exe
```

Example (PowerShell):

```powershell
$env:ONORAB_HUB_PORT = "18741"
$env:ONORAB_DRIVE_PROVIDER = "mock"
.\onorab-hub.exe
```

### 3.1 `.env.example` and `config.example.json`

The repository ships templates — copy them, never edit them in place
with real values:

```cmd
copy .env.example .env
copy config.example.json config.json
```

- `.env` is git-ignored. Put only non-secret local settings there
  (port, provider, cache dir). Real secrets do not belong in `.env`
  either — credentials come from `ONORAB_CREDENTIALS_PATH` and tokens
  from the OS keychain (see `security.md`).
- `config.json` is git-ignored. It holds the same non-secret settings
  in JSON form for tooling that prefers a file. The example file shows
  the shape with placeholder values only.

If either template is missing from your checkout, use the table above
as the source of truth and flag it to the maintainer — do not invent
extra variables.

## 4. Run the Hub (Mock mode)

1. Unzip the CI artifact (§2) into a folder of your choice, e.g.
   `C:\onorab\`.
2. Open a terminal in that folder.
3. Run (Mock mode is the default; setting it explicitly is recommended):

   ```cmd
   set ONORAB_DRIVE_PROVIDER=mock
   onorab-hub.exe
   ```

4. Verify it is up, from a second terminal or a browser on the same
   machine:

   ```cmd
   curl http://127.0.0.1:18741/health
   ```

   Expected: an HTTP 200 response with a JSON body reporting status
   `ok` (exact fields are defined in `packages/protocol`; if the shape
   differs, the protocol schema — not this doc — is authoritative).
5. Keep the Hub running while FL Studio is open. The plugin connects to
   this same Hub process; closing the Hub puts the plugin into the
   **Disconnected** state (see `testing.md`).
6. To stop the Hub: `Ctrl+C` in its terminal, or close its window.

Troubleshooting:

- **Port already in use:** another Hub (or another program) is already
  on port 18741. Stop the other process, or set `ONORAB_HUB_PORT` to a
  free port and set `ONORAB_HUB_URL` to match — but the URL must still
  be `127.0.0.1`.
- **Plugin shows Disconnected:** check the Hub terminal is still
  running and `curl http://127.0.0.1:18741/health` succeeds. If the Hub
  is up but the plugin is not connecting, check that
  `ONORAB_HUB_URL` matches the Hub's actual port.
- **No files in the library:** in Mock mode the library is seeded with
  mock audio files. If it is empty, check the Hub logs for provider
  errors and file an issue with the log excerpt (redact any paths
  that contain your Windows username if you prefer).

## 5. Load the plugin in FL Studio

1. Start the Hub first (§4).
2. Copy the `Onorab.vst3` directory from the CI artifact into your
   VST3 folder, e.g. `C:\Program Files\Common Files\VST3\`.
3. Open FL Studio. If it was already open, rescan plugins:
   **Options -> Manage plugins -> Find plugins** (or the equivalent
   in your FL Studio version).
4. Confirm **Onorab** appears in the plugin list.
5. Load it on a Mixer Track: select a Mixer Track, open an effect slot,
   choose **Onorab**.
6. The plugin should show **Sync status: Connected** and populate the
   **Cloud Library** with the mock audio files. If it shows
   Disconnected, see the troubleshooting in §4 — do not proceed to
   capture/upload tests until the connection is green.

## 6. Mobile app (mock mode)

1. Install the unsigned debug APK from the Android CI artifact (§2) on
   a phone or emulator. Your device will warn that the app is from an
   unknown source / unsigned debug build — that is expected for a
   Phase 1 test build.
2. Open **Onorab** on the phone. The first screen is the Google account
   connection screen; in mock mode it offers a mock sign-in (no real
   Google account, no real OAuth flow).
3. After mock sign-in, the Cloud Library shows mock audio files, and
   the Player, Projects, Transfers, and Profile screens work against
   mock data.
4. Share Target: from another app (WhatsApp, Files, Telegram), share an
   audio file and choose Onorab. In Phase 1 the app accepts the share
   intent and shows the file in its local flow; real Drive upload from
   mobile is Phase 2 work.
5. Do not enter real Google credentials into the mock-mode app, and do
   not configure a real Android OAuth client yet — the Android OAuth
   client is a separate Phase 2 deliverable (see `security.md`).

## 7. Switching to real Drive (Phase 2 — not yet)

Real Drive support is **not** part of Phase 1. When it lands, the steps
will be: create a Desktop OAuth client (Hub) and a separate Android
OAuth client (mobile), set `ONORAB_DRIVE_PROVIDER=drive` and
`ONORAB_CREDENTIALS_PATH` to the Desktop client JSON outside the repo,
complete the PKCE OAuth flow once, and let the Hub store tokens in
Windows Credential Manager. Do not attempt this with Phase 1 builds —
the real provider does not exist yet, and setting `drive` in Phase 1
must fail with a clear error, not silently fall back to mock data.

## 8. Quick reference

```text
Hub health:   curl http://127.0.0.1:18741/health
Hub files:    curl http://127.0.0.1:18741/files
Hub transfers:curl http://127.0.0.1:18741/transfers
Hub events:   WebSocket ws://127.0.0.1:18741/events
Default port: 18741 (ONORAB_HUB_PORT)
Default provider: mock (ONORAB_DRIVE_PROVIDER)
```
