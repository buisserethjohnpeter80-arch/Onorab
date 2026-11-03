# Onorab — Release (Phase 1 MVP)

> Phase 1 releases are test builds, not production releases. The
> Windows ZIP contains a VST3 + Hub built by CI; the Android APK is an
> **unsigned debug** build. Neither is signed for distribution, and
> neither talks to real Google Drive yet.

## 1. Versioning

- Scheme: **Semantic Versioning** — `MAJOR.MINOR.PATCH`
  (e.g. `0.1.0` for the first Phase 1 release candidate).
- Phase 1 stays in `0.x`: the API, protocol schemas, and plugin UI are
  explicitly unstable and may change between releases.
- `MAJOR` stays `0` until the first real-Drive release is stable and
  tested end-to-end (phone <-> Drive <-> FL Studio). Only then is
  `1.0.0` considered.
- `MINOR` increments for each new vertical slice or connector
  (e.g. real Drive provider, mobile Drive sync, a new connector).
- `PATCH` increments for bug fixes with no behaviour change.
- Every release is tagged in git: `v0.1.0`, `v0.2.0`, etc. The tag
  must point at the exact commit the CI artifacts were built from.
- Pre-release tags use suffixes: `v0.1.0-rc.1`, `v0.1.0-beta.1`.
  A `-rc` build is a release candidate awaiting the manual checklist
  in `testing.md` §3; it becomes a release only after that checklist
  is signed off.

## 2. CI artifacts (what each workflow produces)

### 2.1 Windows — `onorab-windows` (ZIP)

Produced by `.github/workflows/windows.yml`, job `package`.

Contents:

```text
onorab-windows.zip
├── Onorab.vst3/        # VST3 plugin directory (Release build)
│   └── Contents/ ...   # (exact inner layout per JUCE/VST3 packaging)
└── onorab-hub.exe      # Hub, single Windows executable (Go)
```

- The ZIP is the **only** supported distribution for Phase 1 Windows
  testing. Do not distribute loose `.vst3` folders or Hub binaries
  built on a developer machine as if they were releases.
- Install: unzip, copy `Onorab.vst3` to the VST3 folder, run
  `onorab-hub.exe` — full steps in `setup.md` §4–§5.
- The Windows workflow needs **no secrets** for a Mock build. If a
  workflow run asks for a secret, something is misconfigured — stop
  and fix the workflow instead of adding a secret.

### 2.2 Android — `onorab-android-debug` (unsigned APK)

Produced by `.github/workflows/android.yml`.

Contents:

```text
onorab-android-debug/
└── app-debug.apk       # Unsigned debug APK (testing only)
```

- Unsigned: install requires allowing unknown sources / a debug
  install, and the APK must never be presented as a store release.
- No signing secrets are used or required. Adding signing is a
  future release-hardening task, not a Phase 1 task.
- The APK runs in mock mode (see `setup.md` §6). It does not perform
  real Drive sync.

### 2.3 Artifact retention

- GitHub artifact retention defaults apply unless the workflow sets
  otherwise. If an artifact has expired, re-run the workflow from the
  tagged commit — do not rebuild locally and re-upload by hand.
- Each release note (§4) names the workflow run / artifact it refers
  to, so an expired artifact can be traced back to its commit.

## 3. Cutting a release (checklist)

1. [ ] All `go test ./...` Hub tests pass in CI (`hub-test` job).
2. [ ] Windows workflow completes: `plugin-build` + `package` green,
      `onorab-windows.zip` downloadable.
3. [ ] Android workflow completes: `app-debug.apk` downloadable.
4. [ ] Tag the commit: `git tag vX.Y.Z` (or `vX.Y.Z-rc.N`).
5. [ ] Write release notes from the template in §4 — fill every
      section honestly; leave "Not tested" where it was not tested.
6. [ ] Run the manual FL Studio checklist (`testing.md` §3) against
      the release ZIP before calling it a release (an `-rc` may be
      published before this step; a final release may not).
7. [ ] Run the manual mobile checklist (`testing.md` §4) against the
      release APK.
8. [ ] Confirm no secrets in the release: no `credentials.json`, no
      `.env`, no tokens in the ZIP, APK, notes, or tag message
      (see `security.md` §5).

## 4. Release notes template

Copy this into every GitHub Release. Delete nothing — write "None" or
"Not tested" rather than dropping a section.

```markdown
## Onorab vX.Y.Z — [Phase 1 test build / release candidate / release]

**Build:** workflow run <link>, commit <sha>, tag <vX.Y.Z>
**Artifacts:**
- Windows: `onorab-windows.zip` (Onorab.vst3 + onorab-hub.exe)
- Android: `app-debug.apk` (unsigned debug build, mock mode)

### What is included
- ...

### What is NOT included (Phase 1 limits)
- No real Google Drive sync (Mock Drive Provider / mock mobile mode only)
- No payments, subscriptions, collaboration, OneDrive, Dropbox
- Android APK is unsigned and for testing only

### Tested
- Hub unit tests: `go test ./...` — [pass / fail + link to CI log]
- Manual FL Studio checklist (docs/testing.md §3): [signed off / in
  progress / not run — list any failed items]
- Manual mobile checklist (docs/testing.md §4): [signed off / in
  progress / not run — list any failed items]

### Known issues
- ...

### Installation
See docs/setup.md §2, §4–§6.

### Upgrade notes
- ...
```

## 5. What a release never contains

- `credentials.json` or any OAuth client JSON (real or copied).
- `.env`, `config.json`, or any file with real paths, tokens, or
  secrets.
- Signing keys, keystores, or keystore passwords (Android signing is
  future work and belongs in GitHub Secrets, never in an artifact).
- A claim that real Drive sync, a connector, or a checklist item
  works when it has not been built or run.
