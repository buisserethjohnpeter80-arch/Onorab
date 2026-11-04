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
    // A Push writer must be ended explicitly: its loop watches
    // pushWriterDone, not `capturing`, so the generic stop below would
    // join a thread that never exits. cleanupPushWriter ends it with
    // reason "destroyed" (partial take) or "cancelled" (never started)
    // and joins it before the FIFO dies.
    cleanupPushWriter();
    if (mode == PluginMode::Send)
    {
        PushSession::getInstance().unregisterSend (instanceId);
        pushEntry.reset();
    }
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

    // ---- Playhead cache (all modes, every block). ----
    // Reading the host playhead from processBlock is standard JUCE
    // practice: it is a lock-free read of host-provided data, no
    // allocation, no I/O. The Manager uses the cache for its "FL Studio
    // is already playing" guard; Sends use it to start on Play and
    // finalize on Stop. When the host gives no position, the flags stay
    // "unknown" and Push falls back to arm-driven start (first block
    // after arming) and Finish-driven stop.
    bool playingNow = false;
    bool timeKnownNow = false;
    int64_t timeSamplesNow = -1;
    if (auto* playHead = getPlayHead(); playHead != nullptr)
    {
        if (auto position = playHead->getPosition(); position.hasValue())
        {
            playingNow = position->getIsPlaying();
            if (auto t = position->getTimeInSamples(); t.hasValue())
            {
                timeKnownNow = true;
                timeSamplesNow = *t;
            }
        }
    }
    transportPlaying.store (playingNow, std::memory_order_relaxed);
    transportTimeKnown.store (timeKnownNow, std::memory_order_relaxed);
    if (timeKnownNow)
        transportTimeSamples.store (timeSamplesNow, std::memory_order_relaxed);

    // ---- Send level meter (only while a Send editor is open). ----
    // A full peak scan every block is wasted CPU when nobody watches,
    // so it is gated on meterEnabled. The scan itself is allocation-free.
    if (mode == PluginMode::Send && meterEnabled.load (std::memory_order_relaxed)
        && numChannels > 0)
    {
        float peak = 0.0f;
        const int numSamples = buffer.getNumSamples();
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* data = buffer.getReadPointer (ch);
            for (int i = 0; i < numSamples; ++i)
            {
                const float s = data[i] < 0.0f ? -data[i] : data[i];
                peak = juce::jmax (peak, s);
            }
        }
        outputPeak.store (peak, std::memory_order_relaxed);
    }

    // ---- Push transitions (Send mode, audio thread: atomics only). ----
    if (mode == PluginMode::Send && pushPrepared.load (std::memory_order_acquire))
    {
        if (! pushRecording.load (std::memory_order_acquire))
        {
            // Start on Play when the playhead is known; start on the
            // first block after arming when it is not (the user was
            // told to press Play right after Ready, so any pre-Play
            // silence in the take is short and still preserves
            // alignment, since every Send starts the same way).
            if ((timeKnownNow && playingNow) || ! timeKnownNow)
                startPushRecordingFromAudioThread();
        }
        else
        {
            bool shouldFinish = PushSession::getInstance().isFinishRequested();
            int reason = 2; // Finish & upload
            if (! shouldFinish && timeKnownNow && wasTransportPlaying && ! playingNow)
            {
                shouldFinish = true;
                reason = 1; // transport stopped
            }
            if (shouldFinish)
                finishPushRecordingFromAudioThread (reason);
        }
    }
    if (timeKnownNow)
        wasTransportPlaying = playingNow;

    // ---- Transport-jump guard (Send Push recording only). ----
    // A take must be ONE continuous pass: if the playhead is not
    // where the previous block left it (beyond tolerance), playback
    // looped, sought, or restarted. Finalizing here keeps the take
    // aligned to the measures it actually covers, instead of gluing
    // a second pass or a distant region onto the same file — the
    // cut/restart artefact that sounds amateur on re-import. The
    // Manager tells the user to turn Loop off and record one pass.
    // Runs after the Stop/Finish transitions above, so a normal
    // Stop still wins over a jump in the same block.
    if (mode == PluginMode::Send && pushRecording.load (std::memory_order_acquire))
    {
        if (timeKnownNow)
        {
            const int64_t last = lastPushPlayhead.load (std::memory_order_relaxed);
            const int64_t diff = timeSamplesNow
                - (last + lastPushBlockSamples.load (std::memory_order_relaxed));
            if (last >= 0 && (diff > 4096 || diff < -4096))
            {
                finishPushRecordingFromAudioThread (5); // transport jumped
            }
            if (pushRecording.load (std::memory_order_acquire))
            {
                lastPushPlayhead.store (timeSamplesNow, std::memory_order_relaxed);
                lastPushBlockSamples.store (buffer.getNumSamples(), std::memory_order_relaxed);
            }
        }
        else
        {
            // Position unknown: drop the reference so the next known
            // block re-syncs instead of reading as a jump.
            lastPushPlayhead.store (-1, std::memory_order_relaxed);
        }
    }

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
    // Persist Hub URL + cache dir + mode + track name + instance id.
    // Tokens/credentials are never stored here (they live in the Hub /
    // OS keychain), so an FL Studio project file can never leak them.
    // An old state (no mode property) loads as Manager below.
    juce::ValueTree state ("OnorabState");
    state.setProperty ("hubUrl", hubUrl, nullptr);
    state.setProperty ("cacheDir", cacheDir.getFullPathName(), nullptr);
    state.setProperty ("pluginMode",
                       mode == PluginMode::Send ? "send"
                       : mode == PluginMode::Undecided ? "undecided" : "manager",
                       nullptr);
    state.setProperty ("trackName", trackName, nullptr);
    state.setProperty ("instanceId", instanceId, nullptr);
    state.setProperty ("wavBits", wavBits.load(), nullptr);
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

        // Mode: a state without the property is an OLD project, which
        // opens as Manager with its previous settings intact.
        PluginMode loadedMode = PluginMode::Manager;
        if (state.hasProperty ("pluginMode"))
        {
            const juce::String m = state.getProperty ("pluginMode").toString();
            if (m == "send")
                loadedMode = PluginMode::Send;
            else if (m == "undecided")
                loadedMode = PluginMode::Undecided;
        }
        if (state.hasProperty ("trackName"))
            trackName = state.getProperty ("trackName").toString();

        // Unregister any previous Send registration (a reload replaces
        // the state wholesale; the old id must not linger in the
        // session). Then apply the loaded mode directly — setMode's
        // push guard does not apply here because nothing can be
        // prepared during a state load.
        if (mode == PluginMode::Send)
        {
            PushSession::getInstance().unregisterSend (instanceId);
            pushEntry.reset();
        }
        mode = loadedMode;

        if (state.hasProperty ("instanceId"))
            instanceId = state.getProperty ("instanceId").toString();
        // Take format: 16 stays 16; anything else (including a state
        // saved before this setting existed) becomes the 32-bit
        // float default.
        if (state.hasProperty ("wavBits"))
            wavBits.store (static_cast<int> (state.getProperty ("wavBits")) == 16 ? 16 : 32);
        if (mode == PluginMode::Send)
        {
            if (instanceId.isEmpty())
                instanceId = generateInstanceId();
            // Clone case: FL copied this state onto a new instance
            // while the original still lives. The copy must get its
            // OWN identity, or the Manager would arm one Send twice.
            // The new id is saved into the project on the next host
            // save; in this session it is already unique.
            while (PushSession::getInstance().isIdLive (instanceId))
                instanceId = generateInstanceId();
            if (trackName.isEmpty())
                trackName = "Track 01";
            pushEntry = PushSession::getInstance().registerSend (
                instanceId, trackName,
                [this] (int pid) { return preparePushRecording (pid); },
                [this] { cancelPushPreparation(); });
        }
    }
}

