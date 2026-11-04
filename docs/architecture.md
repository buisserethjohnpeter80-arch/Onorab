# Onorab — Architecture (Phase 1 MVP)

> Status: Phase 1 vertical slice. Mock Drive Provider only. No payments,
> subscriptions, collaboration, OneDrive, Dropbox, or advanced user accounts.
> Nothing in this document claims a feature works until it is built and tested.

## 1. Product in one paragraph

Onorab is a universal bridge between desktop software and mobile phones.
Phase 1 proves one vertical slice: audio exchange between FL Studio on
Windows, Google Drive, and an Android phone. The design is connector-based
so future connectors (Logic Pro, Blender, Photoshop, Premiere Pro, Maya,
and others) can be added without redesigning the Hub or the protocol.

## 2. Monorepo layout

```text
onorab/
├── plugins/
│   └── onorab-audio/        # C++20, JUCE, CMake, VST3 named "Onorab"
├── apps/
│   ├── onorab-hub/          # Go, single Windows executable, localhost only
│   └── onorab-mobile/       # Ionic + Angular + Capacitor, Android first
├── packages/
│   └── protocol/            # Shared JSON schemas (files, transfers,
│                            #   events, errors, connector capabilities)
├── docs/                    # This documentation set
└── .github/workflows/       # CI: windows.yml, android.yml
```

Rules:

- `plugins/`, `apps/`, and `packages/` are owned by their own builds; the
  Hub never imports plugin code and the plugin never imports Hub code.
- `packages/protocol` is the single source of truth for wire shapes. If a
  field is not in a schema, it does not cross a process boundary.
- Secrets are never part of any of these trees (see `security.md`).

## 3. Runtime topology (Phase 1)

```text
┌──────────────────────────┐
│ FL Studio (Windows)      │
│  ┌────────────────────┐  │
│  │ Onorab VST3        │  │
│  │ Cloud Library ·    │  │
│  │ Projects · Transfers│ │
│  │ Settings · Preview │  │
│  └──────┬─────────────┘  │
└─────────┼────────────────┘
          │ HTTP + WebSocket, localhost only
          │ (default port 18741, see setup.md)
┌─────────▼────────────────┐
│ Onorab Hub (Go service)  │
│  · REST: /health, /files,│
│    /uploads, /downloads/ │
│    {fileId}, /transfers  │
│  · WebSocket: /events    │
│  · Local cache           │
│  · OAuth session guard   │
│  · Drive provider layer  │
└────┬───────────────┬─────┘
     │               │
     │ Phase 1:      │ Future (not Phase 1):
     │ Mock Drive    │ Real Google Drive API
     │ Provider      │ (drive.file scope, PKCE)
     │ (in-memory /  │
     │  local files) │
     └───────────────┘
┌──────────────────────────┐
│ Onorab Mobile (Android)  │
│  · Cloud Library · Player│
│  · Upload / Download     │
│  · Projects · Transfers  │
│  · Profile · Sync status │
│  · Share Target (receive │
│    audio from WhatsApp / │
│    Files / Telegram)     │
│  · Mock mode in Phase 1  │
└──────────────────────────┘
```

### 3.1 Hub is localhost-only

- The Hub binds to `127.0.0.1` only — never `0.0.0.0`. It must refuse to
  start if asked to bind a non-loopback address.
- The plugin and the Hub run on the same Windows machine. There is no
  remote Hub, no inbound internet port, and no multi-user server in
  Phase 1.
- The WebSocket `/events` channel is likewise localhost-only and carries
  transfer progress/status, not tokens.

### 3.2 Plugin <-> Hub

- Transport: plain HTTP on localhost plus one WebSocket (`/events`) for
  live transfer events. No custom binary protocol in Phase 1.
- The plugin polls nothing for progress: it subscribes to `/events` and
  renders upload/download progress from those events.
- The plugin shows two Hub states explicitly: **Connected** and
  **Disconnected**. On disconnect it must show a clear error message and
  a retry action — it must not silently show a stale library.
- The plugin never talks to Google Drive directly. All Drive access goes
  through the Hub, so Drive credentials exist in exactly one process.

### 3.3 Hub <-> Drive

- Phase 1: the **Mock Drive Provider**. It implements the same provider
  interface (list / upload / download) as the future real provider, using
  local/mock data, so development and CI work with no Google credentials.
- Future: the real Google Drive provider, using OAuth 2.0 with PKCE and
  the `drive.file` scope only (see `security.md`). The provider boundary
  means swapping Mock for Drive does not change the plugin or the API.

