# Onorab — Security (Phase 1 MVP)

> Phase 1 ships Mock mode only, so no real Google tokens exist yet.
> These rules are written now so Phase 2 cannot accidentally violate
> them. If any rule here conflicts with an implementation, the rule
> wins and the implementation is fixed.

## 1. OAuth 2.0 with PKCE

- The Hub's future real Drive provider uses **OAuth 2.0 with PKCE**
  (RFC 7636), as required for installed/desktop apps that cannot hold
  a client secret safely.
- Flow (Phase 2, for reference — not implemented in Phase 1):
  1. Hub generates a `code_verifier` and `code_challenge` (S256).
  2. User authorizes in their browser; the authorization server returns
     an authorization code to the loopback redirect.
  3. Hub exchanges the code + `code_verifier` for access and refresh
     tokens over TLS.
  4. Tokens go straight to the OS keychain (§3). They are never written
     to disk in plaintext, never logged, and never sent to the plugin
     except as strictly needed over localhost (and never in a form the
     plugin could persist — see §4).
- PKCE is mandatory, not optional: a plain authorization-code flow
  without PKCE must be rejected in code review.

## 2. Scope: `drive.file` only

- The only Google Drive scope requested is `drive.file`:
  - The app can only see and manage files **it created or the user
    explicitly opened with it**.
  - It cannot list or read the user's entire Drive.
- Requesting broader scopes (`drive`, `drive.readonly`, `drive.metadata`
  etc.) is forbidden. If a future feature appears to need a broader
  scope, it needs a new security review and a user-facing justification
  first — it must not be added silently.
- The mobile app's future Android OAuth client requests the same
  `drive.file` scope, and nothing broader.

## 3. Token storage: OS keychain only

- **Windows Credential Manager** (via the OS-protected credential store)
  is the only approved token store for the Hub on Windows. Equivalent
  OS keychains apply on any future platform port.
- Forbidden token locations, without exception:
  - FL Studio project files (`.flp`, plugin state chunks, presets).
  - The Hub's cache directory (`ONORAB_CACHE_DIR` — audio files only).
  - Any file inside the git repository.
  - Environment files (`.env`), config files, logs, or crash dumps.
  - The plugin's own settings or UI state.
- Tokens are held in memory only for as long as needed to make an
  authorized request, and are never printed to logs, even at debug
  level. Log lines must redact anything shaped like a token.
- Token refresh happens in the Hub (background thread), never in the
  plugin and never on the audio thread.

## 4. Never in an FL Studio project

- The plugin must never serialize access tokens, refresh tokens, OAuth
  codes, verifiers, or client secrets into its state. When FL Studio
  saves a project, the plugin state must contain only UI/project data
  (selected files, project references by file ID, settings) — no
  credentials.
- Rationale: project files are shared, backed up, and sent to
  collaborators. A token inside a project file is a leaked token.
- Test implication: saving and reloading a project in FL Studio must
  never require re-entering credentials, and inspecting the saved
  project must never reveal a token. (See `testing.md` for the manual
  check.)

## 5. Client credentials: never compiled in, never committed

- The Desktop OAuth client JSON (`credentials.json`) is loaded at
  runtime from `ONORAB_CREDENTIALS_PATH` or the equivalent environment
  configuration. It is **never compiled into** the VST3 or the Hub
  executable (no embedded JSON, no baked-in client ID/secret in source).
- The Android app must use **its own, separate Android OAuth client**
  (Phase 2). The Desktop OAuth client must never be reused inside the
  Android app: different client type, different redirect/verification
  model, and embedding a desktop credential in a distributed APK would
  expose it to anyone who downloads the app.
- `.gitignore` rules (must be present at the repo root; verify before
  any commit that touches auth code):

```gitignore
# Onorab secrets — never commit any of these
credentials.json
**/credentials.json
.env
config.json
*.pem
*.key
token*.json
*token*.json
```

- Also forbidden to commit, even in examples or tests: real client
  secrets, real access tokens, real refresh tokens, real authorization
  codes, and real `code_verifier` values. Test fixtures must use
  obviously fake values (e.g. `test-client-id`, `mock-token-...`).