// ---- Send / Manager mode ----

juce::String OnorabAudioProcessor::generateInstanceId()
{
    // Random 64-bit hex. Uniqueness only needs to hold within one FL
    // process (the registry is per-process), but a random id also
    // survives a project file copied to another PC.
    const uint64_t r = static_cast<uint64_t> (juce::Random::getSystemRandom().nextInt64());
    juce::String hex;
    for (int shift = 60; shift >= 0; shift -= 4)
        hex += "0123456789ABCDEF"[(r >> shift) & 0xF];
    return "send-" + hex;
}

void OnorabAudioProcessor::setMode (PluginMode newMode)
{
    if (newMode == mode)
        return;
    // Never orphan a live Push writer by switching mode under it.
    if (pushPrepared.load())
        return;
    if (mode == PluginMode::Send)
    {
        PushSession::getInstance().unregisterSend (instanceId);
        pushEntry.reset();
    }
    mode = newMode;
    if (newMode == PluginMode::Send)
    {
        if (instanceId.isEmpty())
            instanceId = generateInstanceId();
        // Same clone guard as setStateInformation (single message
        // thread, so check-then-register cannot race).
        while (PushSession::getInstance().isIdLive (instanceId))
            instanceId = generateInstanceId();
        pushEntry = PushSession::getInstance().registerSend (
            instanceId, trackName,
            [this] (int pid) { return preparePushRecording (pid); },
            [this] { cancelPushPreparation(); });
    }
}