### 3.3a Project folder model (user decision, 2026-11-03)

- On Drive there is exactly ONE dedicated Onorab folder tree:
  `Onorab_projet/<project>/`, e.g. `Onorab_projet/marcus music/`.
  It is created on the FIRST upload if it does not exist, then reused;
  it is never recreated per upload, and no other Drive folder is ever
  touched. (With `drive.file` scope the app can in any case only see
  files it created or the user explicitly opened with it.)
- A file's project is the protocol's existing `projectId` field — a
  plain display name, never a path. The Hub sanitises it (`/` and `\`
  become `-`, `.`/`..` rejected) so a project name can never escape
  the dedicated folder. In the Mock provider the project is stored as
  `projectId`; the real Drive provider translates the same value into
  the `Onorab_projet/<project>/` folder path.
- Plugin flows built on this model (no separate Download button):
  **Take from Drive** downloads selected Library sounds to the local
  cache in one step, ready to preview and drag into FL Studio.
  **Upload one / Upload all** pick local files into a typed project.
  **Push** uploads the LOCAL copies of selected project sounds into
  `Onorab_projet/<project>/`; a sound with no local copy is skipped
  with a visible message (Take it first). Play/Pause preview always
  stays available before a sound enters FL Studio.
- Honest limit, stated in the UI: FL Studio cannot list its own
  project tracks automatically — a VST sees only its own Mixer Track
  audio, not the whole FL project. Local sounds enter a project only
  via Upload or Push, never by silent auto-discovery.

### 3.3b Onorab Bus routing (user logic, 2026-11-03)

- A VST cannot create an FL Mixer track or route FL tracks — FL Studio
  owns its Mixer, and no VST API exposes track creation or routing.
  The Onorab Bus is therefore a USER workflow in FL Studio, supported
  (not performed) by the plugin:
  1. In the FL Mixer, pick an empty track, name it `Onorab`, and load
     the Onorab plugin on it. This track is used ONLY for Onorab.
  2. Select the track(s) to capture and route them to the Onorab
     track with **Route to this track only** (so the track is heard
     once, through the bus to Master, not twice via Master + bus).
  3. For track-by-track capture: route ONE track, type its name in
     the plugin's capture name box (e.g. `Vocal`), press
     **Capture from FL**, then **Stop capture**. The file is named
     `<name>-<timestamp>.wav`, so it stays identifiable when uploaded
     or pushed to a project.
  4. For a mix capture: route ALL wanted tracks to the bus at once
     and capture with no name (auto `capture-<timestamp>.wav`).
  5. After capturing, remove the route so the track returns to its
     normal Master routing.
- The plugin's part of this logic is only: capture whatever audio
  arrives on its own track (unchanged pass-through), name the capture
  after the typed track name, and offer Upload / Push into
  `Onorab_projet/<project>/`. It never claims to have routed anything
  itself.

### 3.3c Delete, creation order, Drive connect, invites (2026-11-03)

- **Delete:** the Library and Projects tabs have a **Delete** button.
  It deletes the selected sound(s) from the Hub (Drive) via
  `POST /files/{id}/delete` (DELETE `/files/{id}` also works for API
  clients) and, only on Hub success, their local cache copies too.
  Unknown ids are 404, never silent success. Mock limitation: a Hub
  restart re-seeds the 3 mock files, so deleting a mock file is not
  permanent until the real Drive provider exists.
- **Creation order (not alphabetical):** `GET /files` returns files in
  creation order — seed order first, then uploads in upload order.
  The plugin never re-sorts; Library and Projects filters preserve
  Hub order. An invited user uploading from another FL Studio therefore
  appears at the end of the project list, in order, instead of jumping
  into alphabetical position. (Previously the Hub sorted by name;
  that ordering is removed.)
- **Settings → Google Drive:** the user enters a Gmail and presses
  **Connect Drive**. The plugin asks the Hub (`GET /auth/status`,
  then `POST /auth/start`). In Mock mode this honestly reports
  `oauth_unavailable` — no fake login, nothing sent to Google. With
  the real Drive provider, the same button starts real OAuth (PKCE,
  `drive.file` only, tokens in the OS keychain, never in FL projects).
- **Settings → Invite to a project:** the user enters an invited
  Gmail and a project name and presses **Invite**. The Hub records a
  pending invite (`GET/POST /invites`, schema
  `packages/protocol/invite.json`; Mock records are forgotten on Hub
  restart). The real Drive provider turns a pending invite into a
  Drive sharing permission for that Gmail on
  `Onorab_projet/<project>/`, so the invited user sees the same
  project, in the same creation order, from their own FL Studio.

### 3.4 Commercial multi-PC model (user decision, 2026-11-03)

The VST is a commercial product installed on many PCs. That changes
the credential model completely — the Phase 1 assumption ("the
developer's credentials.json lives on his PC only") is WRONG for a
shipped product and is replaced by this:

- **The OAuth client is Onorab's app identity, not a per-PC secret.**
  Every installation uses the SAME Google OAuth client (one Desktop
  app client, created once in Google Cloud Console). A buyer never
  needs their own Google Cloud project, and never uses the seller's
  Google account.
- **Every PC signs in with its owner's Gmail.** First run: Settings →
  Connect Drive → Hub returns a Google sign-in URL → plugin opens it
  in the browser → user approves `drive.file` only → Google redirects
  to the Hub's loopback listener → Hub exchanges the PKCE code for
  tokens. The plugin never sees a password; the Hub never receives
  one.
- **Tokens stay on that PC only.** The refresh token goes to Windows
  Credential Manager (`Onorab:drive:default`, per Windows user). The
  access token lives in Hub memory only. Copying the VST/Hub to
  another PC does NOT carry a sign-in: that PC signs in itself.
  After the first sign-in, reconnect is automatic: the Hub loads the
  refresh token and mints fresh access tokens without a browser.
- **No secret in the repo; id + Desktop secret in the release binary
  only.** The distributed Hub carries the SAME id + secret pair as
  the Desktop OAuth JSON, both injected from GitHub Actions secrets
  (`ONORAB_OAUTH_CLIENT_ID`, `ONORAB_OAUTH_CLIENT_SECRET`) via
  `-X main.buildOAuthClientID=... -X main.buildOAuthClientSecret=...
  -X main.buildDistribution=true` (see the Build Hub step note in
  the workflows; the Muse GitHub App cannot edit workflow files, so
  the owner adds those flags + secrets manually, plus a guard step
  that fails the build when either secret is empty). Verified
  2026-11-04: an id-only distributed build opened the browser and
  Google approved, but the code exchange failed and /auth/status
  stayed authenticated=false, while the same Hub launched with the
  JSON's id + secret worked — so the pair must travel together, and
  a distribution build with an id but no secret FAILS FAST at
  startup instead of shipping silently broken. Why this is safe: for
  Google Desktop (installed) apps the client secret is NOT a
  confidential secret — it ships inside the app by design and grants
  nothing on its own; it only works with a code the user approved in
  their own browser, under PKCE S256 + state verification, for
  drive.file only. What must truly stay private — each user's
  refresh token — is never in the binary or the repo, only in that
  PC's Credential Manager. No backend proxy is needed, because no
  truly-private secret is distributed; if one ever were, THAT part
  would move to a backend instead of into the exe. Dev runs use
  `ONORAB_OAUTH_CLIENT_ID` (+ `ONORAB_OAUTH_CLIENT_SECRET`) env or a
  Desktop OAuth JSON at `ONORAB_CREDENTIALS_PATH` — dev only, never
  committed, never sent in chat.
- **Installer + auto-launch (2026-11-04).**
  `dist/windows/installer.nsi` (NSIS) installs Onorab.vst3 to
  `Common Files\VST3` and onorab-hub.exe to `Program Files\Onorab`
  (admin; uninstaller included; the CI step that runs makensis is
  owner-added like the other workflow steps). On open, the plugin
  checks the default localhost Hub; if it is not running, it
  launches the installed Hub with `-provider drive` once (JUCE
  ChildProcess launches with CREATE_NO_WINDOW, so no console
  window; the child survives FL closing, so reopening FL finds a
  Hub already running; multiple plugin instances share the one Hub
  via the health check — a second instance never launches another).
  The musician never touches PowerShell, JSON, env vars, or Google
  Cloud Console. Because an auto-launched Hub has no console, its
  logs also go to `%LOCALAPPDATA%\Onorab\hub.log`.
- **Sign-in messages (2026-11-04).** The browser success page
  appears ONLY after the code exchange AND the refresh-token store
  both succeed; every failure (user cancel, refused exchange,
  keychain store failure, timeout) writes an error page and is
  reported by /auth/status as `authError`, which the plugin shows
  with a "Try again" button. While a sign-in is in flight the plugin
  shows "Waiting for Google…" and polls auth status every 2 seconds
  instead of 10. A revoked/expired refresh token (`invalid_grant`)
  is deleted from the keychain so a fresh Connect starts clean;
  transient refresh failures keep the token. No page, message, or
  log ever shows a token, code, or client secret.
- **Testing-mode limits (until Production + verification):** only
  Test users (max 100, added in the OAuth consent screen) can sign
  in; every new user sees the "Google hasn't verified this app"
  screen (Advanced → Go to Onorab); refresh tokens expire after
  **7 days**, so buyers re-sign weekly. Before selling: publish the
  app to Production and pass Google verification (consent screen
  with app homepage, Privacy Policy URL, Terms, support contact,
  and a demo video showing the drive.file use). drive.file is the
  narrowest Drive scope — it is what makes verification plausible;
  full-drive scope would not be.
- **drive.file sharing limitation (honest):** an invited user's Hub
  lists a shared project once Drive shows that folder in their
  "Shared with me" view. The plugin's invite status says so; it does
  not promise instant cross-PC visibility.
- **Still to do for a professional release (not in this batch):**
  code signing so Windows SmartScreen does not warn on install; a
  license-key system if sales are per-seat; versioned auto-update
  for Hub + VST; and crash/error reporting that never includes
  tokens or audio content. The installer script exists
  (`dist/windows/installer.nsi`); the CI step that builds it is
  owner-added. None of these change the Drive model above.

### 3.4 Mobile <-> Drive

- Phase 1: the mobile app runs in **mock mode** with local mock data. It
  does not perform real Drive uploads/downloads yet.
- Future: the mobile app talks to Google Drive with **its own Android
  OAuth client**. The Desktop OAuth client used by the Hub must never be
  reused inside the Android app (different client type, different
  redirect handling; reusing it would also leak a desktop credential
  into a distributed app binary).
- The phone-to-FL and FL-to-phone flows in the product vision both pass
  through Drive as the shared store; the Hub is the Windows-side agent
  that makes Drive files visible inside FL Studio, and the mobile app is
  the phone-side agent. They do not talk to each other directly.

## 4. Connector capability model

Future connectors (FL Studio is the first; Logic Pro, Blender, Photoshop,
Premiere Pro, Maya follow) are described by a capability record in
`packages/protocol` (see the `connector` schema there). Conceptually:

```json
{
  "connectorId": "fl-studio",
  "displayName": "FL Studio",
  "platforms": ["windows"],
  "hostKind": "daw",
  "capabilities": {
    "audioCapture": true,
    "audioPreview": true,
    "dragDropOut": true,
    "fileImport": true,
    "projectSync": false
  },
  "transports": ["vst3"],
  "notes": "Phase 1: VST3 on a Mixer Track; capture + drag-out only."
}
```

Rules:

- A connector declares only what it can actually do. A host that cannot
  capture audio must not advertise `audioCapture`.
- Capability checks happen before UI actions are enabled: e.g. the
  "Capture from FL" button is only enabled when the active connector
  advertises `audioCapture`.
- Adding a connector means: add its capability record, implement its
  transport (plugin format, bridge, or file watcher), and implement its
  provider mapping. It must not require changes to the Hub's REST shape
  or to other connectors.

Planned (not Phase 1) capability notes:

| Connector   | Host kind | Likely transport | Notes |
|-------------|-----------|------------------|-------|
| FL Studio   | DAW       | VST3 (Phase 1)   | Capture + drag-out |
| Logic Pro   | DAW       | AU (future)      | macOS host; Hub port needed |
| Blender     | 3D        | Add-on (future)  | Asset/file exchange, not audio capture |
| Photoshop   | Image     | Plugin (future)  | Image exchange |
| Premiere Pro| Video     | Panel (future)   | Media exchange |
| Maya        | 3D        | Plug-in (future) | Asset/file exchange |

## 5. Real-time audio thread rules (VST3, non-negotiable)

The audio processing thread (`processBlock`) is real-time. Violating any
of these rules risks dropouts and FL Studio instability:

1. **No network access** on the audio thread. No HTTP, no WebSocket, no
   Drive calls.
2. **No file writing** on the audio thread. Captured audio is not written
   to disk here.
3. **No memory allocation** on the audio thread. No `new`, no
   containers that grow, no string formatting, no locks that can block.
4. **No blocking locks, no UI calls** on the audio thread.
5. **Lock-free FIFO / ring buffer only.** The audio thread pushes
   captured samples into a lock-free single-producer/single-consumer
   FIFO. A background writer thread pops from the FIFO and writes the
   local WAV file. If the FIFO is full, the audio thread drops samples
   (and counts them for a later, non-real-time error report) — it never
   blocks waiting for the writer.
6. **Errors are reported off-thread.** The audio thread records an error
   flag/counter; the message thread reads it and shows a clear error in
   the UI. Errors are never silently swallowed and never thrown across
   the audio callback.
7. **Capture path:** Mixer Track audio -> audio thread -> lock-free FIFO
   -> background writer thread -> local WAV file -> Hub upload (HTTP,
   background thread only) -> Drive.
8. **Preview/playback path:** file decoding and UI waveform rendering
   happen off the audio thread; only already-decoded buffers are handed
   to the audio callback.

## 6. Token and credential rules (summary)

Full rules are in `security.md`. The architecture enforces them by
construction:

- Tokens live in the OS-protected keychain (Windows Credential Manager
  for the Hub) — never in an FL Studio project file, never in plugin
  state, never in the Hub's cache directory.
- The OAuth client credential JSON is loaded from a local path or
  environment variable at runtime. It is **never compiled into** the
  VST3 or Hub executable and never committed to git.
- Scope is `drive.file` only. The Hub and the plugin request nothing
  broader.
- One OAuth session at a time: the Hub guards against multiple plugin
  instances starting multiple OAuth sessions (see `security.md`).

## 7. What Phase 1 deliberately does NOT include

Payments, subscriptions, collaboration, OneDrive, Dropbox, advanced user
accounts, real mobile Drive sync, real Drive provider, and non-FL
connectors. Each is a future phase with its own design note; none is
half-implemented in Phase 1.

## 8. Send / Manager multi-track Push (2026-11-04)

One VST3, one installer, one name in FL's plugin list ("Onorab"). Each
instance chooses a role once, on first open: **Send** (this insert
records its Mixer Track) or **Manager** (this insert lists Sends and
pushes selected tracks to Drive). An old project (state without a mode)
opens as Manager, preserving its previous behaviour. Mode, track name
and instance id persist in the plugin state; a cloned insert (same
saved id, original still alive) regenerates its id, so the Manager
never arms one Send twice.

Coordination is an in-process singleton (`PushSession`), NOT the Hub:
every instance in one FL project shares the DLL's statics, while two
FL projects on one PC are two processes with separate singletons, so
their tracks can never mix. The session tag ("FL-XXXX", random per
process) is shown in both UIs so the user can see which FL session a
Send belongs to; it is not the .flp name (the host never gives it) and
is never sent to Drive or the Hub. This assumes FL's default unbridged
VST3 load; bridged instances (one bridge process each) cannot see
each other. The Hub's job stays what it was: Drive auth + upload.

Push flow: Manager verifies Hub + Drive + selection, arms the Sends
(their writers launch and idle), and shows "Ready. Press Play in FL
Studio." (If FL is already playing, it refuses and asks for Stop +
reposition.) On the Play transition each armed Send records its own
WAV — no silence gating, so leading and middle silence survive and a
late-entering Vocal stays late when both WAVs are re-imported together.
Alignment across Sends comes from the session start sample (first
starter wins a CAS; later starters pad leading silence up to it). On
transport Stop or Finish & upload, every take finalizes at the same
end region (within one audio block) and the Manager uploads only the
selected takes to the chosen project, showing "Uploaded" only on
Hub-confirmed success. A failed upload keeps the local WAV with Retry
upload (no re-performance); a retry cannot duplicate a Drive file,
because Hub Upload updates a same-name file in the folder instead of
creating a second one, while take names stay unique per Push so a new
Push never overwrites the previous take. A Send that disappears
mid-recording yields either a partial take (flagged) or a "Send
disappeared" row — never silent loss.

Audio-thread rules (see section 5) hold for Push too: processBlock
touches only atomics + the lock-free FIFO; the writer thread owns all
file I/O; the Manager arms Sends under the registry lock on the
message thread; the old silence-splitting auto-capture loop is a
separate writer mode and never touches Push takes. A Push survives
Send windows closing (processors live independently of editors) and
Manager window closing (arm state lives in the singleton; a reopened
Manager drains the queued takes and uploads them).

Take preview (added 2026-11-04, user request: "is this the right
audio?"): a Tracks row whose finished take still exists in the local
cache shows a Play button. It plays that exact WAV — the same local
file the Push uploads — through the editor's one shared preview
transport (the Library player's transport), so Library playback and
take preview never overlap: starting one clears the other's id, and
arming a new Push stops an active take preview. The Library bottom
player behaves like the row Play button: a Drive-only selection
downloads first, then plays automatically, instead of dead-ending
with "press Download first".