- If a secret is ever committed by accident: revoke/rotate it
  immediately, purge it from git history, and treat it as compromised
  until rotated — do not just delete the file in a later commit.

## 6. `.env.example` and `config.example.json`

- Both templates ship with placeholder values only (e.g.
  `ONORAB_HUB_PORT=18741`, `ONORAB_DRIVE_PROVIDER=mock`,
  `ONORAB_CREDENTIALS_PATH=/path/to/credentials.json`).
- Neither template may contain a real client ID, secret, token, or
  real filesystem path belonging to a developer. Paths in examples
  must be generic (`/path/to/...`, `C:\path\to\...`).
- Real local settings live in the git-ignored `.env` / `config.json`
  copies. Even there, no tokens and no client secrets: credentials
  come from the keychain + the credentials file path, not from env
  files.

## 7. Single OAuth session guard

- Problem: if two plugin instances (two Mixer Tracks, or two FL Studio
  projects) each start their own OAuth flow, the user faces duplicate
  browser prompts, token races, and potentially two live token sets.
- Rule: the **Hub owns exactly one OAuth session**. The plugin never
  starts an OAuth flow itself; it asks the Hub, and the Hub serializes
  authorization:
  - If an authorization is already in progress, a second request
    returns the in-progress state (or a clear "authorization already
    in progress" status) instead of opening a second browser flow.
  - If a valid token already exists in the keychain, no new flow
    starts at all.
  - The guard is enforced in the Hub with a mutex around the
    authorization state machine, and it is covered by a unit test
    (concurrent authorization attempts must yield exactly one flow).
- In Phase 1 (Mock mode) there is no OAuth flow, but the guard's
  structure (single session owner = Hub) is already the design; the
  mock provider must not invent a second session concept.

## 8. Localhost binding only

- The Hub binds to `127.0.0.1` only. Binding `0.0.0.0` or any
  non-loopback interface is a security defect, not a configuration
  option.
- The plugin connects only to loopback URLs. `ONORAB_HUB_URL` values
  with a non-loopback host must be rejected with a clear error.
- Consequences: no remote attacker can reach the Hub over the network;
  the attack surface is local processes on the same machine and the
  browser-based OAuth redirect (loopback), both of which are covered
  by the rules above.
- The WebSocket `/events` endpoint inherits the same binding. It
  carries transfer metadata, not tokens.

## 9. Cache handling

- `ONORAB_CACHE_DIR` holds downloaded/cached audio files and transfer
  working files only.
- The cache must never contain tokens, credentials, or OAuth material
  (§3). Cache files are audio data + metadata (file ID, name, size,
  transfer state) — nothing secret.
- Cached audio is the user's own audio; still, the cache directory
  should live under the user's profile (default `%LOCALAPPDATA%`), not
  in a shared or world-readable location, and cache cleanup must not
  delete files that are mid-transfer.
- On Hub exit, in-flight transfers are marked failed/interrupted in
  transfer state (not silently dropped); on restart, the user can retry
  them explicitly.

## 10. Error handling and disclosure

- Errors are never silently swallowed: every failed upload, download,
  authorization, or transfer produces a user-visible error message
  with an actionable next step (see `testing.md` error cases).
- Error messages must not include tokens, credentials, authorization
  codes, or full filesystem paths beyond what the user needs. When in
  doubt, show the failure kind ("Upload failed: Hub disconnected")
  and put detail in a local log file the user can choose to share
  after redacting secrets.
- Security-sensitive failures (auth denied, token refresh failure,
  keychain unavailable) must surface as distinct errors, not as a
  generic "sync failed".

## 11. Phase 1 vs Phase 2 security checklist

| Control | Phase 1 (now) | Phase 2 (future) |
|---------|---------------|------------------|
| Drive provider | Mock only | Real Drive, PKCE, `drive.file` |
| Tokens exist? | No | Yes, keychain only |
| OAuth session guard | Design + Hub ownership | Implemented + unit-tested |
| Desktop credentials file | Not used | Runtime path only, never compiled/committed |
| Android OAuth client | Not configured (mock mode) | Separate Android client, never the desktop one |
| Localhost binding | Required | Required |
| `.gitignore` secrets | Required | Required |
