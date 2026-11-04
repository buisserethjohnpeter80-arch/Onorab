#pragma once

// OnorabAudioProcessor — the VST3 audio processor.
//
// Audio-path contract (read this before touching processBlock):
//   * processBlock NEVER allocates, NEVER locks, NEVER does file I/O and
//     NEVER touches the network. It only (a) lets audio pass through
//     untouched and (b) — while capturing — interleaves the incoming
//     samples into a pre-allocated scratch buffer and pushes them into
//     the lock-free CaptureFifo.
//   * All file writing happens on captureWriterThread, a plain background
//     thread that drains the FIFO and writes a WAV file. That thread is
//     the only place a WAV writer / FileOutputStream is touched.
//   * Settings (Hub URL, cache dir) and capture state live here so the
//     editor can read them; they are message-thread data except for the
//     explicitly atomic capture flags, which processBlock may read.
//
// Send / Manager modes (2026-11-04): one VST3, two roles, chosen once
// when the instance first opens. A Send registers with the in-process
// PushSession (see PushSession.h) and records its own Mixer Track when a
// Manager arms a Push; a Manager never records Push audio itself. Mode,
// track name and instance id persist in the plugin state, so reopening
// an .flp restores them. An old project (state without a mode) opens as
// Manager, preserving its previous behaviour.
//
// Push recording rules (Send mode only):
//   * The Manager arms Sends on the message thread (preparePushRecording
//     launches the writer, which idles on an empty FIFO). The audio
//     thread only flips flags: when the transport starts playing it
//     begins FIFO pushes; when the transport stops — or the Manager
//     requests Finish — it stops pushing and the writer drains, closes
//     the file and queues exactly one take (even an empty one, so the
//     Manager never waits forever on a prepared Send).
//   * Push takes are NEVER silence-gated (unlike auto-capture takes):
//     silence at the start and in the middle is real timing and must
//     survive, so a Vocal that enters 8 bars after the Kick still
//     enters 8 bars after it when both WAVs are re-imported together.
//     Alignment across Sends comes from the session start sample: the
//     first Send to start wins a CAS on PushSession's pushStartSample
//     and later starters pad leading silence up to it.
//   * Manual capture, auto-capture and Push recording share the one
//     writer thread and can never run together (guards in the start
//     functions). The writer thread is joined on the message thread
//     only — never from processBlock.
//
// No tokens, secrets or credentials are stored here — and deliberately
// never in the plugin state saved into an FL Studio project (see
// getStateInformation: only Hub URL, cache dir, mode, track name and
// instance id are persisted).

#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include "CaptureFifo.h"
#include "PushSession.h"

class OnorabAudioProcessor : public juce::AudioProcessor
{
public:
    // Called on the writer thread when a capture finishes (or fails).
    // `error` is empty on success. The callback must NOT touch UI
    // directly — hop to the message thread first (the editor does this).
    using CaptureFinishedCallback = std::function<void (juce::File file, juce::String error)>;

    OnorabAudioProcessor();
    ~OnorabAudioProcessor() override;

    // AudioProcessor boilerplate (JUCE 8).
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Onorab"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- Settings (message thread) ----
    juce::String getHubUrl() const { return hubUrl; }
    void setHubUrl (const juce::String& url) { hubUrl = url; }
    juce::File getCacheDir() const { return cacheDir; }
    void setCacheDir (const juce::File& dir) { cacheDir = dir; }

    // ---- Send / Manager mode (message thread) ----
    enum class PluginMode { Undecided, Manager, Send };
    PluginMode getMode() const { return mode; }
    // Registers (Send) or unregisters (leaving Send) with PushSession.
    // Refused while a Push writer is prepared or recording: the editor
    // never offers a mode switch in that state, and this guard keeps a
    // stray call from orphaning a live recording.
    void setMode (PluginMode newMode);
    juce::String getTrackName() const { return trackName; }
    void setTrackName (const juce::String& name);
    juce::String getInstanceId() const { return instanceId; }
    juce::String getSessionTag() const { return PushSession::getInstance().getSessionTag(); }

