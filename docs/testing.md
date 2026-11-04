# Onorab — Testing (Phase 1 MVP)

> IMPORTANT — READ FIRST: the manual FL Studio checklist in §3 is a
> **checklist to run by hand**. It is not a report of passed results.
> Nothing below claims a test has passed unless §2's automated tests
> were actually run and their output is quoted in a release note or
> CI log. Do not mark checklist items "passed" without running them.

## 1. Test strategy

| Layer | Method | Where |
|-------|--------|-------|
| Hub protocol + transfer states | Automated unit tests (`go test`) | `apps/onorab-hub` |
| Protocol schemas | Schema validation (JSON Schema) | `packages/protocol` |
| Plugin <-> Hub integration | Manual, against a running Hub | §3 checklist |
| Mobile UI | Manual, mock mode | §4 checklist |
| Builds | CI workflows (Windows + Android) | `.github/workflows/` |

Rules:

- Do not silently swallow errors in tests either: a test that cannot
  reach the Hub fails loudly instead of skipping.
- A test that needs real Google credentials is out of scope for
  Phase 1. All Phase 1 automated tests run against the Mock Drive
  Provider.
- Never run tests against a real Drive account in CI. CI uses Mock
  mode and no secrets (see `release.md`).

## 2. Automated tests

### 2.1 Hub unit tests (Go)

From the repository root (or from `apps/onorab-hub`, per that
package's own README if it differs — the package README wins on
paths):

```bash
cd apps/onorab-hub
go test ./...
```

What the Hub test suite must cover (Phase 1):

- [ ] Protocol shapes: `/health`, `/files`, `/transfers` responses
      validate against the schemas in `packages/protocol`.
- [ ] Transfer state machine: a transfer moves through its states in
      order (e.g. queued -> uploading/downloading -> completed, or ->
      failed) and never jumps backwards or skips to completed from a
      failed state without an explicit retry.
- [ ] Concurrent transfers: multiple simultaneous uploads/downloads
      update their own progress independently; progress events carry
      the correct transfer/file ID.
- [ ] Mock provider: list returns the seeded mock files; upload stores
      and lists back; download returns the uploaded bytes.
- [ ] Error paths: unknown file ID returns a proper error response
      (not an empty 200); Hub-disconnected behaviour is represented
      in the client, not hidden.
- [ ] Single OAuth session guard (design test in Phase 1; full flow
      test in Phase 2): concurrent authorization requests yield one
      session, not two.
- [ ] Localhost binding: the Hub refuses (or the test asserts) a
      non-loopback bind configuration.

Run with verbose output when investigating a failure:

```bash
go test -v ./...
```

A green `go test ./...` run is required before any Phase 1 release
candidate is cut. The CI `hub-test` job runs exactly this command.

### 2.2 Protocol schema validation

The schemas in `packages/protocol` cover: files, transfers, events,
errors, and connector capabilities.

Validation procedure (manual, until a schema-check script lands):

1. Take a real response body from the running Hub
   (`curl http://127.0.0.1:18741/files`, `/transfers`, and one
   `/events` message).
2. Check every field against the matching schema: required fields
   present, types correct, enum values (transfer states, event kinds,
   error codes) within the allowed set.
3. Check error responses against the error schema: a machine-readable
   error code plus a human-readable message — never a bare HTTP status
   with an empty body.
4. Any mismatch is a bug in either the Hub or the schema. Fix one side
   and re-validate; do not "fix" it by ignoring the schema.

### 2.3 What is NOT automated in Phase 1

- FL Studio plugin behaviour (requires FL Studio + a human ear/eye).
- Real-time audio thread behaviour (FIFO overflow, dropout-free
  capture) — verified manually per §3 and by code review against
  `architecture.md` §5.
- Mobile Share Target on real devices (requires a real phone and
  WhatsApp/Files/Telegram).
- Real Google Drive flows (Phase 2).

## 3. Manual FL Studio checklist (run by hand)

Setup first: Hub running in Mock mode (`docs/setup.md` §4), plugin
installed, FL Studio open. Work through the items in order and record
Pass / Fail + notes for each. Do not skip the error cases at the end.

### A. Load and connect

- [ ] **A1. Plugin loads.** Onorab appears in FL Studio's plugin list
      and loads on a Mixer Track without crashing FL Studio.
- [ ] **A2. Connected state.** With the Hub running, the plugin shows
      **Sync status: Connected**.
- [ ] **A3. Disconnected state.** Quit the Hub while the plugin is
      open. The plugin shows **Disconnected** with a clear error
      message (not a frozen or empty library). Restart the Hub: the
      plugin reconnects (or offers a visible Retry that reconnects).

### B. Cloud Library

- [ ] **B1. Library loads mock files.** The Cloud Library lists the
      seeded mock audio files (names, sizes/durations where shown).
- [ ] **B2. Search.** Typing in Search filters the list to matching
      files; clearing the search restores the full list.
