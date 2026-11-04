// OnorabAudioProcessor implementation. See PluginProcessor.h for the
// real-time contract: processBlock does no allocation, locking, I/O or
// networking — only a lock-free FIFO push.

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "HubClient.h"
#include <chrono>

OnorabAudioProcessor::OnorabAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      cacheDir (HubClient::getDefaultCacheDir())
{
}

OnorabAudioProcessor::~OnorabAudioProcessor()
{
    // Never leave a writer thread running past destruction. If capture is
    // still armed, stop it first (joins the writer after it drains).
    if (isCapturing())
        stopCapture();
    else if (captureWriterThread.joinable())
        captureWriterThread.join();
}

void OnorabAudioProcessor::prepareToPlay (double /*sampleRate*/, int samplesPerBlock)
{
    // Pre-allocate the interleave scratch buffer here (message/audio
    // setup thread, NOT inside processBlock). Sized for stereo; if the
    // host later delivers a larger block, processBlock clamps instead of
    // growing this buffer.
    captureScratch.assign (static_cast<size_t> (samplesPerBlock) * 2, 0.0f);
}

void OnorabAudioProcessor::releaseResources()
{
    // Nothing to release: the FIFO/scratch buffers stay allocated so a
    // later prepareToPlay can reuse them without allocating mid-stream.
}

bool OnorabAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& mainOut = layouts.getMainOutputChannelSet();
    if (mainOut != juce::AudioChannelSet::mono() && mainOut != juce::AudioChannelSet::stereo())
        return false;
    if (const auto& mainIn = layouts.getMainInputChannelSet();
        mainIn != juce::AudioChannelSet::mono() && mainIn != juce::AudioChannelSet::stereo())
        return false;
    return true;
}

void OnorabAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                         juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    midiMessages.clear();

    // Pass-through: the buffer is processed in place by the host, so
    // untouched audio flows through the Mixer Track unchanged. (The
    // plugin's job is to observe/copy audio for capture, not to alter it.)
    const int numChannels = buffer.getNumChannels();
    if (numChannels > 0)
        lastNumChannels.store (numChannels);

    if (! capturing.load (std::memory_order_acquire))
        return;

    // ---- Capture path: lock-free FIFO push ONLY. ----
    // No allocation (scratch is pre-allocated), no locks, no I/O, no
    // network, no logging, no string work. Any of those here would risk
    // an audio dropout in FL Studio.
    const int channelsToCapture = juce::jmin (2, numChannels);
    if (channelsToCapture <= 0 || captureScratch.empty())
        return;

    const int numSamples = buffer.getNumSamples();
    const int maxFrames = static_cast<int> (captureScratch.size()) / channelsToCapture;
    const int framesToCapture = juce::jmin (numSamples, maxFrames);

    // Interleave the first 1-2 channels into the scratch buffer.
    for (int frame = 0; frame < framesToCapture; ++frame)
    {
        for (int ch = 0; ch < channelsToCapture; ++ch)
            captureScratch[static_cast<size_t> (frame) * static_cast<size_t> (channelsToCapture)
                           + static_cast<size_t> (ch)]
                = buffer.getSample (ch, frame);
    }

    const int samplesToWrite = framesToCapture * channelsToCapture;
    const int written = captureFifo.write (captureScratch.data(), samplesToWrite);
    if (written < samplesToWrite)
        droppedSamples.fetch_add (samplesToWrite - written);
}

juce::AudioProcessorEditor* OnorabAudioProcessor::createEditor()
{
    return new OnorabAudioProcessorEditor (*this);
}

void OnorabAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Persist ONLY Hub URL + cache dir. Tokens/credentials are never
    // stored here (they live in the Hub / OS keychain), so an FL Studio
    // project file can never leak them.
    juce::ValueTree state ("OnorabState");
    state.setProperty ("hubUrl", hubUrl, nullptr);
    state.setProperty ("cacheDir", cacheDir.getFullPathName(), nullptr);
    juce::MemoryOutputStream stream (destData, false);
    state.writeToStream (stream);
}

void OnorabAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;
    if (auto state = juce::ValueTree::readFromData (data, static_cast<size_t> (sizeInBytes));
        state.isValid() && state.hasType ("OnorabState"))
    {
        if (state.hasProperty ("hubUrl"))
            hubUrl = state.getProperty ("hubUrl").toString();
        if (state.hasProperty ("cacheDir"))
        {
            const juce::String dir = state.getProperty ("cacheDir").toString();
            if (dir.isNotEmpty())
                cacheDir = juce::File (dir);
        }
    }
}

