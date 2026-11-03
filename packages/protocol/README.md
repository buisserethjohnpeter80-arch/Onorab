# Onorab protocol (Phase 1)

Shared JSON shapes used by the Hub, the VST3 plugin, and the mobile app.
Field names are the contract — do not rename a field on one side only.

| Schema | Used by |
| --- | --- |
| `file.json` | `GET /files`, `file.added` events, mobile `OnorabFile` |
| `transfer.json` | `GET /transfers`, transfer events, mobile `Transfer` |
| `event.json` | WebSocket `/events` |
| `error.json` | Every Hub error response: `{"error": {"code": "...", "message": "..."}}` |
| `capability.json` | Future connectors (FL Studio, Logic Pro, Blender, Photoshop, Premiere Pro, Maya). Declaration only in Phase 1. |

## Hub wire shapes (Phase 1, implemented in `apps/onorab-hub`)

- `GET /health` → `{"ok": true, "status": "ok", "provider": "mock", "version": "0.1.0"}`
- `GET /files` → bare JSON array of `OnorabFile`
- `GET /transfers` → bare JSON array of `OnorabTransfer`
- `POST /uploads` → `multipart/form-data`, single field named `file`; response is the created `OnorabFile`
- `GET /downloads/{fileId}` → raw audio bytes
- WebSocket `/events` → JSON `OnorabEvent` text frames

Transfer status vocabulary is `queued | uploading | downloading | completed | failed`
and `progress` is 0–100. The VST3 plugin displays the status string as received
and normalises progress > 1 to a 0–1 fraction; the mobile app uses 0–100 directly.

## Validation

The schemas are JSON Schema draft 2020-12. A quick check that they parse and
that a sample file/transfer conforms is in the Hub test suite
(`apps/onorab-hub/protocol_test.go`, which embeds the field rules) and can also
be done with any JSON Schema validator, e.g. Python `jsonschema` if installed.
No schema in this folder contains a secret, token, or credential.