- [ ] **B3. Projects view.** The Projects tab opens and shows the
      mock projects (or an honest empty state if none are seeded —
      record which).
- [ ] **B4. Transfers view.** The Transfers tab opens and shows
      transfer history/status (or an honest empty state before any
      transfer).

### C. Preview (play / pause / stop)

- [ ] **C1. Waveform preview.** Selecting a file shows its waveform.
- [ ] **C2. Play.** Press Play: audio plays through FL Studio and the
      playhead/position advances.
- [ ] **C3. Pause.** Press Pause mid-playback: audio stops and
      position holds; pressing Play again resumes from that position.
- [ ] **C4. Stop.** Press Stop: audio stops and position resets to
      the start.
- [ ] **C5. Preview does not disturb the Mixer Track.** Previewing a
      library file does not inject audio into the Mixer Track's
      processing chain (preview is monitoring only, unless the plugin
      UI explicitly says otherwise — record the actual behaviour).

### D. Capture from FL

- [ ] **D1. Capture button present and enabled** when the connector
      advertises audio capture.
- [ ] **D2. Capture records the Mixer Track.** Play audio through the
      Mixer Track, press **Capture from FL**, let it run a few
      seconds, stop capture. A local WAV file is created.
- [ ] **D3. Capture is clean.** The captured WAV plays back correctly
      (no dropouts, clicks, or silence where audio was playing) and
      its length matches the capture duration.
- [ ] **D4. Capture upload.** After capture, the recording uploads via
      the Hub (Transfers tab shows progress, then completed) and the
      file appears in the Cloud Library.

### E. Upload and download

- [ ] **E1. Upload button.** Pressing **Upload** opens a file picker;
      choosing a local audio file starts an upload with visible
      progress in the Transfers tab.
- [ ] **E2. Download.** Choosing a library file and downloading it
      saves a local copy with visible progress, and the downloaded
      file plays correctly outside the plugin.
- [ ] **E3. Progress accuracy.** Upload/download progress advances
      smoothly to 100% and the transfer state becomes Completed
      (not stuck at 99% or Completed-before-finished).

### F. Drag to FL

- [ ] **F1. Drag to Playlist.** Drag a downloaded audio file from the
      plugin into FL Studio's Playlist: a clip appears and plays.