void OnorabAudioProcessor::setTrackName (const juce::String& name)
{
    trackName = name;
    // Registered Sends rename live in the Manager list; unregistered
    // instances (choice not made yet) just keep the member.
    if (pushEntry != nullptr)
        PushSession::getInstance().renameSend (instanceId, name);
}

void OnorabAudioProcessor::startCapture (const juce::String& fileName, CaptureFinishedCallback cb)
{
    if (isCapturing() || pushPrepared.load())
    {
        // Already capturing (manual, auto, or a prepared Push writer):
        // report, don't silently restart and lose audio.
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
    if (isCapturing() || pushPrepared.load())
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
                                       wavBits.load (std::memory_order_relaxed), {}, 0));
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
                                           wavBits.load (std::memory_order_relaxed), {}, 0));
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

// ---- Push recording (Send mode) ----

juce::String OnorabAudioProcessor::preparePushRecording (int pushId)
{
    // Message thread, called by PushSession while the registry lock is
    // held (so this Send cannot be destroyed mid-prepare). The writer
    // launches now and idles on an empty FIFO; the audio thread starts
    // it for real when the transport starts playing.
    if (mode != PluginMode::Send)
        return "This instance is not in Send mode.";
    if (isCapturing())
        return "Send is busy with another capture.";
    if (pushPrepared.load())
    {
        // A previous Push ended (writer exited) or was prepared but
        // never started (writer idle): shut that writer down first so
        // this prepare owns a fresh thread. A RECORDING Send cannot
        // reach here: it has isCapturing() true and failed above.
        pushWriterDone.store (true, std::memory_order_release);
        if (captureWriterThread.joinable())
            captureWriterThread.join();
        pushPrepared.store (false, std::memory_order_release);
    }
    if (captureWriterThread.joinable())
        captureWriterThread.join();

    const int channels = juce::jlimit (1, 2, lastNumChannels.load());
    const double sampleRate = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    if (! cacheDir.exists() && ! cacheDir.createDirectory())
        return "Could not create the cache folder: " + cacheDir.getFullPathName();

    // Unique per Push AND per Send: the track name plus a millisecond
    // timestamp plus the instance's short id. Two Sends with the same
    // track name can never truncate each other's take, and a new Push
    // for the same track never silently overwrites the previous take.
    pushTakeName = trackName.isNotEmpty() ? trackName : "Track";
    const juce::String shortId = instanceId.length() >= 4
        ? instanceId.substring (instanceId.length() - 4) : instanceId;
    const juce::String fileName = HubClient::sanitiseFileName (
        pushTakeName + "_push-"
        + juce::String (juce::Time::getCurrentTime().toMilliseconds())
        + "-" + shortId + ".wav");
    juce::File outFile = cacheDir.getChildFile (fileName);

    pushIdCurrent = pushId;
    pushDroppedBaseline = droppedSamples.load();
    pushWriterDone.store (false, std::memory_order_release);
    pushRecording.store (false, std::memory_order_release);
    pushFinished.store (false, std::memory_order_release);
    pushFinishReason.store (0, std::memory_order_relaxed);
    pushLeadSilenceFrames.store (0, std::memory_order_relaxed);
    captureFifo.reset();

    captureWriterThread = std::thread ([this, outFile, channels, sampleRate, pushId]
    {
        captureWriterLoopPush (channels, sampleRate, outFile, pushId);
    });
    pushPrepared.store (true, std::memory_order_release);
    if (pushEntry != nullptr)
        pushEntry->status.store (1); // Prepared
    return "";
}

