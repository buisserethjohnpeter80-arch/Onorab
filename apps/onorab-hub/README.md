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
| `ONORAB_DRIVE_PROVIDER` | `mock` | Phase 1: `mock` only. Any other value fails fast at startup instead of silently falling back |
| `ONORAB_CACHE_DIR` | OS cache dir + `Onorab/cache` | Local audio cache (audio bytes only, never tokens) |
| `ONORAB_CREDENTIALS_PATH` | (unset) | Phase 2 only: path to the Desktop OAuth JSON. Never compiled in, never committed |

## Endpoints

| Method | Path | Notes |
| --- | --- | --- |
| GET | `/health` | `{"ok": true, "status": "ok", "provider": "mock", "version": "0.1.0"}` |
| GET | `/files` | Bare array of `OnorabFile` (see `packages/protocol/file.json`) |
| POST | `/uploads` | `multipart/form-data`, field `file`; returns the created file, HTTP 201 |
| GET | `/downloads/{fileId}` | Raw audio bytes |
| GET | `/transfers` | Bare array of `OnorabTransfer`, progress 0–100 |
| WebSocket | `/events` | JSON `OnorabEvent` text frames (`hub.connected`, `transfer.*`, `file.added`) |
| GET | `/auth/status` | Provider + `oauthInProgress`; Phase 1 always `authenticated: false` |
| POST | `/auth/start` | Phase 1: HTTP 501 `oauth_unavailable` (clear, not silent) |

## Tests

```powershell
go test ./...
```

## What Phase 1 does NOT do

- No real Google Drive sync, no real OAuth session, no token storage.
  `MemoryTokenStore` is test-only; the shipped Windows build must use
  Windows Credential Manager in Phase 2 (interface: `TokenStore`).
- The Android app must use its own Android OAuth client in Phase 2;
  the Desktop OAuth client is never reused there.