void OnorabAudioProcessor::startCapture (const juce::String& fileName, CaptureFinishedCallback cb)
{
    if (isCapturing())
    {
        // Already capturing: report, don't silently restart and lose audio.
        if (cb)
            cb ({}, "Capture is already running — stop it before starting a new one.");
        return;
    }

    const int channels = juce::jlimit (1, 2, lastNumChannels.load());
    const double sampleRate = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    const juce::String safeName = HubClient::sanitiseFileName (fileName);
    juce::File outFile = cacheDir.getChildFile (safeName);

    droppedSamples.store (0);
    capturedSamples.store (0);
    captureFifo.reset();
    captureFinishedCallback = std::move (cb);
    lastCaptureFile = {};

    // Launch the writer FIRST, then arm the flag, so no audio block can
    // arrive before the writer exists to drain it.
    captureWriterThread = std::thread ([this, outFile, channels, sampleRate]
    {
        captureWriterLoop (outFile, channels, sampleRate);
    });
    capturing.store (true, std::memory_order_release);
}

void OnorabAudioProcessor::stopCapture()
{
    if (! isCapturing() && ! captureWriterThread.joinable())
        return;

    // Disarm first: processBlock stops pushing. The writer then drains
    // whatever is left in the FIFO before closing the WAV file.
    capturing.store (false, std::memory_order_release);
    if (captureWriterThread.joinable())
        captureWriterThread.join();
}

bool OnorabAudioProcessor::startAutoCapture (const juce::String& project)
{
    if (isCapturing())
        return false;

    const int channels = juce::jlimit (1, 2, lastNumChannels.load());
    const double sampleRate = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;

    droppedSamples.store (0);
    capturedSamples.store (0);
    captureFifo.reset();
    // Auto takes use the finished-take queue, never the manual
    // finished callback (which would open the review panel).
    captureFinishedCallback = nullptr;
    lastCaptureFile = {};
    autoMode = true;
    autoProject = project;

    // A previous writer thread object may still be joinable here even
    // though no capture is running (the auto writer exits on its own
    // after a fatal file error). Assigning a new std::thread to a
    // joinable one terminates the process, so join it first: the old
    // thread has already finished (or is just returning), so this
    // join cannot block on live audio work.
    if (captureWriterThread.joinable())
        captureWriterThread.join();

    // Launch the writer FIRST, then arm the flag, so no audio block can
    // arrive before the writer exists to drain it (same order as manual).
    captureWriterThread = std::thread ([this, channels, sampleRate]
    {
        captureWriterLoopAuto (channels, sampleRate);
    });
    capturing.store (true, std::memory_order_release);
    return true;
}

void OnorabAudioProcessor::stopAutoCapture()
{
    if (! autoMode && ! isCapturing())
        return;
    autoMode = false;
    // Shared stop: disarms + joins the writer, which finalizes the
    // open take into the finished queue before it exits.
    stopCapture();
}

std::vector<OnorabAudioProcessor::AutoTake> OnorabAudioProcessor::takeFinishedAutoTakes()
{
    std::lock_guard<std::mutex> lock (autoTakesMutex);
    std::vector<AutoTake> out;
    out.swap (finishedAutoTakes);
    return out;
}

