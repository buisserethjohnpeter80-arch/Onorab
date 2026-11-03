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
