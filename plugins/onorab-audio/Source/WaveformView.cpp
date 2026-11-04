// WaveformView implementation. See WaveformView.h.

#include "WaveformView.h"

WaveformView::WaveformView (juce::AudioFormatManager& formatManagerToUse)
    : formatManager (formatManagerToUse)
{
    thumbnail.addChangeListener (this);
}

WaveformView::~WaveformView()
{
    thumbnail.removeChangeListener (this);
}

void WaveformView::setFile (const juce::File& file)
{
    currentFile = file;
    errorMessage.clear();

    if (! file.existsAsFile())
    {
        errorMessage = "No local file yet - press Download on this sound first.";
        thumbnail.clear();
        repaint();
        return;
    }

    // FileInputSource is owned by the thumbnail; the thumbnail loads it
    // on its own background thread and repaints via ChangeListener.
    thumbnail.setSource (new juce::FileInputSource (file));
    repaint();
}

void WaveformView::clear()
{
    currentFile = {};
    errorMessage.clear();
    thumbnail.clear();
    repaint();
}

void WaveformView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff16161b)); // dark panel, matches the redesign palette

    if (errorMessage.isNotEmpty())
    {
        g.setColour (juce::Colours::lightgrey);
        g.setFont (13.0f);
        g.drawFittedText (errorMessage, getLocalBounds().reduced (8), juce::Justification::centred, 3);
        return;
    }

    if (thumbnail.getNumChannels() == 0)
    {
        g.setColour (juce::Colours::grey);
        g.setFont (13.0f);
        g.drawFittedText ("Loading waveform...", getLocalBounds(), juce::Justification::centred, 1);
        return;
    }

    // Light purple waveform, matching the redesign accent colour.
    g.setColour (juce::Colour (0xffb7a6ff));
    thumbnail.drawChannels (g, getLocalBounds().reduced (4),
                            0.0, thumbnail.getTotalLength(), 1.0f);
}

void WaveformView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    repaint();
}
