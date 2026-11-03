#pragma once

// WaveformView — draws the waveform of a downloaded / selected local file.
//
// Runs on the message thread only: AudioThumbnail loads its data in the
// background itself and notifies this component via ChangeListener, which
// triggers a repaint. Nothing here touches the audio thread or the network.

#include <JuceHeader.h>

class WaveformView : public juce::Component,
                    private juce::ChangeListener
{
public:
    explicit WaveformView (juce::AudioFormatManager& formatManagerToUse);
    ~WaveformView() override;

    // Points the view at a local audio file (already downloaded to the
    // cache dir). A missing/unreadable file shows a placeholder message.
    void setFile (const juce::File& file);
    void clear();
    juce::File getFile() const { return currentFile; }

    void paint (juce::Graphics& g) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;

    juce::AudioFormatManager& formatManager;
    juce::AudioThumbnailCache thumbnailCache { 5 };
    juce::AudioThumbnail thumbnail { 512, formatManager, thumbnailCache };
    juce::File currentFile;
    juce::String errorMessage;
};
