# Onorab Mobile — Phase 1 MVP

Ionic + Angular + Capacitor app (Android first) for the Onorab bridge:
phone ↔ Google Drive ↔ Onorab Hub ↔ FL Studio (via the Onorab VST3).

**Phase 1 is mock mode by default.** The app renders mock files, a mock
account, and mock transfers. It does not sign into Google, does not
upload to Drive, and does not claim otherwise. Every mock surface is
labelled "MOCK MODE" in the UI.

## What is in this folder

| Path | Purpose |
|---|---|
| `capacitor.config.ts` | appId `com.potoriktrack.onorab`, appName `Onorab` |
| `src/app/pages/` | Connect, Library, Player, Upload, Projects, Transfers, Profile (standalone components, no NgModule) |
| `src/app/components/sync-indicator/` | Sync pill used on every page header |
| `src/app/services/mock-data.service.ts` | Mock files / projects / transfers matching the shared protocol fields (`id, name, mimeType, sizeBytes, modifiedTime, projectId, source`) |
| `src/app/services/hub.service.ts` | Hub HTTP client, used only when mock mode is off |
| `src/app/services/share-intent.service.ts` | Share-intent stub (see "Share Target" below) |
| `src/app/services/player-state.service.ts` | Selected-file state shared between Library and Player |
| `src/assets/sample.wav` | Bundled 2-second 440 Hz tone so the Player has real, playable audio in mock mode |
| `android/app/src/main/AndroidManifest.xml` | Launcher + `ACTION_SEND` / `ACTION_SEND_MULTIPLE` Share Target filters (`audio/*`, plus a documented `*/*` fallback) |
| `android/app/src/main/java/com/potoriktrack/onorab/MainActivity.java` | Capacitor `BridgeActivity` host, no custom native code yet |

Pinned versions (see `package.json`, no unversioned `main` branches):
Angular 18.2.0, Ionic 8.4.0, Capacitor 6.2.0 (core / cli / android),
TypeScript 5.5.4, RxJS 7.8.1, zone.js 0.14.10.

## Install

```bash
cd apps/onorab-mobile
npm install
```

## Run in the browser

`npm start` runs `ng serve` — the `ionic serve` equivalent for this
project (same dev server, same app, without the Ionic CLI wrapper).
Open the printed `http://localhost:4200` URL. The browser preview:

- Shows all 7 tabs with mock data.
- Plays `onorab-demo-tone.wav` in Player (the only mock file with real bytes).
- Cannot receive Android share intents (there is no Android intent
  system in a browser) — the Upload page states this.

## Hub connection (only when mock mode is off)

Mock mode is controlled by `src/environments/environment.ts`
(`mockMode: true`). To point the app at a real Hub, set
`mockMode: false` in the environment file and rebuild.

Hub base URL resolution (see `hub.service.ts`):

- **Android emulator:** `http://10.0.2.2:18741` — the emulator's alias
  for the host PC's localhost. Plain `localhost` inside an emulator
  points at the emulated phone, not your PC, and is the most common
  "Hub unreachable" mistake.
- **Browser on the same PC as the Hub:** `http://localhost:18741`.
- **Real device on the same Wi-Fi:** the PC's LAN IP, e.g.
  `http://192.168.1.42:18741` (not hardcoded — it changes per network;
  Phase 2 should add a Hub address field in Settings).

Hub errors are shown on screen with the Hub URL included; they are
never silently replaced by mock data.

## Build a debug APK

**Do not claim a local APK build from this folder alone.** The signed
build path is the repo's GitHub Actions Android workflow, which
installs dependencies, builds the web app, syncs Capacitor, and builds
the unsigned debug APK. Locally, the equivalent steps (after
`npm install`) are:

```bash
npm run build
npx cap sync android
npx cap open android   # then Build > Build App Bundle(s) / APK(s) in Android Studio
```

Notes:

- `npx cap sync android` generates/refreshes the Android project
  around the committed `AndroidManifest.xml` skeleton. If `android/`
  is missing on a fresh checkout, `npx cap add android` creates it —
  then re-apply the Share Target intent-filters from the committed
  manifest (a fresh `cap add` manifest has no SEND filters).
- This repo does not commit a keystore, `google-services.json`, or
  any credential. Debug APKs are unsigned-debug builds for testing.

## How Share Target will be verified (real device, Phase 1 vs Phase 2)

Phase 1 verification (what can be checked today):

1. Install the debug APK on a real Android device.
2. In Files / WhatsApp / Telegram, share an audio file and confirm
   **Onorab appears in the share sheet** and tapping it **opens the
   app**. That proves the manifest intent-filters work.
3. Expected Phase 1 result: the app opens, but the shared file does
   **not** appear in Upload — importing shared bytes is not wired yet
   (see below). If the file appears, that is a Phase 2 feature landing
   early, not a Phase 1 pass.

Phase 2 verification (not yet possible):

1. Share audio to Onorab; the file appears under "Shared to Onorab"
   on the Upload page with its real name.
2. Upload it; it appears in the Cloud Library and then in the VST3.

## What is NOT done (Phase 1 limits — honest list)

- **Real Android OAuth:** no Android OAuth client is configured, no
  client id is hardcoded anywhere, and the Desktop OAuth client is
  never reused in this app. The Connect page's Google button is
  disabled and says so. Phase 2 needs a dedicated Android OAuth
  client (Google Cloud Console, Android application type) plus token
  handling that never stores tokens in plain files.
- **Real Drive upload/download from the phone:** mock only. The
  Upload page's mock progress bar is labelled as a UI simulation.
- **Share import:** manifest filters + app launch only. Reading the
  shared content URI and its bytes is Phase 2 (native content-resolver
  read + JavaScript handoff in `share-intent.service.ts`).
- **Real waveform decoding:** Player canvas bars are a deterministic
  placeholder seeded from the file id, not decoded samples.
- **Hub Projects endpoint:** the Hub has no `/projects` endpoint in
  Phase 1, so Projects in non-mock mode shows that plainly instead of
  fake projects.
- **No automated mobile tests** in Phase 1. Hub protocol / transfer
  tests live with the Hub component (parent scope), not here.

## Security notes

- No `credentials.json`, client secret, access token, or refresh
  token in this folder — `.gitignore` blocks them (`credentials.json`,
  `*.keystore`, `*.jks`, `google-services.json`, `.env`).
- The app never writes tokens into any file, project, or local storage
  in Phase 1, because it never obtains one.
- Only the `drive.file` scope is planned for Phase 2 (same scope as
  the Hub); no broader Drive scope.