void OnorabAudioProcessor::captureWriterLoopAuto (int numChannels, double sampleRate)
{
    // Writer thread ONLY. See the header for the take-splitting rules.
    // A take is any continuous burst of routed audio; silence between
    // bursts splits takes, and a 10-minute take splits on length.
    constexpr float kSignalThreshold = 0.001f; // ~-60 dBFS peak
    constexpr double kSilenceTimeoutSeconds = 2.0;
    constexpr double kMaxTakeSeconds = 600.0;
    const int64_t silenceTimeoutFrames = static_cast<int64_t> (kSilenceTimeoutSeconds * sampleRate);
    const int64_t maxTakeFrames = static_cast<int64_t> (kMaxTakeSeconds * sampleRate);

    if (! cacheDir.exists() && ! cacheDir.createDirectory())
    {
        DBG ("Onorab auto-capture failed: could not create the cache folder: "
             << cacheDir.getFullPathName());
        capturing.store (false, std::memory_order_release);
        return;
    }

    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    juce::File takeFile;
    int64_t takeFrames = 0;
    int64_t silenceFrames = 0;
    bool takeOpen = false;
    int takeIndex = 0;
    bool fatalError = false;

    auto closeTake = [&]()
    {
        if (! takeOpen)
            return;
        if (writer != nullptr)
        {
            writer->flush();
            writer.reset(); // closes the file
        }
        // Only a file that really exists on disk may be queued: a
        // failed writer must never hand the editor a phantom take.
        if (takeFrames > 0 && takeFile.existsAsFile())
        {
            std::lock_guard<std::mutex> lock (autoTakesMutex);
            finishedAutoTakes.push_back ({ takeFile, autoProject });
        }
        else
        {
            takeFile.deleteFile();
        }
        takeOpen = false;
        takeFrames = 0;
        silenceFrames = 0;
    };

    auto openTake = [&]() -> bool
    {
        // Milliseconds + a per-run index: two takes can never share a
        // name, so a new take can never truncate a queued one.
        const juce::String name = "bus-capture-"
            + juce::String (juce::Time::getCurrentTime().toMilliseconds())
            + "-" + juce::String (++takeIndex) + ".wav";
        takeFile = cacheDir.getChildFile (HubClient::sanitiseFileName (name));
        std::unique_ptr<juce::FileOutputStream> newStream (takeFile.createOutputStream());
        if (newStream == nullptr)
            return false;
        std::unique_ptr<juce::AudioFormatWriter> newWriter (
            wavFormat.createWriterFor (newStream.get(), sampleRate,
                                       static_cast<unsigned int> (numChannels),
                                       16, {}, 0));
        if (newWriter == nullptr)
        {
            // The stream created an empty file; remove it so a failed
            // take never litters the cache with zero-byte files.
            takeFile.deleteFile();
            return false;
        }
        newStream.release(); // owned by the writer now
        writer = std::move (newWriter);
        takeOpen = true;
        takeFrames = 0;
        silenceFrames = 0;
        return true;
    };

    constexpr int kChunkFrames = 4096;
    juce::AudioBuffer<float> chunk (numChannels, kChunkFrames);
    std::vector<float> interleaved (static_cast<size_t> (kChunkFrames)
                                    * static_cast<size_t> (numChannels));

    while (capturing.load (std::memory_order_acquire) || captureFifo.getNumReady() > 0)
    {
        const int readySamples = captureFifo.getNumReady();
        if (readySamples < numChannels)
        {
            // A partial frame can never complete once capture stopped;
            // drop it instead of sleeping forever on a drained FIFO.
            if (! capturing.load (std::memory_order_acquire))
                break;
            // Nothing complete to write yet; poll briefly. (Polling
            // here is fine: this is the writer thread, not the audio
            // thread, and it never blocks the RT side.)
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
            continue;
        }
        const int framesAvailable = readySamples / numChannels;
        const int framesToWrite = juce::jmin (kChunkFrames, framesAvailable);
        const int read = captureFifo.read (interleaved.data(), framesToWrite * numChannels);
        const int framesRead = read / numChannels;
        if (framesRead <= 0)
            continue;

        // Peak of this chunk (writer thread: cheap, no RT impact).
        float peak = 0.0f;
        for (int i = 0; i < framesRead * numChannels; ++i)
        {
            const float s = interleaved[static_cast<size_t> (i)];
            peak = juce::jmax (peak, s < 0.0f ? -s : s);
        }
        const bool signal = peak >= kSignalThreshold;

        if (! takeOpen)
        {
            if (! signal)
                continue; // leading silence: dropped, never an empty take
            if (! openTake())
            {
                DBG ("Onorab auto-capture failed: could not create a take file in: "
                     << cacheDir.getFullPathName());
                fatalError = true;
                break;
            }
        }

        // De-interleave into the JUCE buffer for the writer.
        for (int ch = 0; ch < numChannels; ++ch)
            for (int frame = 0; frame < framesRead; ++frame)
                chunk.setSample (ch, frame,
                                 interleaved[static_cast<size_t> (frame)
                                             * static_cast<size_t> (numChannels)
                                             + static_cast<size_t> (ch)]);

        if (! writer->writeFromAudioSampleBuffer (chunk, 0, framesRead))
        {
            DBG ("Onorab auto-capture failed while writing a take file.");
            fatalError = true;
            break;
        }
        capturedSamples.fetch_add (framesRead);
        takeFrames += framesRead;
        if (signal)
            silenceFrames = 0;
        else
            silenceFrames += framesRead;

        if (takeFrames >= maxTakeFrames)
            closeTake(); // long take splits; the next signal chunk opens a new one
        else if (silenceFrames >= silenceTimeoutFrames)
            closeTake();
    }

    // Finalize any open take on stop (the drain path above already
    // wrote the trailing audio into it).
    closeTake();
    if (fatalError)
        capturing.store (false, std::memory_order_release);
}