void OnorabAudioProcessor::cancelPushPreparation()
{
    // Only a prepared-but-never-started Send can be cancelled: its
    // writer exits with reason "cancelled" and queues no take. A
    // recording Send ignores this (it ends via Stop / Finish only).
    if (! pushPrepared.load() || pushRecording.load())
        return;
    pushFinishReason.store (4, std::memory_order_relaxed);
    pushWriterDone.store (true, std::memory_order_release);
    pushPrepared.store (false, std::memory_order_release);
    if (pushEntry != nullptr)
        pushEntry->status.store (0); // Idle
    // The thread object is joined at the next prepare / destruction;
    // the writer itself has already been told to exit.
}

void OnorabAudioProcessor::startPushRecordingFromAudioThread()
{
    // Audio thread: atomics + the FIFO flag only. No allocation, no
    // locks, no I/O. The order matters: lead silence is stored BEFORE
    // the capturing flag (release), so the writer — which only sees
    // FIFO data after the flag — always sees the padding.
    const int64_t startSample = transportTimeKnown.load (std::memory_order_acquire)
        ? transportTimeSamples.load (std::memory_order_acquire) : -1;
    if (startSample >= 0)
    {
        int64_t expected = -1;
        PushSession::getInstance().compareExchangePushStart (expected, startSample);
        const int64_t sessionStart = PushSession::getInstance().getPushStartSample();
        pushLeadSilenceFrames.store (
            sessionStart >= 0 && startSample > sessionStart ? startSample - sessionStart : 0,
            std::memory_order_relaxed);
    }
    else
    {
        pushLeadSilenceFrames.store (0, std::memory_order_relaxed);
    }
    // No playhead reference yet: the first recorded block sets it,
    // so a stale position from an earlier take can never read as a
    // jump on the first block of this one.
    lastPushPlayhead.store (-1, std::memory_order_relaxed);
    capturing.store (true, std::memory_order_release);
    pushRecording.store (true, std::memory_order_release);
    if (pushEntry != nullptr)
    {
        pushEntry->status.store (2); // Recording
        pushEntry->framesWritten.store (0);
    }
}

void OnorabAudioProcessor::finishPushRecordingFromAudioThread (int reasonCode)
{
    // Audio thread: reason FIRST, then the flags (release), so the
    // writer always sees the reason when it drains and exits.
    pushFinishReason.store (reasonCode, std::memory_order_relaxed);
    lastPushPlayhead.store (-1, std::memory_order_relaxed);
    capturing.store (false, std::memory_order_release);
    pushWriterDone.store (true, std::memory_order_release);
    pushRecording.store (false, std::memory_order_release);
    pushFinished.store (true, std::memory_order_release);
    if (pushEntry != nullptr)
        pushEntry->status.store (3); // Finished
}

void OnorabAudioProcessor::cleanupPushWriter()
{
    if (! pushPrepared.load())
        return;
    // A mid-recording destruction is a partial take ("destroyed"); a
    // prepared-but-never-started destruction is an abandoned Push
    // ("cancelled": no take). A normally finalized Push keeps its
    // reason — the writer already queued its take.
    if (pushRecording.load())
        pushFinishReason.store (3, std::memory_order_relaxed);
    else
        pushFinishReason.store (4, std::memory_order_relaxed);
    capturing.store (false, std::memory_order_release);
    pushWriterDone.store (true, std::memory_order_release);
    pushRecording.store (false, std::memory_order_release);
    if (captureWriterThread.joinable())
        captureWriterThread.join();
    pushPrepared.store (false, std::memory_order_release);
}

