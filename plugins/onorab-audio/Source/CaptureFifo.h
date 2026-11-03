#pragma once

// CaptureFifo — lock-free single-producer / single-consumer ring buffer.
//
// Producer: the real-time audio thread (PluginProcessor::processBlock).
// Consumer: the background capture-writer thread (PluginProcessor).
// No allocation, no locks, no I/O happen in write()/read() — the buffer is
// fully pre-allocated in the constructor. If the writer falls behind and
// the ring fills up, write() drops the excess and returns how many samples
// it actually stored; the processor counts the dropped samples and reports
// them when capture stops instead of silently losing audio.
//
// This wraps juce::AbstractFifo, which is designed exactly for this
// SPSC pattern (one writer, one reader, wait-free on both sides).

#include <JuceHeader.h>
#include <algorithm>
#include <vector>

class CaptureFifo
{
public:
    // capacitySamples must be > 0. Default: ~10 seconds of stereo audio
    // at 48 kHz (1,048,576 float samples), so a slow disk hiccup does not
    // immediately drop captured audio.
    explicit CaptureFifo (int capacitySamples = 1 << 20)
        : fifo (capacitySamples),
          buffer (static_cast<size_t> (capacitySamples), 0.0f)
    {
    }

    CaptureFifo (const CaptureFifo&) = delete;
    CaptureFifo& operator= (const CaptureFifo&) = delete;

    // Called ONLY from the audio thread. Returns samples actually stored.
    int write (const float* data, int numSamples) noexcept
    {
        if (data == nullptr || numSamples <= 0)
            return 0;

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToWrite (numSamples, start1, size1, start2, size2);

        if (size1 > 0)
            std::copy (data, data + size1, buffer.begin() + start1);
        if (size2 > 0)
            std::copy (data + size1, data + size1 + size2, buffer.begin() + start2);

        fifo.finishedWrite (size1 + size2);
        return size1 + size2;
    }

    // Called ONLY from the writer thread. Returns samples actually read.
    int read (float* dest, int numSamples) noexcept
    {
        if (dest == nullptr || numSamples <= 0)
            return 0;

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (numSamples, start1, size1, start2, size2);

        if (size1 > 0)
            std::copy (buffer.begin() + start1, buffer.begin() + start1 + size1, dest);
        if (size2 > 0)
            std::copy (buffer.begin() + start2, buffer.begin() + start2 + size2, dest + size1);

        fifo.finishedRead (size1 + size2);
        return size1 + size2;
    }

    int getNumReady() const noexcept { return fifo.getNumReady(); }
    int getFreeSpace() const noexcept { return fifo.getFreeSpace(); }

    // Called from the message thread, only when capture is stopped and
    // the writer thread has finished draining.
    void reset() noexcept { fifo.reset(); }

private:
    juce::AbstractFifo fifo;
    std::vector<float> buffer;
};