void OnorabAudioProcessor::captureWriterLoop (juce::File outFile, int numChannels, double sampleRate)
{
    // Writer thread ONLY. This is the one place captured audio is written.
    juce::String error;
    bool wroteAnything = false;

    if (! cacheDir.exists() && ! cacheDir.createDirectory())
    {
        error = "Could not create the cache folder: " + cacheDir.getFullPathName();
    }
    else
    {
        juce::WavAudioFormat wavFormat;
        // createWriterFor takes ownership of the stream on success.
        std::unique_ptr<juce::FileOutputStream> stream (outFile.createOutputStream());
        if (stream == nullptr)
        {
            error = "Could not create the capture file: " + outFile.getFullPathName();
        }
        else
        {
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wavFormat.createWriterFor (stream.get(), sampleRate,
                                           static_cast<unsigned int> (numChannels),
                                           16, {}, 0));
            if (writer == nullptr)
            {
                // Writer creation failed: it did not take the stream.
                stream.reset();
                error = "Could not create a WAV writer for: " + outFile.getFullPathName();
            }
            else
            {
                stream.release(); // owned by the writer now
                constexpr int kChunkFrames = 4096;
                juce::AudioBuffer<float> chunk (numChannels, kChunkFrames);
                std::vector<float> interleaved (static_cast<size_t> (kChunkFrames)
                                                * static_cast<size_t> (numChannels));

                while (capturing.load (std::memory_order_acquire)
                       || captureFifo.getNumReady() > 0)
                {
                    const int readySamples = captureFifo.getNumReady();
                    if (readySamples < numChannels)
                    {
                        // Nothing complete to write yet; poll briefly.
                        // (Polling here is fine: this is the writer thread,
                        // not the audio thread, and it never blocks the RT side.)
                        std::this_thread::sleep_for (std::chrono::milliseconds (5));
                        continue;
                    }
                    const int framesAvailable = readySamples / numChannels;
                    const int framesToWrite = juce::jmin (kChunkFrames, framesAvailable);
                    const int read = captureFifo.read (interleaved.data(),
                                                       framesToWrite * numChannels);
                    const int framesRead = read / numChannels;
                    if (framesRead <= 0)
                        continue;

                    // De-interleave into the JUCE buffer for the writer.
                    for (int ch = 0; ch < numChannels; ++ch)
                        for (int frame = 0; frame < framesRead; ++frame)
                            chunk.setSample (ch, frame,
                                             interleaved[static_cast<size_t> (frame)
                                                         * static_cast<size_t> (numChannels)
                                                         + static_cast<size_t> (ch)]);

                    if (writer->writeFromAudioSampleBuffer (chunk, 0, framesRead))
                    {
                        capturedSamples.fetch_add (framesRead);
                        wroteAnything = true;
                    }
                    else
                    {
                        error = "Failed while writing the capture file: " + outFile.getFullPathName();
                        break;
                    }
                }

                writer->flush();
                writer.reset(); // closes the file
            }
        }
    }

    if (error.isEmpty() && ! wroteAnything)
        error = "Capture stopped but no audio was received on this Mixer Track.";

    if (error.isEmpty())
        lastCaptureFile = outFile;
    else
        DBG ("Onorab capture failed: " << error);

    // Fire the callback on this (writer) thread; the editor hops to the
    // message thread before touching UI. Keep a local copy: stopCapture
    // may be called re-entrantly from a callback.
    if (auto cb = captureFinishedCallback)
    {
        captureFinishedCallback = nullptr;
        cb (error.isEmpty() ? outFile : juce::File {}, error);
    }
}

// Required by JUCE: factory function that creates an instance of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OnorabAudioProcessor();
}