- [ ] **F2. Drag to Channel Rack / Sampler.** Drag a downloaded file
      onto a Channel (or per FL Studio's drag target): the sample
      loads and plays from the Channel.
- [ ] **F3. Dragged file is the real file.** The audio FL Studio
      receives is the full downloaded file (correct length, not a
      placeholder, preview snippet, or empty file).

### G. Error cases (do not skip)

- [ ] **G1. Hub down during upload.** Stop the Hub mid-upload: the
      transfer fails with a clear error message (not a silent stall),
      and retrying after restarting the Hub works.
- [ ] **G2. Unknown / missing file.** Requesting a file that does not
      exist (e.g. after clearing the mock store) shows a clear error,
      not a crash or empty success.
- [ ] **G3. No audio on the Mixer Track during capture.** Capturing
      silence produces a valid (silent) WAV or a clear warning — not
      a crash and not a corrupt file. Record the actual behaviour.
- [ ] **G4. Project save contains no secrets.** Save the FL Studio
      project, then inspect the saved plugin state/project file for
      any token-like strings (see `security.md` §4). None may be
      present.
- [ ] **G5. Double plugin instance.** Load Onorab on two Mixer Tracks:
      both show the same library state, and no duplicate OAuth-style
      prompt or duplicate Hub session appears (Phase 1: no OAuth flow
      at all; Phase 2: the single-session guard in `security.md` §7).

### H. Sign-off

- [ ] All A–G items recorded Pass, or every Fail has an issue filed
      with steps to reproduce, Hub log excerpt, and FL Studio version.
- [ ] FL Studio version + Windows version + plugin build (CI run
      number / artifact name) recorded with the results.
- [ ] No checklist item marked Pass without actually running it.

### I. Installer + distributed Drive sign-in (real PC only, 2026-11-04)

None of these can be proven by CI or by this VM: they run only on a
real Windows PC with the installer artifact from a GitHub Actions
run. Do NOT mark any of them Pass without running them there.

- [ ] **I1. Clean-account install.** On a Windows account with no
      JSON, no OAuth env vars, and no previous Onorab install, run
      Onorab-Installer.exe: Onorab.vst3 lands in
      `Common Files\VST3`, onorab-hub.exe in `Program Files\Onorab`,
      and FL Studio lists the plugin. (SmartScreen "Run anyway"
      expected until code signing exists.)
- [ ] **I2. One Hub for two instances.** Open FL, load Onorab on two
      Mixer Tracks: the Hub starts once (one onorab-hub.exe in Task
      Manager), both instances show "Hub connected", no console
      window appears.
- [ ] **I3. Distributed sign-in.** Settings → Connect Google Drive
      → browser sign-in with a Test-user Gmail → plugin shows
      "Waiting for Google…" then "Drive connected" + "Connected as
      …", and the browser success page appears only after the
      sign-in truly completed. Library loads real Drive files.
- [ ] **I4. Small WAV to Drive.** Capture or push a small WAV and
      verify the file appears in Drive under
      `Onorab_projet/<project>/`.
- [ ] **I5. Reopen FL.** Close and reopen FL: Drive is still
      connected with no browser prompt (token resumed + refreshed
      from Credential Manager).
- [ ] **I6. Cancel / Try again / Disconnect.** Cancel on Google's
      page → plugin shows the failure + "Try again"; a second
      attempt succeeds; Disconnect removes the sign-in and the
      plugin shows "Not connected", and Connect works again after.
- [ ] **I7. Restart the PC.** After a reboot, opening FL reconnects
      Drive with no sign-in prompt (Hub auto-launched, token
      resumed).

### J. Send / Manager multi-track Push (real PC only, 2026-11-04)

None of these can be proven by CI or by this VM: they need FL Studio
with several live plugin instances. Do NOT mark any of them Pass
without running them there. The acceptance case from the spec is J1;
J2–J6 are its required variations.

- [ ] **J1. One playback, two separate WAVs, still aligned.** In one
      FL project: one Manager + three Sends named Kick, Bass, Vocal.
      Vocal enters later than Kick. Select Kick + Vocal only, press
      Push selected, play the passage once. Drive must hold exactly
      two new WAVs in the chosen project: Kick with Kick only, Vocal
      with Vocal only. Drop both into the Playlist at the same
      position: Vocal still enters late, exactly as in the project.
      Bass must NOT appear on Drive.
- [ ] **J2. Windows closed.** Repeat J1 with all three Send windows
      closed before pressing Play: recording still happens, takes
      still upload, statuses still show in the Manager.
- [ ] **J3. Reopen the .flp.** Save, close, reopen: the Manager is
      still Manager, the Sends keep their names, and a new Push works
      without re-choosing modes. Clone one Send insert: the clone
      appears as its own row with its own name (not a duplicate arm
      of the original).
- [ ] **J4. Failed upload + Retry.** Break the upload (e.g. disconnect
      the network after recording, or revoke Drive mid-upload): the
      failed row shows Retry upload, the local WAV is kept, and Retry
      (network back) succeeds WITHOUT replaying the music and WITHOUT
      a second copy of the file on Drive.
- [ ] **J5. Old project.** Open a pre-Push .flp that already has
      Onorab: the instance opens as Manager with its old settings,
      no mode question, and Library / Projects / Transfers /
      Settings still work.
- [ ] **J6. Post-effect capture.** Put a Send AFTER an EQ + compressor
      on a track: the pushed WAV must contain the processed sound,
      not the dry input.
- [ ] **J7. Loop / seek guard.** Arm a Push with Loop ON in FL (or
      seek the playhead mid-recording): the take STOPS at the jump —
      it must NOT contain the loop repetition or a cut. The row
      shows "Recorded - playback jumped" and the final notice tells
      the user to turn Loop off and record in one continuous pass.
      Re-imported, the take stays aligned to its measures.
- [ ] **J8. Take format setting.** Settings shows "Take format"
      defaulting to 32-bit float; record a Push, confirm the WAV is
      32-bit float (hot signal above 0 dB stays intact, not
      clipped). Switch to 16-bit, record again, confirm the WAV is
      16-bit. Reopen the .flp: the setting persists.

## 4. Manual mobile checklist (mock mode)

- [ ] **M1.** App opens to the Google account connection screen; mock
      sign-in succeeds with no real credentials.
- [ ] **M2.** Cloud Library shows mock audio files; search filters them.
- [ ] **M3.** Audio player: play / pause / stop work on a mock file.
- [ ] **M4.** Upload and download controls show progress against mock
      data (or an honest "mock mode" note where a control is stubbed).
- [ ] **M5.** Projects, Transfers, Profile, and sync-indicator screens
      all open without crashing.
- [ ] **M6.** Share Target: sharing an audio file from Files (or
      WhatsApp/Telegram if available) to Onorab opens the app with the
      file visible in its flow. Record the actual behaviour — real
      Drive upload from mobile is Phase 2.
- [ ] **M7.** No real Google credentials are requested or accepted in
      mock mode.

## 5. Reporting results

When reporting a test run, include:

1. What was run: `go test ./...` output (automated) and/or checklist
   sections A–H / M1–M7 (manual), with the build under test identified
   by CI run + artifact name.
2. Pass/fail per item — never a blanket "all tests passed" without the
   item list behind it.
3. For failures: steps to reproduce, expected vs actual behaviour, and
   the relevant log excerpt (redact secrets first).
4. Known limitations at the time of the run (e.g. "mock provider only;
   real Drive not tested").