    // Transport playhead cache, updated in processBlock (all modes).
    // The Manager reads this for its "FL Studio is already playing"
    // guard; it is a cache, not a promise — if the host suspends
    // processBlock while stopped, it can go stale until the next block.
    bool isTransportPlaying() const noexcept { return transportPlaying.load(); }

    // Level meter (Send editor only). Disabled by default so a closed
    // Send window costs no per-block scan; the editor enables it while
    // open and the value decays on the message thread.
    void setMeterEnabled (bool enabled) noexcept { meterEnabled.store (enabled); }
    float getOutputPeak() const noexcept { return outputPeak.load(); }

    // ---- Capture from FL (message thread to start/stop) ----
    // fileName is the name under the cache dir, e.g. "capture-2026-11-03-1.wav".
    // The writer thread creates the file; startCapture only arms the flag
    // and launches the writer. Safe to call when already capturing: it is
    // ignored and the error is reported through the callback, not swallowed.
    void startCapture (const juce::String& fileName, CaptureFinishedCallback cb);
    void stopCapture();
    bool isCapturing() const noexcept { return capturing.load(); }
    int getDroppedSamples() const noexcept { return droppedSamples.load(); }
    int64_t getCapturedSamples() const noexcept { return capturedSamples.load(); }
    juce::File getLastCaptureFile() const { return lastCaptureFile; }

    // ---- Auto-capture from routed Mixer audio (message thread) ----
    // One finished take per continuous burst of audio on this plugin's
    // Mixer Track: the user routes Mixer Tracks to this track ("Route
    // to this track"), turns auto-capture on for a project, and plays.
    // The writer thread (never the audio thread) splits the stream on
    // silence and queues finished takes; the editor uploads each take
    // to the project. Manual capture and auto-capture share the one
    // capture writer, so they can never run at the same time: starting
    // either while the other runs fails instead of corrupting audio.
    // A finished take is a real WAV file plus the project it belongs
    // to; the editor drains the queue on its timer.
    struct AutoTake { juce::File file; juce::String project; };
    // False when any capture (manual or auto) is already running.
    bool startAutoCapture (const juce::String& project);
    void stopAutoCapture();
    // Drains the finished-take queue (message thread only).
    std::vector<AutoTake> takeFinishedAutoTakes();

    // ---- Push recording (Send mode; see the header contract) ----
    // Called by PushSession on the message thread (registry lock held)
    // when the Manager arms this Send. "" = prepared, else the error.
    juce::String preparePushRecording (int pushId);
    // Called by PushSession on Cancel (message thread). No-op while
    // recording: a recording Send can only be ended by transport Stop
    // or Finish & upload.
    void cancelPushPreparation();
    bool isPushPrepared() const noexcept { return pushPrepared.load(); }
    bool isPushRecording() const noexcept { return pushRecording.load(); }
    // True after a Push take finalized (until the next prepare). The
    // Send editor uses it for its Finished status line.
    bool hasFinishedPush() const noexcept { return pushFinished.load(); }

private:
    // Writer-thread loop. Opens the WAV writer, drains the FIFO until
    // capture stops AND the FIFO is empty, then closes the file and
    // fires the finished callback. This is the ONLY function that writes
    // captured audio to disk.
    void captureWriterLoop (juce::File outFile, int numChannels, double sampleRate);
    // Auto writer-thread loop. Same FIFO discipline as the manual loop,
    // but it manages take files itself: leading silence is dropped (no
    // empty takes), pauses inside a take are written (real timing is
    // kept), and a take closes after 2 seconds of continuous silence
    // or after 10 minutes (a long take splits). Finished takes go to
    // finishedAutoTakes; the manual finished callback is never fired.
    void captureWriterLoopAuto (int numChannels, double sampleRate);
    // Push writer-thread loop. Same FIFO discipline, but the file is
    // created LAZILY on the first data (a prepared-but-never-started
    // Send leaves no file), NOTHING is silence-gated, and leading
    // silence is written first to align this take with the session
    // start sample. Exactly one take is queued on exit — a real file,
    // an empty take (no audio), or an error take — except on Cancel,
    // which queues nothing. The loop also watches the session's
    // finishRequested flag so a prepared-but-never-started Send ends
    // when the Manager finishes the Push.
    void captureWriterLoopPush (int numChannels, double sampleRate, juce::File outFile, int pushId);
    // Audio-thread transitions (atomics + FIFO flag only).
    void startPushRecordingFromAudioThread();
    void finishPushRecordingFromAudioThread (int reasonCode); // 1 = transport stop, 2 = Finish & upload
    // Message thread / destructor: ends a prepared or recording Push
    // writer with reason "destroyed" (partial take) or "cancelled"
    // (prepared, never started: no take) and joins the thread.
    void cleanupPushWriter();
    juce::String generateInstanceId();

