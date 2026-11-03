# Onorab Hub

Lightweight Windows background service (single executable, Go standard
library only — no third-party dependencies).

## Run (Phase 1, Mock Drive)

```powershell
cd apps/onorab-hub
go run .
# or, after building:
.\onorab-hub.exe
```

The Hub binds **127.0.0.1 only** (localhost), default port **18741**.

```powershell
curl http://127.0.0.1:18741/health
curl http://127.0.0.1:18741/files
curl http://127.0.0.1:18741/transfers
```

Upload (multipart field must be named `file`):

```powershell
curl -F "file=@voice.wav" http://127.0.0.1:18741/uploads
curl http://127.0.0.1:18741/downloads/mock_voice_memo -o voice-memo.wav
```

## Environment

| Variable | Default | Meaning |
| --- | --- | --- |
| `ONORAB_HUB_PORT` | `18741` | Local port |
| `ONORAB_DRIVE_PROVIDER` | `mock` | `mock` or `drive` (real Google Drive, drive.file only). Unknown values fail fast instead of silently falling back |
| `ONORAB_CACHE_DIR` | OS cache dir + `Onorab/cache` | Local audio cache (audio bytes only, never tokens) |
| `ONORAB_CREDENTIALS_PATH` | (unset) | Dev only: path to a Desktop OAuth JSON. Never compiled in, never committed |
| `ONORAB_OAUTH_CLIENT_ID` | (unset) | Dev/manual runs: the Onorab app OAuth client id. Release builds inject it via `-X main.buildOAuthClientID` from a CI secret instead |

## Endpoints

| Method | Path | Notes |
| --- | --- | --- |
| GET | `/health` | `{"ok": true, "status": "ok", "provider": "mock", "version": "0.1.0"}` |
| GET | `/files` | Bare array of `OnorabFile` in **creation order** (see `packages/protocol/file.json`); never alphabetical |
| POST | `/uploads` | `multipart/form-data`, field `file` plus optional text field `project` (project name, stored as `projectId`; maps to Drive folder `Onorab_projet/<project>/` in Phase 2); returns the created file, HTTP 201 |
| DELETE | `/files/{fileId}` | Deletes the file + its cache bytes; 404 for unknown ids |
| POST | `/files/{fileId}/delete` | Same delete, for the JUCE plugin (no proven DELETE verb in JUCE URL) |
| GET/POST | `/invites` | Project invites (`packages/protocol/invite.json`); POST body `{"email","project"}`, HTTP 201; Mock records only, Drive mode creates a real Drive writer permission |
| POST | `/auth/start` | Starts Google sign-in; returns `{"status":"awaiting_user","auth_url":"..."}` for the plugin to open in the browser (Mock mode: 501 `oauth_unavailable`) |
| GET | `/auth/status` | `{"provider","authenticated","account","oauthInProgress"}`; authenticated is true after sign-in or automatic refresh-token reconnect |
| GET | `/downloads/{fileId}` | Raw audio bytes |
| GET | `/transfers` | Bare array of `OnorabTransfer`, progress 0–100 |
| WebSocket | `/events` | JSON `OnorabEvent` text frames (`hub.connected`, `transfer.*`, `file.added`, `auth.connected`) |

## Tests

```powershell
go test ./...
```

## Commercial Drive mode (see docs/architecture.md §3.4)

- `ONORAB_DRIVE_PROVIDER=drive` uses real Google Drive with
  `drive.file` scope only, under the folder `Onorab_projet/`.
- Every PC signs in with its owner's Gmail (Settings → Connect Drive
  in the plugin, or POST `/auth/start` then open the returned
  `auth_url`). The refresh token is stored in Windows Credential
  Manager on that PC only; after the first sign-in, reconnect is
  automatic via the stored refresh token.
- The repo contains NO OAuth client id or secret. Release builds
  inject the client id from a CI secret (`-X
  main.buildOAuthClientID`); dev runs use `ONORAB_OAUTH_CLIENT_ID`
  or `ONORAB_CREDENTIALS_PATH`. Never commit a credentials JSON,
  never send one in chat.
- While the Google app is in Testing mode: only Test users (max 100)
  can sign in, the unverified-app screen appears, and refresh tokens
  expire after 7 days. Production + Google verification removes
  those limits — required before selling.
- The Android app must use its own Android OAuth client; the Desktop
  OAuth client is never reused there.
