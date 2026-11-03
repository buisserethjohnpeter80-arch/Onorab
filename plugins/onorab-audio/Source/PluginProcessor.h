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
// No tokens, secrets or credentials are stored here — and deliberately
// never in the plugin state saved into an FL Studio project (see
// getStateInformation: only Hub URL + cache dir are persisted).

#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <thread>
#include "CaptureFifo.h"

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

private:
    // Writer-thread loop. Opens the WAV writer, drains the FIFO until
    // capture stops AND the FIFO is empty, then closes the file and
    // fires the finished callback. This is the ONLY function that writes
    // captured audio to disk.
    void captureWriterLoop (juce::File outFile, int numChannels, double sampleRate);

    juce::String hubUrl = "http://127.0.0.1:18741";
    juce::File cacheDir;

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OnorabAudioProcessor)
};