    juce::String hubUrl = "http://127.0.0.1:18741";
    juce::File cacheDir;

    // Mode / identity (message thread; setMode / setStateInformation).
    PluginMode mode = PluginMode::Undecided;
    juce::String trackName;
    juce::String instanceId;
    // Shared with PushSession at registration; the entry object outlives
    // unregistration, so audio/writer-thread status writes through it
    // can never dangle.
    std::shared_ptr<PushSendEntry> pushEntry;

    // Playhead cache (audio thread writes, message thread reads).
    std::atomic<bool> transportPlaying { false };
    std::atomic<bool> transportTimeKnown { false };
    std::atomic<int64_t> transportTimeSamples { 0 };
    bool wasTransportPlaying = false; // audio thread only

    // Meter (audio thread writes when enabled, Send editor reads).
    std::atomic<bool> meterEnabled { false };
    std::atomic<float> outputPeak { 0.0f };

    // Capture state. `capturing` is the only flag processBlock reads.
    std::atomic<bool> capturing { false };
    std::atomic<int> droppedSamples { 0 };
    std::atomic<int64_t> capturedSamples { 0 };
    // Last channel count seen in processBlock; used to fix the WAV
    // channel count when capture starts (message thread reads it).
    std::atomic<int> lastNumChannels { 2 };

    CaptureFifo captureFifo;
    // Pre-allocated ONLY in prepareToPlay / constructor; processBlock
    // only reads/writes it, never resizes it (see the clamp in processBlock).
    std::vector<float> captureScratch;

    std::thread captureWriterThread;
    CaptureFinishedCallback captureFinishedCallback;
    juce::File lastCaptureFile;

    // Auto-capture state. autoMode/autoProject are written on the
    // message thread before the writer thread launches and read by
    // the writer thread only while it runs (stop joins it first), so
    // no lock is needed for them. The finished-take queue IS shared
    // between the writer thread (push) and the message thread (drain),
    // so it has its own mutex. The queue is deliberately NOT cleared
    // when a new auto session starts: a take recorded before the
    // editor closed must never be silently dropped.
    bool autoMode = false;
    juce::String autoProject;
    std::mutex autoTakesMutex;
    std::vector<AutoTake> finishedAutoTakes;

    // Push recording state. pushIdCurrent / pushTakeName /
    // pushDroppedBaseline are written on the message thread at prepare
    // and read by the writer thread only while it runs (same discipline
    // as autoMode/autoProject). All other fields are atomics so the
    // audio thread, writer thread and message thread can share them.
    std::atomic<bool> pushPrepared { false };
    std::atomic<bool> pushRecording { false };
    std::atomic<bool> pushFinished { false };
    std::atomic<bool> pushWriterDone { false };
    std::atomic<int64_t> pushLeadSilenceFrames { 0 };
    // 0 none, 1 transport stop, 2 Finish & upload, 3 destroyed,
    // 4 cancelled. Set before the done flag (release) so the writer
    // always sees the reason when it exits.
    std::atomic<int> pushFinishReason { 0 };
    int pushIdCurrent = 0;
    juce::String pushTakeName;
    int pushDroppedBaseline = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OnorabAudioProcessor)
};