void OnorabAudioProcessor::captureWriterLoopPush (int numChannels, double sampleRate,
                                                  juce::File outFile, int pushId)
{
    // Writer thread ONLY. Lazy file creation: a prepared-but-never-
    // started Send creates no file and (unless cancelled) queues an
    // empty take, so the Manager can resolve it as "No audio recorded"
    // instead of waiting forever. NOTHING is silence-gated here —
    // silence is timing — and leading silence pads the take up to the
    // session start sample before the first real audio.
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    juce::String writeError;
    int64_t totalFrames = 0;
    bool fileCreated = false;
    bool leadWritten = false;

    auto createWriter = [&]() -> bool
    {
        std::unique_ptr<juce::FileOutputStream> stream (outFile.createOutputStream());
        if (stream == nullptr)
        {
            writeError = "Could not create the take file: " + outFile.getFullPathName();
            return false;
        }
        std::unique_ptr<juce::AudioFormatWriter> newWriter (
            wavFormat.createWriterFor (stream.get(), sampleRate,
                                       static_cast<unsigned int> (numChannels),
                                       wavBits.load (std::memory_order_relaxed), {}, 0));
        if (newWriter == nullptr)
        {
            outFile.deleteFile();
            writeError = "Could not create a WAV writer for: " + outFile.getFullPathName();
            return false;
        }
        stream.release(); // owned by the writer now
        writer = std::move (newWriter);
        fileCreated = true;
        return true;
    };

    auto writeSilence = [&](int64_t frames) -> bool
    {
        juce::AudioBuffer<float> silence (numChannels, 4096);
        silence.clear();
        while (frames > 0)
        {
            const int chunk = static_cast<int> (juce::jmin<int64_t> (4096, frames));
            if (! writer->writeFromAudioSampleBuffer (silence, 0, chunk))
                return false;
            frames -= chunk;
            totalFrames += chunk;
        }
        return true;
    };

    constexpr int kChunkFrames = 4096;
    juce::AudioBuffer<float> chunk (numChannels, kChunkFrames);
    std::vector<float> interleaved (static_cast<size_t> (kChunkFrames)
                                    * static_cast<size_t> (numChannels));

    // The loop also watches the session's finishRequested flag: a
    // prepared-but-never-started Send has no audio-thread finalize
    // (it never started), so Finish & upload is its only exit signal.
    while ((! pushWriterDone.load (std::memory_order_acquire)
            && ! PushSession::getInstance().isFinishRequested())
           || captureFifo.getNumReady() > 0)
    {
        const int readySamples = captureFifo.getNumReady();
        if (readySamples < numChannels)
        {
            if (pushWriterDone.load (std::memory_order_acquire)
                || PushSession::getInstance().isFinishRequested())
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

        if (! fileCreated && ! createWriter())
            break;
        if (! leadWritten)
        {
            leadWritten = true;
            const int64_t lead = pushLeadSilenceFrames.load (std::memory_order_relaxed);
            if (lead > 0 && ! writeSilence (lead))
            {
                writeError = "Failed while writing the take file: " + outFile.getFullPathName();
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
            writeError = "Failed while writing the take file: " + outFile.getFullPathName();
            break;
        }
        totalFrames += framesRead;
        if (pushEntry != nullptr)
            pushEntry->framesWritten.store (totalFrames);
    }

    if (writer != nullptr)
    {
        writer->flush();
        writer.reset(); // closes the file
    }

    const int dropped = droppedSamples.load() - pushDroppedBaseline;
    const int reason = pushFinishReason.load (std::memory_order_relaxed);
    juce::String finishedBy;
    if (reason == 1)
        finishedBy = "stop";
    else if (reason == 2)
        finishedBy = "finish";
    else if (reason == 3)
        finishedBy = "destroyed";
    else if (reason == 4)
        finishedBy = "cancelled";
    else if (reason == 5)
        finishedBy = "jumped";
    else if (PushSession::getInstance().isFinishRequested())
        finishedBy = "finish"; // prepared, never started, ended by Finish
    else
        finishedBy = "stop";

    // Cancelled writers queue nothing: the Manager already forgot the
    // Push. Every other exit queues exactly one take — a real file
    // (even partial), an error take (no file + reason), or an empty
    // take (no audio recorded) — so the Manager can resolve every
    // armed track instead of waiting forever.
    if (finishedBy != "cancelled")
    {
        if (fileCreated && writeError.isEmpty() && outFile.existsAsFile())
        {
            PushSession::getInstance().queueTake (
                { instanceId, pushTakeName, outFile, totalFrames,
                  dropped > 0 ? dropped : 0, finishedBy, pushId, {} });
        }
        else if (! writeError.isEmpty())
        {
            if (fileCreated)
                outFile.deleteFile();
            PushSession::getInstance().queueTake (
                { instanceId, pushTakeName, {}, 0,
                  dropped > 0 ? dropped : 0, finishedBy, pushId, writeError });
        }
        else
        {
            PushSession::getInstance().queueTake (
                { instanceId, pushTakeName, {}, 0,
                  dropped > 0 ? dropped : 0, finishedBy, pushId, {} });
        }
    }

    if (pushEntry != nullptr)
        pushEntry->status.store (3); // Finished (writer done)
    pushPrepared.store (false, std::memory_order_release);
}

// Required by JUCE: factory function that creates an instance of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OnorabAudioProcessor();
}
