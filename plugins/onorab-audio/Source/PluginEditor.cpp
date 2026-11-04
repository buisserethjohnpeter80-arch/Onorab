// OnorabAudioProcessorEditor implementation (redesign 2026-11-04).
// See PluginEditor.h for the design notes, the honest drag-and-drop
// contract and the ASCII-only text rule. All Hub interaction goes
// through HubClient (background thread) and all UI updates run on the
// message thread. No network, file I/O or allocation for audio happens
// here that could reach the real-time thread - this file never touches
// processBlock.

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "PushSession.h"
#include <algorithm>

namespace
{
// Palette from the reference mockup: near-black background, dark panels,
// light readable text, light purple for selection and primary actions.
const juce::Colour kBackground (0xff0c0c0f);
const juce::Colour kPanel (0xff16161b);
const juce::Colour kPanel2 (0xff1e1e24);
const juce::Colour kSelected (0xff2e2440);
const juce::Colour kPurple (0xffb7a6ff); // light purple: primary actions
const juce::Colour kPurpleDark (0xff8b5cf6);
const juce::Colour kText (0xfff2f2f5);
const juce::Colour kTextDim (0xffa1a1aa);
const juce::Colour kGreen (0xff22c55e);
const juce::Colour kRed (0xffef4444);
const juce::Colour kGrey (0xff71717a);

void styleButton (juce::TextButton& button, juce::Colour textColour = kText)
{
    button.setColour (juce::TextButton::buttonColourId, kPanel2);
    button.setColour (juce::TextButton::textColourOffId, textColour);
    button.setColour (juce::TextButton::textColourOnId, textColour);
}

void stylePrimaryButton (juce::TextButton& button)
{
    button.setColour (juce::TextButton::buttonColourId, kPurple);
    button.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    button.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
}

void styleLabel (juce::Label& label, juce::Colour colour = kText, float size = 13.0f)
{
    label.setColour (juce::Label::textColourId, colour);
    label.setFont (juce::Font (juce::FontOptions (size)));
    label.setJustificationType (juce::Justification::left);
}

void styleSearchBox (juce::TextEditor& box, const juce::String& emptyText)
{
    box.setTextToShowWhenEmpty (emptyText, kGrey);
    box.setColour (juce::TextEditor::backgroundColourId, kPanel2);
    box.setColour (juce::TextEditor::textColourId, kText);
    box.setColour (juce::TextEditor::outlineColourId, kPanel2);
    box.setColour (juce::TextEditor::focusedOutlineColourId, kPurpleDark);
    box.setFont (juce::Font (juce::FontOptions (13.0f)));
}

void styleTextBox (juce::TextEditor& box, const juce::String& emptyText)
{
    styleSearchBox (box, emptyText);
}

juce::String formatBytes (int64_t bytes)
{
    if (bytes < 1024)
        return juce::String (bytes) + " B";
    if (bytes < 1024 * 1024)
        return juce::String (bytes / 1024.0, 1) + " KB";
    return juce::String (bytes / (1024.0 * 1024.0), 1) + " MB";
}

juce::String formatDuration (double seconds)
{
    if (seconds < 0)
        return "--:--";
    const int total = static_cast<int> (seconds + 0.5);
    return juce::String (total / 60) + ":"
         + juce::String (total % 60).paddedLeft ('0', 2);
}

bool isAudioFile (const juce::String& path)
{
    static const juce::StringArray exts { "wav", "mp3", "flac", "aiff", "aif", "ogg", "m4a" };
    return exts.contains (juce::File (path).getFileExtension().toLowerCase(), true);
}
} // namespace

// ============================== IconButton ==============================

void IconButton::setPauseIcon (bool isPause)
{
    pauseIcon = isPause;
    repaint();
}

void IconButton::setStopIcon (bool isStop)
{
    stopIcon = isStop;
    repaint();
}

void IconButton::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    g.setColour (kPurple);
    g.fillEllipse (area);
    g.setColour (juce::Colours::black);
    if (stopIcon)
    {
        const float w = area.getWidth(), h = area.getHeight();
        g.fillRect (area.getX() + w * 0.32f, area.getY() + h * 0.32f, w * 0.36f, h * 0.36f);
    }
    else if (pauseIcon)
    {
        const float w = area.getWidth(), h = area.getHeight();
        g.fillRect (area.getX() + w * 0.32f, area.getY() + h * 0.28f, w * 0.13f, h * 0.44f);
        g.fillRect (area.getX() + w * 0.55f, area.getY() + h * 0.28f, w * 0.13f, h * 0.44f);
    }
    else
    {
        juce::Path triangle;
        const float x = area.getX(), y = area.getY(), w = area.getWidth(), h = area.getHeight();
        triangle.addTriangle (x + w * 0.36f, y + h * 0.26f,
                              x + w * 0.36f, y + h * 0.74f,
                              x + w * 0.74f, y + h * 0.50f);
        g.fillPath (triangle);
    }
}

void IconButton::mouseUp (const juce::MouseEvent& event)
{
    if (event.mouseWasClicked() && onClick != nullptr)
        onClick();
}

// ============================== DragGrip ==============================

void DragGrip::setEnabledGrip (bool enabled)
{
    gripEnabled = enabled;
    repaint();
}

void DragGrip::paint (juce::Graphics& g)
{
    g.setColour (gripEnabled ? kPurple : kGrey);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 2; ++col)
            g.fillEllipse (3.0f + static_cast<float> (col) * 6.0f,
                           5.0f + static_cast<float> (row) * 8.0f, 3.0f, 3.0f);
}

void DragGrip::mouseDrag (const juce::MouseEvent&)
{
    if (isDragging || getFilesToDrag == nullptr || ! gripEnabled)
        return;
    juce::StringArray files;
    for (const auto& path : getFilesToDrag())
        if (juce::File (path).existsAsFile())
            files.add (path);
    if (files.isEmpty())
        return;
    auto* container = findParentComponentOfClass<juce::DragAndDropContainer>();
    if (container == nullptr)
        return;
    // Hand the host real file paths. Whether FL Studio accepts the drop
    // is the host's decision (see the header note); this call is the
    // plugin's side of the contract, and it blocks until the drag ends.
    isDragging = true;
    container->performExternalDragDropOfFiles (files, false);
    isDragging = false;
}

// ============================== StatusDot ==============================

void StatusDot::setDotColour (juce::Colour colour)
{
    dotColour = colour;
    repaint();
}

void StatusDot::paint (juce::Graphics& g)
{
    g.setColour (dotColour);
    g.fillEllipse (getLocalBounds().toFloat().reduced (2.0f));
}

// ============================== Library row ==============================

// One Library row: drag grip, play button, name, size, and the status
// area (Download button, Downloading progress, or Ready for FL).
// The row never invents data: size comes from the Hub, the local state
// from the editor's cache check, progress from the real download.
class LibraryRowComponent : public juce::Component
{
public:
    struct State
    {
        CloudFile file;
        bool isLocal = false;
        bool isPlaying = false;
        bool isSelected = false;
        bool downloading = false;
        double progress = -1.0; // 0..1, or < 0 when unknown
        std::function<void()> onPlay;
        std::function<void()> onDownload;
        std::function<juce::StringArray()> getDragFiles;
    };

    LibraryRowComponent()
    {
        playButton.onClick = [this] { if (onPlay != nullptr) onPlay(); };
        addAndMakeVisible (playButton);
        grip.getFilesToDrag = [this]
        {
            if (getDragFiles != nullptr)
                return getDragFiles();
            return juce::StringArray {};
        };
        addAndMakeVisible (grip);
        styleLabel (nameLabel, kText, 13.0f);
        addAndMakeVisible (nameLabel);
        styleLabel (metaLabel, kTextDim, 12.0f);
        metaLabel.setJustificationType (juce::Justification::right);
        addAndMakeVisible (metaLabel);
        styleButton (downloadButton, kPurple);
        downloadButton.onClick = [this] { if (onDownload != nullptr) onDownload(); };
        addAndMakeVisible (downloadButton);
        styleLabel (statusLabel, kGreen, 12.0f);
        addAndMakeVisible (statusLabel);
    }

    void update (const State& state)
    {
        onPlay = state.onPlay;
        onDownload = state.onDownload;
        getDragFiles = state.getDragFiles;
        selected = state.isSelected;
        playButton.setPauseIcon (state.isPlaying);
        grip.setEnabledGrip (state.isLocal);
        nameLabel.setText (state.file.name, juce::NotificationType::dontSendNotification);
        metaLabel.setText (formatBytes (state.file.sizeBytes),
                           juce::NotificationType::dontSendNotification);
        if (state.isLocal)
        {
            downloadButton.setVisible (false);
            statusLabel.setVisible (true);
            statusLabel.setText ("Ready for FL", juce::NotificationType::dontSendNotification);
            statusLabel.setColour (juce::Label::textColourId, kGreen);
            progressVisible = false;
        }
        else if (state.downloading)
        {
            downloadButton.setVisible (false);
            statusLabel.setVisible (true);
            statusLabel.setText (state.progress >= 0
                                     ? "Downloading " + juce::String (static_cast<int> (state.progress * 100.0)) + "%"
                                     : "Downloading...",
                                 juce::NotificationType::dontSendNotification);
            statusLabel.setColour (juce::Label::textColourId, kTextDim);
            progressVisible = state.progress >= 0;
            progressValue = state.progress >= 0 ? state.progress : 0.0;
        }
        else
        {
            downloadButton.setVisible (true);
            downloadButton.setButtonText ("Download");
            statusLabel.setVisible (false);
            progressVisible = false;
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (selected)
            g.fillAll (kSelected);
        if (progressVisible)
        {
            g.setColour (kPanel2);
            g.fillRect (0, getHeight() - 3, getWidth(), 3);
            g.setColour (kPurple);
            g.fillRect (0, getHeight() - 3,
                        static_cast<int> (static_cast<double> (getWidth()) * progressValue), 3);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (4, 3);
        grip.setBounds (area.removeFromLeft (16));
        area.removeFromLeft (4);
        playButton.setBounds (area.removeFromLeft (28));
        area.removeFromLeft (8);
        auto statusArea = area.removeFromRight (130);
        downloadButton.setBounds (statusArea);
        statusLabel.setBounds (statusArea);
        area.removeFromRight (8);
        metaLabel.setBounds (area.removeFromRight (80));
        nameLabel.setBounds (area);
    }

private:
    DragGrip grip;
    IconButton playButton;
    juce::Label nameLabel, metaLabel, statusLabel;
    juce::TextButton downloadButton { "Download" };
    std::function<void()> onPlay, onDownload;
    std::function<juce::StringArray()> getDragFiles;
    bool selected = false;
    bool progressVisible = false;
    double progressValue = 0.0;
};

// ============================== Transfer row ==============================

class TransferRowComponent : public juce::Component
{
public:
    TransferRowComponent()
    {
        styleLabel (nameLabel, kText, 13.0f);
        addAndMakeVisible (nameLabel);
        styleLabel (infoLabel, kTextDim, 12.0f);
        addAndMakeVisible (infoLabel);
        styleLabel (errorLabel, kRed, 12.0f);
        addAndMakeVisible (errorLabel);
        styleButton (retryButton, kPurple);
        retryButton.onClick = [this] { if (onRetry != nullptr) onRetry(); };
        addAndMakeVisible (retryButton);
    }

    void update (const TransferRow& row, std::function<void()> retry)
    {
        onRetry = std::move (retry);
        nameLabel.setText (row.fileName, juce::NotificationType::dontSendNotification);
        infoLabel.setText ((row.direction.isNotEmpty() ? row.direction : "Transfer")
                               + " - " + row.status,
                           juce::NotificationType::dontSendNotification);
        const bool failed = row.status == "Failed";
        errorLabel.setVisible (failed && row.error.isNotEmpty());
        errorLabel.setText (row.error, juce::NotificationType::dontSendNotification);
        retryButton.setVisible (row.canRetry);
        retryButton.setButtonText ("Retry");
        progressValue = juce::jlimit (0.0, 1.0, row.progress);
        done = row.status == "Done";
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        // Progress bar along the bottom of the row.
        g.setColour (kPanel2);
        g.fillRect (0, getHeight() - 3, getWidth(), 3);
        g.setColour (done ? kGreen : kPurple);
        g.fillRect (0, getHeight() - 3,
                    static_cast<int> (static_cast<double> (getWidth()) * progressValue), 3);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (4, 3);
        auto right = area.removeFromRight (90);
        retryButton.setBounds (right);
        nameLabel.setBounds (area.removeFromTop (18));
        infoLabel.setBounds (area.removeFromTop (16));
        errorLabel.setBounds (area);
    }

private:
    juce::Label nameLabel, infoLabel, errorLabel;
    juce::TextButton retryButton { "Retry" };
    std::function<void()> onRetry;
    double progressValue = 0.0;
    bool done = false;
};

// ============================== LibraryTab ==============================

LibraryTab::LibraryTab (OnorabAudioProcessorEditor& editorIn)
    : owner (editorIn),
      waveform (waveformFormatManager)
{
    waveformFormatManager.registerBasicFormats();

    styleSearchBox (searchBox, "Search sounds...");
    searchBox.addListener (this);
    addAndMakeVisible (searchBox);

    styleButton (refreshButton);
    refreshButton.onClick = [this] { owner.refreshFiles(); };
    addAndMakeVisible (refreshButton);

    // No laptop file picker here by design (2026-11-04): sounds reach
    // the Library from the phone via Drive, or from FL Studio via
    // Capture from FL / project Auto-capture — never from a local
    // file chooser. (The editor's uploadLocalFile helpers stay for
    // those flows; only the picker button is gone.)
    styleButton (deleteButton, kRed);
    deleteButton.onClick = [this]
    {
        if (const int row = fileList.getSelectedRow(); row >= 0
            && row < static_cast<int> (filteredFiles.size()))
            owner.deleteHubFiles ({ filteredFiles[static_cast<size_t> (row)] });
    };
    addAndMakeVisible (deleteButton);

    styleLabel (messageLabel, kTextDim, 12.0f);
    addAndMakeVisible (messageLabel);
    styleLabel (errorLabel, kRed, 12.0f);
    addAndMakeVisible (errorLabel);

    styleLabel (emptyLabel, kTextDim, 14.0f);
    emptyLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (emptyLabel);
    stylePrimaryButton (emptyCaptureButton);
    emptyCaptureButton.onClick = [this]
    {
        // The header Capture button was replaced by the Tracks
        // workflow, so this button toggles manual capture itself:
        // Start when idle, Stop while capturing (the review panel
        // appears on stop, as before).
        if (owner.isCapturing())
            owner.stopCapture();
        else
            owner.startCapture();
    };
    addAndMakeVisible (emptyCaptureButton);

    fileList.setRowHeight (34);
    fileList.setMultipleSelectionEnabled (false);
    addAndMakeVisible (fileList);

    // Player card for the selected sound.
    styleLabel (playerNameLabel, kText, 14.0f);
    playerNameLabel.setText ("No sound selected", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (playerNameLabel);
    addAndMakeVisible (waveform);
    playerPlayButton.onClick = [this] { owner.togglePlayerPlay(); };
    addAndMakeVisible (playerPlayButton);
    playerStopButton.onClick = [this] { owner.stopPreview(); };
    playerStopButton.setStopIcon (true);
    addAndMakeVisible (playerStopButton);
    styleLabel (timeLabel, kTextDim, 12.0f);
    timeLabel.setText ("0:00 / 0:00", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (timeLabel);
    styleLabel (playerHintLabel, kTextDim, 12.0f);
    addAndMakeVisible (playerHintLabel);
    gainSlider.setRange (0.0, 1.0, 0.01);
    gainSlider.setValue (0.8);
    gainSlider.onValueChange = [this] { owner.setPreviewGain (static_cast<float> (gainSlider.getValue())); };
    addAndMakeVisible (gainSlider);
    styleLabel (gainLabel, kTextDim, 12.0f);
    gainLabel.setText ("Volume", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (gainLabel);

    updateEmptyState();
}

LibraryTab::~LibraryTab() = default;

void LibraryTab::setFiles (const std::vector<CloudFile>& files,
                           const std::vector<juce::String>& localFileIds)
{
    allFiles = files;
    localIds = localFileIds;
    applySearchFilter();
}

void LibraryTab::applySearchFilter()
{
    const juce::String query = searchBox.getText().trim().toLowerCase();
    filteredFiles.clear();
    for (const auto& f : allFiles)
        if (query.isEmpty() || f.name.toLowerCase().contains (query))
            filteredFiles.push_back (f);
    fileList.updateContent();
    fileList.repaint();
    updateEmptyState();
}

void LibraryTab::updateEmptyState()
{
    const bool libraryEmpty = allFiles.empty();
    const bool searchEmpty = ! libraryEmpty && filteredFiles.empty();
    emptyLabel.setVisible (libraryEmpty || searchEmpty);
    emptyCaptureButton.setVisible (libraryEmpty);
    if (libraryEmpty)
        emptyLabel.setText ("No sounds yet", juce::NotificationType::dontSendNotification);
    else if (searchEmpty)
        emptyLabel.setText ("No sounds match your search.", juce::NotificationType::dontSendNotification);
    fileList.setVisible (! libraryEmpty);
}

void LibraryTab::textEditorTextChanged (juce::TextEditor&)
{
    applySearchFilter();
}

void LibraryTab::setSelected (const CloudFile& file, bool localExists)
{
    selectedId = file.id;
    (void) localExists;
    fileList.updateContent();
    fileList.repaint();
}

void LibraryTab::clearSelection()
{
    selectedId.clear();
    playingId.clear();
    fileList.deselectAllRows();
    setPlayer ("No sound selected", juce::File {}, false, -1.0);
}

void LibraryTab::setDownloadProgress (const juce::String& fileId, double progress, bool isDownloading)
{
    if (isDownloading)
    {
        downloading[fileId] = true;
        downloadProgress[fileId] = progress;
    }
    else
    {
        downloading.erase (fileId);
        downloadProgress.erase (fileId);
    }
    fileList.updateContent();
    fileList.repaint();
}

void LibraryTab::setPlaying (const juce::String& fileId, bool playing)
{
    playingId = playing ? fileId : juce::String {};
    fileList.updateContent();
    fileList.repaint();
}

void LibraryTab::setMessage (const juce::String& text)
{
    messageLabel.setText (text, juce::NotificationType::dontSendNotification);
}

void LibraryTab::setError (const juce::String& text)
{
    errorLabel.setText (text, juce::NotificationType::dontSendNotification);
    if (text.isNotEmpty())
    {
        DBG ("Onorab UI error: " << text);
    }
}

void LibraryTab::setEmptyCaptureButtonText (const juce::String& text)
{
    emptyCaptureButton.setButtonText (text);
}

void LibraryTab::setPlayer (const juce::String& name, const juce::File& localFile,
                            bool localExists, double durationSeconds)
{
    playerNameLabel.setText (name, juce::NotificationType::dontSendNotification);
    if (name == "No sound selected")
    {
        waveform.clear();
        playerHintLabel.setText ("Select a sound to preview it.",
                                 juce::NotificationType::dontSendNotification);
    }
    else if (localExists)
    {
        waveform.setFile (localFile);
        playerHintLabel.setText ("Drag the handle on this row into the FL Playlist.",
                                 juce::NotificationType::dontSendNotification);
    }
    else
    {
        waveform.setFile (localFile); // Placeholder when the file is not local yet.
        playerHintLabel.setText ("Press Download on this sound to fetch it to this computer.",
                                 juce::NotificationType::dontSendNotification);
    }
    timeLabel.setText ("0:00 / " + formatDuration (durationSeconds),
                       juce::NotificationType::dontSendNotification);
}

void LibraryTab::setPlayerPosition (double currentSeconds, double totalSeconds)
{
    timeLabel.setText (formatDuration (currentSeconds) + " / " + formatDuration (totalSeconds),
                       juce::NotificationType::dontSendNotification);
}

void LibraryTab::setPlayerPlaying (bool playing)
{
    playerPlayButton.setPauseIcon (playing);
}

void LibraryTab::resized()
{
    auto area = getLocalBounds().reduced (12);
    auto topRow = area.removeFromTop (30);
    searchBox.setBounds (topRow.removeFromLeft (topRow.getWidth() - 106));
    topRow.removeFromLeft (8);
    refreshButton.setBounds (topRow.removeFromLeft (90));
    area.removeFromTop (6);
    auto actionRow = area.removeFromTop (28);
    deleteButton.setBounds (actionRow.removeFromLeft (110));
    actionRow.removeFromLeft (8);
    messageLabel.setBounds (actionRow);
    area.removeFromTop (4);
    errorLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (6);

    // Player card at the bottom; the list takes the rest.
    auto playerArea = area.removeFromBottom (168);
    playerNameLabel.setBounds (playerArea.removeFromTop (20));
    playerArea.removeFromTop (4);
    waveform.setBounds (playerArea.removeFromTop (84));
    playerArea.removeFromTop (6);
    auto transportRow = playerArea.removeFromTop (28);
    playerPlayButton.setBounds (transportRow.removeFromLeft (28));
    transportRow.removeFromLeft (8);
    playerStopButton.setBounds (transportRow.removeFromLeft (28));
    transportRow.removeFromLeft (12);
    timeLabel.setBounds (transportRow.removeFromLeft (110));
    transportRow.removeFromLeft (12);
    gainLabel.setBounds (transportRow.removeFromLeft (50));
    gainSlider.setBounds (transportRow.removeFromLeft (120));
    playerArea.removeFromTop (4);
    playerHintLabel.setBounds (playerArea);

    fileList.setBounds (area);
    // Empty state overlays the list area.
    auto emptyArea = area;
    emptyLabel.setBounds (emptyArea.removeFromTop (24));
    auto buttonRow = emptyArea.removeFromTop (32);
    buttonRow.removeFromLeft ((buttonRow.getWidth() - 170) / 2);
    emptyCaptureButton.setBounds (buttonRow.removeFromLeft (170));
}

int LibraryTab::getNumRows()
{
    return static_cast<int> (filteredFiles.size());
}

void LibraryTab::paintListBoxItem (int, juce::Graphics&, int, int, bool)
{
    // Rows are custom components (see refreshComponentForRow); nothing
    // to paint here.
}

juce::Component* LibraryTab::refreshComponentForRow (int rowNumber, bool isRowSelected,
                                                     juce::Component* existingComponentToUpdate)
{
    auto* row = dynamic_cast<LibraryRowComponent*> (existingComponentToUpdate);
    if (row == nullptr)
        row = new LibraryRowComponent();
    if (rowNumber < 0 || rowNumber >= static_cast<int> (filteredFiles.size()))
        return row;
    const CloudFile file = filteredFiles[static_cast<size_t> (rowNumber)];
    const bool isLocal = std::find (localIds.begin(), localIds.end(), file.id) != localIds.end();
    const auto dl = downloading.find (file.id);
    const auto pr = downloadProgress.find (file.id);
    LibraryRowComponent::State state;
    state.file = file;
    state.isLocal = isLocal;
    state.isPlaying = (playingId == file.id);
    state.isSelected = isRowSelected || (selectedId == file.id);
    state.downloading = (dl != downloading.end() && dl->second);
    state.progress = (pr != downloadProgress.end() ? pr->second : -1.0);
    state.onPlay = [this, file] { owner.playRowFile (file); };
    state.onDownload = [this, file] { owner.downloadFile (file); };
    state.getDragFiles = [this, file, isLocal]
    {
        if (! isLocal)
            return juce::StringArray {};
        const juce::File local = owner.getCacheDir()
                                     .getChildFile (HubClient::sanitiseFileName (file.name));
        if (local.existsAsFile())
            return juce::StringArray { local.getFullPathName() };
        return juce::StringArray {};
    };
    row->update (state);
    return row;
}

void LibraryTab::selectedRowsChanged (int lastRowSelected)
{
    if (lastRowSelected >= 0 && lastRowSelected < static_cast<int> (filteredFiles.size()))
        owner.onFileSelected (filteredFiles[static_cast<size_t> (lastRowSelected)]);
}

// ============================== ProjectsTab ==============================

ProjectsTab::ProjectsTab (OnorabAudioProcessorEditor& editorIn)
    : owner (editorIn)
{
    styleLabel (titleLabel, kText, 14.0f);
    titleLabel.setText ("Projects", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (titleLabel);
    styleLabel (emptyLabel, kTextDim, 14.0f);
    emptyLabel.setJustificationType (juce::Justification::centred);
    emptyLabel.setText ("No projects yet", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (emptyLabel);

    projectList.setRowHeight (30);
    projectList.setMultipleSelectionEnabled (false);
    addAndMakeVisible (projectList);

    styleButton (backButton, kPurple);
    backButton.onClick = [this] { openProject.clear(); detailList.deselectAllRows(); updateView(); };
    addAndMakeVisible (backButton);
    styleLabel (detailTitleLabel, kText, 15.0f);
    addAndMakeVisible (detailTitleLabel);
    styleSearchBox (detailSearchBox, "Search sounds in this project...");
    detailSearchBox.addListener (this);
    addAndMakeVisible (detailSearchBox);
    detailList.setRowHeight (26);
    detailList.setMultipleSelectionEnabled (true); // Push / Delete one or more sounds.
    addAndMakeVisible (detailList);

    // No laptop file picker here by design (2026-11-04): a project's
    // sounds come from audio routed to this plugin's Mixer Track.
    // Auto-capture records that audio into this project in takes;
    // the header's Capture from FL does the same manually.
    stylePrimaryButton (autoCaptureButton);
    autoCaptureButton.onClick = [this] { owner.toggleProjectAutoCapture (openProject); };
    addAndMakeVisible (autoCaptureButton);
    styleLabel (busHintLabel, kTextDim, 12.0f);
    busHintLabel.setText ("Route a Mixer Track to this plugin's Mixer Track, then turn "
                          "Auto-capture on: every take played through it is recorded "
                          "and uploaded to this project automatically.",
                          juce::NotificationType::dontSendNotification);
    addAndMakeVisible (busHintLabel);
    styleButton (inviteButton, kPurple);
    inviteButton.onClick = [this]
    {
        invitePanelVisible = true;
        inviteProjectLabel.setText ("Invite to project: " + openProject,
                                    juce::NotificationType::dontSendNotification);
        inviteEmailBox.setText ("");
        inviteStatusLabel.setText ("", juce::NotificationType::dontSendNotification);
        updateView();
    };
    addAndMakeVisible (inviteButton);
    styleButton (pushButton, kGreen);
    pushButton.onClick = [this] { owner.pushProjectFiles (getDetailSelectedFiles(), openProject); };
    addAndMakeVisible (pushButton);
    styleButton (deleteButton, kRed);
    deleteButton.onClick = [this] { owner.deleteHubFiles (getDetailSelectedFiles()); };
    addAndMakeVisible (deleteButton);

    styleLabel (statusLabel, kTextDim, 12.0f);
    addAndMakeVisible (statusLabel);

    // Invite panel (hidden until Invite is pressed).
    styleLabel (inviteProjectLabel, kText, 13.0f);
    addAndMakeVisible (inviteProjectLabel);
    styleTextBox (inviteEmailBox, "Invited Gmail, e.g. friend@gmail.com");
    addAndMakeVisible (inviteEmailBox);
    stylePrimaryButton (inviteSendButton);
    inviteSendButton.onClick = [this]
    {
        owner.inviteToProject (inviteEmailBox.getText().trim(), openProject);
    };
    addAndMakeVisible (inviteSendButton);
    styleButton (inviteCancelButton);
    inviteCancelButton.onClick = [this] { invitePanelVisible = false; updateView(); };
    addAndMakeVisible (inviteCancelButton);
    styleLabel (inviteStatusLabel, kTextDim, 12.0f);
    addAndMakeVisible (inviteStatusLabel);

    updateView();
}

void ProjectsTab::setFiles (const std::vector<CloudFile>& files)
{
    allFiles = files;
    projectNames.clear();
    for (const auto& f : allFiles)
    {
        if (f.projectId.isEmpty())
            continue;
        if (std::find (projectNames.begin(), projectNames.end(), f.projectId) == projectNames.end())
            projectNames.push_back (f.projectId);
    }
    applyDetailFilter();
    updateView();
}

void ProjectsTab::applyDetailFilter()
{
    const juce::String query = detailSearchBox.getText().trim().toLowerCase();
    detailFiles.clear();
    for (const auto& f : allFiles)
    {
        if (f.projectId != openProject)
            continue;
        if (query.isNotEmpty() && ! f.name.toLowerCase().contains (query))
            continue;
        detailFiles.push_back (f);
    }
    detailList.updateContent();
    detailList.repaint();
    projectList.updateContent();
    projectList.repaint();
}

void ProjectsTab::textEditorTextChanged (juce::TextEditor&)
{
    applyDetailFilter();
}

void ProjectsTab::updateView()
{
    const bool detail = openProject.isNotEmpty();
    titleLabel.setVisible (! detail);
    projectList.setVisible (! detail);
    emptyLabel.setVisible (! detail && projectNames.empty());
    backButton.setVisible (detail);
    detailTitleLabel.setVisible (detail);
    detailSearchBox.setVisible (detail);
    detailList.setVisible (detail);
    autoCaptureButton.setVisible (detail);
    busHintLabel.setVisible (detail);
    if (detail)
        setAutoCaptureState (owner.getAutoCaptureProject() == openProject);
    inviteButton.setVisible (detail);
    pushButton.setVisible (detail);
    deleteButton.setVisible (detail);
    inviteProjectLabel.setVisible (detail && invitePanelVisible);
    inviteEmailBox.setVisible (detail && invitePanelVisible);
    inviteSendButton.setVisible (detail && invitePanelVisible);
    inviteCancelButton.setVisible (detail && invitePanelVisible);
    inviteStatusLabel.setVisible (detail && invitePanelVisible);
    if (detail)
    {
        detailTitleLabel.setText (openProject, juce::NotificationType::dontSendNotification);
        applyDetailFilter();
    }
    resized();
}

void ProjectsTab::setAutoCaptureState (bool on)
{
    autoCaptureButton.setButtonText (on ? "Auto-capture: On" : "Auto-capture: Off");
}

void ProjectsTab::setStatus (const juce::String& text, bool isError)
{
    statusLabel.setText (text, juce::NotificationType::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, isError ? kRed : kTextDim);
}

void ProjectsTab::setInviteStatus (const juce::String& text)
{
    inviteStatusLabel.setText (text, juce::NotificationType::dontSendNotification);
}

std::vector<CloudFile> ProjectsTab::getDetailSelectedFiles() const
{
    std::vector<CloudFile> out;
    // juce::SparseSet has no begin()/end() iterators, so a range-for
    // over getSelectedRows() does not compile (MSVC C3312). Index it:
    // rows[i] is the i-th selected row, rows.size() the count.
    const juce::SparseSet<int> rows = detailList.getSelectedRows();
    for (int i = 0; i < rows.size(); ++i)
    {
        const int row = rows[i];
        if (row >= 0 && row < static_cast<int> (detailFiles.size()))
            out.push_back (detailFiles[static_cast<size_t> (row)]);
    }
    return out;
}

int ProjectsTab::getNumRows()
{
    // One model serves both lists. Only the visible list is laid out
    // by JUCE, so returning the visible list's count is correct; the
    // hidden list's rows are never painted.
    if (openProject.isNotEmpty())
        return static_cast<int> (detailFiles.size());
    return static_cast<int> (projectNames.size());
}

void ProjectsTab::paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height,
                                    bool rowIsSelected)
{
    if (rowIsSelected)
        g.fillAll (kSelected);
    g.setColour (kText);
    g.setFont (13.0f);
    if (openProject.isNotEmpty())
    {
        if (rowNumber < 0 || rowNumber >= static_cast<int> (detailFiles.size()))
            return;
        const auto& file = detailFiles[static_cast<size_t> (rowNumber)];
        g.drawText (file.name, 6, 0, width - 110, height, juce::Justification::left, true);
        g.setColour (kTextDim);
        g.drawText (formatBytes (file.sizeBytes), width - 100, 0, 94, height,
                    juce::Justification::right, true);
        return;
    }
    if (rowNumber < 0 || rowNumber >= static_cast<int> (projectNames.size()))
        return;
    const juce::String project = projectNames[static_cast<size_t> (rowNumber)];
    int count = 0;
    for (const auto& f : allFiles)
        if (f.projectId == project)
            ++count;
    g.drawText (project, 6, 0, width - 110, height, juce::Justification::left, true);
    g.setColour (kTextDim);
    g.drawText (juce::String (count) + (count == 1 ? " sound" : " sounds"),
                width - 100, 0, 94, height, juce::Justification::right, true);
}

void ProjectsTab::selectedRowsChanged (int lastRowSelected)
{
    if (openProject.isEmpty())
    {
        // Project list click: open the project detail view.
        if (lastRowSelected >= 0 && lastRowSelected < static_cast<int> (projectNames.size()))
        {
            openProject = projectNames[static_cast<size_t> (lastRowSelected)];
            projectList.deselectAllRows();
            updateView();
        }
        return;
    }
    // Detail list click: select a sound (preview follows the Library
    // selection, so nothing extra happens here).
    if (lastRowSelected >= 0 && lastRowSelected < static_cast<int> (detailFiles.size()))
        owner.onFileSelected (detailFiles[static_cast<size_t> (lastRowSelected)]);
}

void ProjectsTab::resized()
{
    auto area = getLocalBounds().reduced (12);
    if (openProject.isEmpty())
    {
        titleLabel.setBounds (area.removeFromTop (22));
        area.removeFromTop (4);
        auto listArea = area.removeFromTop (area.getHeight() - 24);
        projectList.setBounds (listArea);
        emptyLabel.setBounds (listArea);
        statusLabel.setBounds (area);
        return;
    }
    auto headerRow = area.removeFromTop (30);
    backButton.setBounds (headerRow.removeFromLeft (110));
    headerRow.removeFromLeft (8);
    detailTitleLabel.setBounds (headerRow);
    area.removeFromTop (6);
    detailSearchBox.setBounds (area.removeFromTop (28));
    area.removeFromTop (6);
    auto buttonRow = area.removeFromTop (30);
    autoCaptureButton.setBounds (buttonRow.removeFromLeft (150));
    buttonRow.removeFromLeft (6);
    inviteButton.setBounds (buttonRow.removeFromLeft (90));
    buttonRow.removeFromLeft (6);
    pushButton.setBounds (buttonRow.removeFromLeft (90));
    buttonRow.removeFromLeft (6);
    deleteButton.setBounds (buttonRow.removeFromLeft (110));
    area.removeFromTop (6);
    busHintLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (6);
    if (invitePanelVisible)
    {
        inviteProjectLabel.setBounds (area.removeFromTop (20));
        area.removeFromTop (4);
        inviteEmailBox.setBounds (area.removeFromTop (28));
        area.removeFromTop (4);
        auto inviteRow = area.removeFromTop (30);
        inviteSendButton.setBounds (inviteRow.removeFromLeft (130));
        inviteRow.removeFromLeft (6);
        inviteCancelButton.setBounds (inviteRow.removeFromLeft (90));
        area.removeFromTop (4);
        inviteStatusLabel.setBounds (area.removeFromTop (18));
        area.removeFromTop (6);
    }
    detailList.setBounds (area.removeFromTop (area.getHeight() - 24));
    area.removeFromTop (6);
    statusLabel.setBounds (area);
}

// ============================== TransfersTab ==============================

TransfersTab::TransfersTab (OnorabAudioProcessorEditor& editorIn)
    : owner (editorIn)
{
    styleLabel (noteLabel, kTextDim, 12.0f);
    noteLabel.setText ("Transfers you start here appear in this list with their real progress.",
                       juce::NotificationType::dontSendNotification);
    addAndMakeVisible (noteLabel);
    styleLabel (emptyLabel, kTextDim, 14.0f);
    emptyLabel.setJustificationType (juce::Justification::centred);
    emptyLabel.setText ("No transfers yet", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (emptyLabel);
    transferList.setRowHeight (44);
    addAndMakeVisible (transferList);
}

void TransfersTab::setRows (const std::vector<TransferRow>& newRows)
{
    rows = newRows;
    transferList.updateContent();
    transferList.repaint();
    emptyLabel.setVisible (rows.empty());
    transferList.setVisible (! rows.empty());
}

void TransfersTab::setNote (const juce::String& text)
{
    noteLabel.setText (text, juce::NotificationType::dontSendNotification);
}

void TransfersTab::resized()
{
    auto area = getLocalBounds().reduced (12);
    noteLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (6);
    transferList.setBounds (area);
    emptyLabel.setBounds (area);
}

int TransfersTab::getNumRows()
{
    return static_cast<int> (rows.size());
}

void TransfersTab::paintListBoxItem (int, juce::Graphics&, int, int, bool)
{
    // Rows are custom components (see refreshComponentForRow).
}

juce::Component* TransfersTab::refreshComponentForRow (int rowNumber, bool,
                                                       juce::Component* existingComponentToUpdate)
{
    auto* row = dynamic_cast<TransferRowComponent*> (existingComponentToUpdate);
    if (row == nullptr)
        row = new TransferRowComponent();
    if (rowNumber < 0 || rowNumber >= static_cast<int> (rows.size()))
        return row;
    const TransferRow data = rows[static_cast<size_t> (rowNumber)];
    std::function<void()> retry;
    if (data.canRetry)
        retry = [this, data] { owner.retryTransfer (data.entry); };
    row->update (data, std::move (retry));
    return row;
}

// ============================== SettingsTab ==============================

SettingsTab::SettingsTab (OnorabAudioProcessorEditor& editorIn)
    : owner (editorIn)
{
    styleLabel (driveTitleLabel, kText, 15.0f);
    driveTitleLabel.setText ("Google Drive", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (driveTitleLabel);
    driveDot.setDotColour (kGrey);
    addAndMakeVisible (driveDot);
    styleLabel (driveStatusLabel, kTextDim, 13.0f);
    driveStatusLabel.setText ("Not connected", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (driveStatusLabel);
    // No email field here by design (2026-11-04): the account is
    // chosen in the browser at sign-in and reported back by the Hub,
    // so the user never types their Gmail into the plugin — and never
    // re-types it when the window reopens, because the Hub's stored
    // token restores the connection on its own.
    styleLabel (driveAccountLabel, kTextDim, 13.0f);
    addAndMakeVisible (driveAccountLabel);
    stylePrimaryButton (connectDriveButton);
    connectDriveButton.onClick = [this] { owner.connectDrive(); };
    addAndMakeVisible (connectDriveButton);
    styleButton (disconnectDriveButton, kRed);
    disconnectDriveButton.onClick = [this] { owner.disconnectDrive(); };
    // Hidden until the first Hub status poll confirms a connection
    // (setDriveConnected owns visibility after that).
    disconnectDriveButton.setVisible (false);
    addAndMakeVisible (disconnectDriveButton);
    styleLabel (driveMessageLabel, kTextDim, 12.0f);
    addAndMakeVisible (driveMessageLabel);

    styleButton (advancedButton, kPurple);
    advancedButton.onClick = [this]
    {
        advancedVisible = ! advancedVisible;
        advancedButton.setButtonText (advancedVisible ? "Advanced ^" : "Advanced v");
        updateAdvancedVisibility();
        resized();
    };
    addAndMakeVisible (advancedButton);

    styleLabel (hubUrlLabel, kText, 13.0f);
    hubUrlLabel.setText ("Hub URL", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (hubUrlLabel);
    styleTextBox (hubUrlBox, "http://127.0.0.1:18741");
    addAndMakeVisible (hubUrlBox);
    styleLabel (cacheDirLabel, kText, 13.0f);
    cacheDirLabel.setText ("Cache folder", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (cacheDirLabel);
    styleTextBox (cacheDirBox, "");
    addAndMakeVisible (cacheDirBox);
    styleButton (saveButton, kGreen);
    saveButton.onClick = [this]
    {
        owner.applySettings (hubUrlBox.getText().trim(),
                             juce::File (cacheDirBox.getText().trim()));
    };
    addAndMakeVisible (saveButton);
    styleButton (browseButton);
    browseButton.onClick = [this]
    {
        dirChooser = std::make_unique<juce::FileChooser> ("Choose the Onorab cache folder",
                                                          juce::File (cacheDirBox.getText().trim()));
        dirChooser->launchAsync (juce::FileBrowserComponent::openMode
                                     | juce::FileBrowserComponent::canSelectDirectories,
                                 [this] (const juce::FileChooser& chooser)
                                 {
                                     if (const juce::File d = chooser.getResult(); d.isDirectory())
                                         cacheDirBox.setText (d.getFullPathName());
                                 });
    };
    addAndMakeVisible (browseButton);
    styleLabel (savedLabel, kGreen, 12.0f);
    addAndMakeVisible (savedLabel);

    styleLabel (noteLabel, kTextDim, 12.0f);
    noteLabel.setJustificationType (juce::Justification::topLeft);
    noteLabel.setText ("Notes: the Hub runs on localhost only. Google Drive OAuth lives in "
                       "the Hub, not in this plugin, and no tokens are ever saved in an "
                       "FL Studio project. Default cache on Windows: "
                       "%LOCALAPPDATA%\\Onorab\\cache (override with ONORAB_CACHE_DIR). "
                       "Mock Drive mode is a Hub-side setting.",
                       juce::NotificationType::dontSendNotification);
    addAndMakeVisible (noteLabel);

    updateAdvancedVisibility();
}

void SettingsTab::updateAdvancedVisibility()
{
    hubUrlLabel.setVisible (advancedVisible);
    hubUrlBox.setVisible (advancedVisible);
    cacheDirLabel.setVisible (advancedVisible);
    cacheDirBox.setVisible (advancedVisible);
    saveButton.setVisible (advancedVisible);
    browseButton.setVisible (advancedVisible);
    savedLabel.setVisible (advancedVisible);
}

void SettingsTab::setValues (const juce::String& hubUrl, const juce::String& cacheDir)
{
    hubUrlBox.setText (hubUrl);
    cacheDirBox.setText (cacheDir);
}

void SettingsTab::setStatus (const juce::String& text)
{
    savedLabel.setText (text, juce::NotificationType::dontSendNotification);
}

void SettingsTab::setDriveConnected (bool connected, const juce::String& account)
{
    driveDot.setDotColour (connected ? kGreen : kGrey);
    driveStatusLabel.setText (connected ? "Drive connected" : "Not connected",
                              juce::NotificationType::dontSendNotification);
    driveStatusLabel.setColour (juce::Label::textColourId, connected ? kGreen : kTextDim);
    // The connected account is DISPLAYED from the Hub's report; an
    // unknown account still shows "Drive connected" (the connection
    // itself is the fact that matters, not the email string).
    driveAccountLabel.setText (connected
                                   ? (account.isNotEmpty() ? ("Connected as " + account)
                                                          : "Connected - account details unavailable")
                                   : "",
                               juce::NotificationType::dontSendNotification);
    connectDriveButton.setVisible (! connected);
    disconnectDriveButton.setVisible (connected);
}

// The Connect button doubles as "Try again" after a failed sign-in
// (refreshDriveStatus sets it); starting a fresh attempt resets it.
void SettingsTab::setConnectButtonText (const juce::String& text)
{
    connectDriveButton.setButtonText (text);
}

void SettingsTab::setDriveMessage (const juce::String& text)
{
    driveMessageLabel.setText (text, juce::NotificationType::dontSendNotification);
}

void SettingsTab::resized()
{
    auto area = getLocalBounds().reduced (12);
    driveTitleLabel.setBounds (area.removeFromTop (22));
    area.removeFromTop (4);
    auto statusRow = area.removeFromTop (22);
    driveDot.setBounds (statusRow.removeFromLeft (22));
    driveStatusLabel.setBounds (statusRow);
    area.removeFromTop (6);
    driveAccountLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (6);
    auto connectRow = area.removeFromTop (30);
    connectDriveButton.setBounds (connectRow.removeFromLeft (200));
    connectRow.removeFromLeft (8);
    disconnectDriveButton.setBounds (connectRow.removeFromLeft (140));
    area.removeFromTop (4);
    driveMessageLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (10);
    advancedButton.setBounds (area.removeFromTop (30));
    area.removeFromTop (6);
    if (advancedVisible)
    {
        hubUrlLabel.setBounds (area.removeFromTop (20));
        hubUrlBox.setBounds (area.removeFromTop (28));
        area.removeFromTop (6);
        cacheDirLabel.setBounds (area.removeFromTop (20));
        cacheDirBox.setBounds (area.removeFromTop (28));
        area.removeFromTop (6);
        auto saveRow = area.removeFromTop (30);
        saveButton.setBounds (saveRow.removeFromLeft (90));
        saveRow.removeFromLeft (6);
        browseButton.setBounds (saveRow.removeFromLeft (110));
        area.removeFromTop (4);
        savedLabel.setBounds (area.removeFromTop (18));
        area.removeFromTop (6);
    }
    noteLabel.setBounds (area);
}

// ============================== CaptureReviewPanel ==============================

CaptureReviewPanel::CaptureReviewPanel (juce::AudioFormatManager& formatManagerToUse,
                                        std::function<void (const juce::String&)> onSend,
                                        std::function<void()> onClose,
                                        std::function<void()> onDiscard)
    : sendCallback (std::move (onSend)),
      closeCallback (std::move (onClose)),
      discardCallback (std::move (onDiscard)),
      waveform (formatManagerToUse)
{
    styleLabel (titleLabel, kText, 15.0f);
    titleLabel.setText ("Capture ready", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (titleLabel);
    addAndMakeVisible (waveform);
    styleTextBox (nameBox, "Capture name");
    addAndMakeVisible (nameBox);
    stylePrimaryButton (sendButton);
    sendButton.onClick = [this] { if (sendCallback != nullptr) sendCallback (nameBox.getText().trim()); };
    addAndMakeVisible (sendButton);
    styleButton (closeButton);
    closeButton.onClick = [this] { if (closeCallback != nullptr) closeCallback(); };
    addAndMakeVisible (closeButton);
    styleButton (discardButton, kRed);
    discardButton.onClick = [this] { if (discardCallback != nullptr) discardCallback(); };
    addAndMakeVisible (discardButton);
    styleLabel (statusLabel, kTextDim, 12.0f);
    addAndMakeVisible (statusLabel);
}

void CaptureReviewPanel::showFor (const juce::File& captureFile)
{
    waveform.setFile (captureFile);
    nameBox.setText (captureFile.getFileNameWithoutExtension());
    statusLabel.setText ("Review the capture, rename it if you like, then send it to Drive.",
                         juce::NotificationType::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, kTextDim);
    sendButton.setEnabled (true);
    sendButton.setButtonText ("Send to Drive");
    setVisible (true);
}

void CaptureReviewPanel::setStatus (const juce::String& text, bool isError)
{
    statusLabel.setText (text, juce::NotificationType::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, isError ? kRed : kTextDim);
}

void CaptureReviewPanel::setUploaded()
{
    statusLabel.setText ("Uploaded", juce::NotificationType::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, kGreen);
    sendButton.setEnabled (false);
    sendButton.setButtonText ("Uploaded");
}

void CaptureReviewPanel::setSending (bool sending)
{
    sendButton.setEnabled (! sending);
    sendButton.setButtonText (sending ? "Sending..." : "Send to Drive");
}

void CaptureReviewPanel::resized()
{
    auto area = getLocalBounds().reduced (14);
    titleLabel.setBounds (area.removeFromTop (22));
    area.removeFromTop (4);
    waveform.setBounds (area.removeFromTop (110));
    area.removeFromTop (6);
    nameBox.setBounds (area.removeFromTop (28));
    area.removeFromTop (6);
    auto buttonRow = area.removeFromTop (30);
    sendButton.setBounds (buttonRow.removeFromLeft (150));
    buttonRow.removeFromLeft (6);
    closeButton.setBounds (buttonRow.removeFromLeft (90));
    buttonRow.removeFromLeft (6);
    discardButton.setBounds (buttonRow.removeFromLeft (90));
    area.removeFromTop (6);
    statusLabel.setBounds (area);
}

// ============================== Tracks tab ==============================

TracksTab::TracksTab (OnorabAudioProcessorEditor& editor)
    : owner (editor)
{
    styleLabel (titleLabel, kText, 16.0f);
    titleLabel.setText ("Tracks", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (titleLabel);
    styleLabel (sessionLabel, kTextDim, 12.0f);
    addAndMakeVisible (sessionLabel);
    styleLabel (projectLabel, kTextDim, 12.0f);
    projectLabel.setText ("Drive project:", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (projectLabel);
    projectBox.setEditableText (true);
    projectBox.setTextWhenNothingSelected ("Select or type a project...");
    addAndMakeVisible (projectBox);
    stylePrimaryButton (pushButton);
    pushButton.onClick = [this] { owner.onPushSelected(); };
    addAndMakeVisible (pushButton);
    styleButton (finishButton);
    finishButton.onClick = [this] { owner.onPushFinish(); };
    addAndMakeVisible (finishButton);
    styleButton (cancelButton);
    cancelButton.onClick = [this] { owner.onPushCancel(); };
    addAndMakeVisible (cancelButton);
    styleLabel (statusLabel, kTextDim, 12.0f);
    addAndMakeVisible (statusLabel);
    styleLabel (emptyLabel, kTextDim, 13.0f);
    emptyLabel.setText ("Add Onorab to a Mixer insert and choose Send.",
                        juce::NotificationType::dontSendNotification);
    addAndMakeVisible (emptyLabel);
    addAndMakeVisible (list);
}

void TracksTab::setView (const View& view)
{
    // The project list changes only when the Hub file list changes, so
    // rebuilding the ComboBox items on every timer tick would erase
    // text the user just typed. Rebuild only when the items differ,
    // and always restore the current text afterwards.
    juce::StringArray currentItems;
    for (int i = 0; i < projectBox.getNumItems(); ++i)
        currentItems.add (projectBox.getItemText (i));
    juce::StringArray newItems;
    for (const auto& p : view.projects)
        newItems.add (p);
    const juce::String currentText = projectBox.getText();
    if (currentItems != newItems)
    {
        projectBox.clear (juce::NotificationType::dontSendNotification);
        int itemId = 1;
        for (const auto& p : view.projects)
            projectBox.addItem (p, itemId++);
        projectBox.setText (view.selectedProject.isNotEmpty() ? view.selectedProject : currentText,
                            juce::NotificationType::dontSendNotification);
    }
    projectBox.setEnabled (view.projectEnabled);

    pushButton.setButtonText ("Push selected (" + juce::String (view.selectedCount) + ")");
    pushButton.setEnabled (view.pushEnabled);
    finishButton.setVisible (view.showFinish);
    finishButton.setButtonText (view.finishText);
    finishButton.setEnabled (view.finishEnabled);
    cancelButton.setVisible (view.showCancel);
    statusLabel.setText (view.statusText, juce::NotificationType::dontSendNotification);
    sessionLabel.setText (view.sessionText, juce::NotificationType::dontSendNotification);

    rows = view.rows;
    emptyLabel.setVisible (rows.empty());
    list.setVisible (! rows.empty());
    list.updateContent();
    resized();
}

juce::String TracksTab::getSelectedProject() const
{
    return projectBox.getText().trim();
}

void TracksTab::resized()
{
    auto area = getLocalBounds().reduced (12);
    auto top = area.removeFromTop (28);
    titleLabel.setBounds (top.removeFromLeft (120));
    sessionLabel.setBounds (top);
    auto projectRow = area.removeFromTop (30);
    projectLabel.setBounds (projectRow.removeFromLeft (110));
    projectBox.setBounds (projectRow);
    area.removeFromTop (8);
    auto buttonRow = area.removeFromTop (30);
    pushButton.setBounds (buttonRow.removeFromLeft (180));
    buttonRow.removeFromLeft (8);
    finishButton.setBounds (buttonRow.removeFromLeft (150));
    buttonRow.removeFromLeft (8);
    cancelButton.setBounds (buttonRow.removeFromLeft (90));
    area.removeFromTop (8);
    statusLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (4);
    list.setBounds (area);
    emptyLabel.setBounds (area);
}

int TracksTab::getNumRows()
{
    return (int) rows.size();
}

TrackRowComponent::TrackRowComponent()
{
    // Light text on the dark panels: default JUCE ToggleButton/Label
    // text is dark and would stay unreadable even once visible.
    checkBox.setColour (juce::ToggleButton::textColourId, kText);
    checkBox.setColour (juce::ToggleButton::tickColourId, kTextDim);
    addAndMakeVisible (checkBox);
    styleLabel (statusLabel, kTextDim, 12.0f);
    addAndMakeVisible (statusLabel);
    styleButton (retryButton);
    addAndMakeVisible (retryButton);
}

void TracksTab::paintListBoxItem (int, juce::Graphics&, int, int, bool)
{
    // Rows are full components (see refreshComponentForRow).
}

juce::Component* TracksTab::refreshComponentForRow (int rowNumber, bool,
                                                    juce::Component* existingComponentToUpdate)
{
    auto* row = dynamic_cast<TrackRowComponent*> (existingComponentToUpdate);
    if (row == nullptr)
        row = new TrackRowComponent();
    if (rowNumber >= 0 && rowNumber < (int) rows.size())
    {
        const auto& data = rows[(size_t) rowNumber];
        row->update (data.instanceId, data.trackName, data.status, data.selected,
                     data.selectionEnabled, data.canRetry,
                     [this] (const juce::String& id) { owner.toggleTrackSelection (id); },
                     [this] (const juce::String& id) { owner.retryPushUpload (id); });
    }
    return row;
}

// ============================== Editor ==============================

OnorabAudioProcessorEditor::OnorabAudioProcessorEditor (OnorabAudioProcessor& processorIn)
    : AudioProcessorEditor (processorIn),
      audioProcessor (processorIn),
      hub (processorIn.getHubUrl())
{
    setSize (920, 600);

    formatManager.registerBasicFormats();
    readAheadThread.startThread();
    hub.setAliveToken (aliveToken);

    // Persistent header: title, Hub / Drive status, Capture button.
    styleLabel (titleLabel, kText, 20.0f);
    titleLabel.setText ("Onorab", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (titleLabel);
    styleLabel (byLabel, kTextDim, 12.0f);
    byLabel.setText ("by Potorik Track", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (byLabel);
    hubDot.setDotColour (kGrey);
    addAndMakeVisible (hubDot);
    styleLabel (hubStatusLabel, kTextDim, 12.0f);
    hubStatusLabel.setText ("Hub: checking...", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (hubStatusLabel);
    driveDot.setDotColour (kGrey);
    addAndMakeVisible (driveDot);
    styleLabel (driveStatusLabel, kTextDim, 12.0f);
    driveStatusLabel.setText ("Drive: checking...", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (driveStatusLabel);
    styleLabel (captureCounterLabel, kText, 13.0f);
    addAndMakeVisible (captureCounterLabel);
    // The header Capture button was replaced by the Tracks -> Push
    // selected workflow, so it is never added to the header anymore.
    // Manual capture is still fully usable: the Library empty-state
    // button toggles Start / Stop (see its onClick), and setCaptureUI
    // keeps updating its text. The captureButton member stays only so
    // existing setCaptureUI calls keep compiling; it is never visible.
    stylePrimaryButton (captureButton);

    // Tracks is the Manager's main screen, so it is the first tab.
    tracksTab = new TracksTab (*this);
    libraryTab = new LibraryTab (*this);
    projectsTab = new ProjectsTab (*this);
    transfersTab = new TransfersTab (*this);
    settingsTab = new SettingsTab (*this);

    tabbedComponent.addTab ("Tracks", kPanel, tracksTab, true);
    tabbedComponent.addTab ("Library", kPanel, libraryTab, true);
    tabbedComponent.addTab ("Projects", kPanel, projectsTab, true);
    tabbedComponent.addTab ("Transfers", kPanel, transfersTab, true);
    tabbedComponent.addTab ("Settings", kPanel, settingsTab, true);
    addAndMakeVisible (tabbedComponent);

    // Send editor (compact): track name, session, level meter, status.
    // The name box renames the live registry entry, so the Manager
    // list updates without reopening anything.
    styleLabel (sendTitleLabel, kText, 20.0f);
    sendTitleLabel.setText ("Onorab Send", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (sendTitleLabel);
    styleLabel (sendNameLabel, kTextDim, 13.0f);
    sendNameLabel.setText ("Track:", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (sendNameLabel);
    sendNameBox.setText (audioProcessor.getTrackName());
    sendNameBox.onTextChange = [this]
    {
        const juce::String name = sendNameBox.getText().trim();
        if (name.isEmpty())
        {
            sendNameBox.setText (audioProcessor.getTrackName(),
                                 juce::NotificationType::dontSendNotification);
            return;
        }
        audioProcessor.setTrackName (name);
    };
    addAndMakeVisible (sendNameBox);
    styleLabel (sendSessionLabel, kTextDim, 12.0f);
    addAndMakeVisible (sendSessionLabel);
    addAndMakeVisible (sendMeter);
    styleLabel (sendStatusLabel, kText, 13.0f);
    addAndMakeVisible (sendStatusLabel);
    styleLabel (sendHintLabel, kTextDim, 12.0f);
    sendHintLabel.setText ("Close this window - recording continues.",
                           juce::NotificationType::dontSendNotification);
    addAndMakeVisible (sendHintLabel);

    // First-open mode choice: one short sentence per mode, as the
    // spec requires. There is deliberately no mode-switch button
    // afterwards: removing and re-adding the insert changes the mode.
    styleLabel (choiceTitleLabel, kText, 20.0f);
    choiceTitleLabel.setText ("Onorab", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (choiceTitleLabel);
    stylePrimaryButton (choiceSendButton);
    choiceSendButton.onClick = [this] { chooseMode (OnorabAudioProcessor::PluginMode::Send); };
    addAndMakeVisible (choiceSendButton);
    styleLabel (choiceSendHintLabel, kTextDim, 12.0f);
    choiceSendHintLabel.setText ("This insert records its Mixer Track audio for a Push.",
                                 juce::NotificationType::dontSendNotification);
    addAndMakeVisible (choiceSendHintLabel);
    stylePrimaryButton (choiceManagerButton);
    choiceManagerButton.onClick = [this] { chooseMode (OnorabAudioProcessor::PluginMode::Manager); };
    addAndMakeVisible (choiceManagerButton);
    styleLabel (choiceManagerHintLabel, kTextDim, 12.0f);
    choiceManagerHintLabel.setText ("This insert lists Sends and pushes selected tracks to Drive.",
                                    juce::NotificationType::dontSendNotification);
    addAndMakeVisible (choiceManagerHintLabel);
    styleLabel (choiceNoteLabel, kTextDim, 12.0f);
    choiceNoteLabel.setText ("To change mode later, remove and re-add the plugin.",
                             juce::NotificationType::dontSendNotification);
    addAndMakeVisible (choiceNoteLabel);

    captureReview = new CaptureReviewPanel (formatManager,
        [this] (const juce::String& name) { sendCaptureToDrive (name); },
        [this] { closeCaptureReview(); },
        [this] { discardCapture(); });
    addAndMakeVisible (captureReview);
    captureReview->setVisible (false);

    settingsTab->setValues (audioProcessor.getHubUrl(),
                            audioProcessor.getCacheDir().getFullPathName());

    refreshFiles();
    checkHealth();
    refreshTransfers (true);
    refreshDriveStatus();
    refreshProjectsTab();
    // Owns visibility, editor size and the timer rate for the current
    // mode (Manager / Send / first-open choice).
    buildUIForMode();
}

OnorabAudioProcessorEditor::~OnorabAudioProcessorEditor()
{
    // Flip the token FIRST so no queued Hub callback can touch this editor
    // after destruction starts. Then stop capture (joins the writer thread)
    // before any member is destroyed.
    aliveToken->store (false);
    stopTimer();
    audioProcessor.setMeterEnabled (false);
    // Auto-capture owns the same writer thread as manual capture, so
    // stopping it here (before members die) also finalizes its open
    // take. Finished-but-unuploaded takes stay queued in the
    // processor and upload when the editor reopens.
    if (autoCaptureProject.isNotEmpty())
        audioProcessor.stopAutoCapture();
    // A Push recording must NEVER be stopped by closing a window:
    // Sends keep recording with their windows closed, and stopCapture
    // cannot end a Push writer correctly (it never sets its done flag,
    // so joining it would hang). The processor ends Push writers on
    // transport Stop, Finish, Cancel, or instance destruction.
    else if (audioProcessor.isCapturing() && ! audioProcessor.isPushRecording())
        audioProcessor.stopCapture();
    transport.stop();
    transport.setSource (nullptr);
    previewSource.reset();
    readAheadThread.stopThread (500);
}

void OnorabAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);
    g.setColour (kPanel);
    g.fillRect (0, 0, getWidth(), 56);
    g.setColour (kPanel2);
    g.fillRect (0, 56, getWidth(), 1);
}

void OnorabAudioProcessorEditor::resized()
{
    const auto mode = audioProcessor.getMode();
    if (mode == OnorabAudioProcessor::PluginMode::Send)
    {
        auto area = getLocalBounds().reduced (12);
        sendTitleLabel.setBounds (area.removeFromTop (28));
        auto nameRow = area.removeFromTop (28);
        sendNameLabel.setBounds (nameRow.removeFromLeft (60));
        sendNameBox.setBounds (nameRow);
        area.removeFromTop (8);
        sendSessionLabel.setBounds (area.removeFromTop (20));
        area.removeFromTop (8);
        sendMeter.setBounds (area.removeFromTop (18));
        area.removeFromTop (8);
        sendStatusLabel.setBounds (area.removeFromTop (20));
        area.removeFromTop (8);
        sendHintLabel.setBounds (area.removeFromTop (20));
        return;
    }
    if (mode == OnorabAudioProcessor::PluginMode::Undecided)
    {
        auto area = getLocalBounds().reduced (16);
        choiceTitleLabel.setBounds (area.removeFromTop (32));
        area.removeFromTop (8);
        choiceSendButton.setBounds (area.removeFromTop (30));
        choiceSendHintLabel.setBounds (area.removeFromTop (20));
        area.removeFromTop (12);
        choiceManagerButton.setBounds (area.removeFromTop (30));
        choiceManagerHintLabel.setBounds (area.removeFromTop (20));
        area.removeFromTop (16);
        choiceNoteLabel.setBounds (area.removeFromTop (20));
        return;
    }

    auto header = getLocalBounds().removeFromTop (56);
    header.removeFromLeft (12);
    titleLabel.setBounds (header.removeFromLeft (100));
    byLabel.setBounds (header.removeFromLeft (110));
    header.removeFromLeft (8);
    auto hubArea = header.removeFromLeft (170);
    hubDot.setBounds (hubArea.removeFromLeft (20));
    hubStatusLabel.setBounds (hubArea);
    header.removeFromLeft (4);
    auto driveArea = header.removeFromLeft (180);
    driveDot.setBounds (driveArea.removeFromLeft (20));
    driveStatusLabel.setBounds (driveArea);
    // The header Capture button is gone (Tracks workflow); the counter
    // label keeps the remaining header space for manual-capture time.
    auto captureArea = header;
    captureCounterLabel.setBounds (captureArea);

    tabbedComponent.setBounds (0, 57, getWidth(), getHeight() - 57);
    if (captureReview != nullptr && captureReview->isVisible())
    {
        const int w = 660, h = 320;
        captureReview->setBounds ((getWidth() - w) / 2, 57 + (getHeight() - 57 - h) / 2, w, h);
        captureReview->toFront (false);
    }
}

bool OnorabAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isAudioFile (f))
            return true;
    return false;
}

void OnorabAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    // Dropped files are queued for Hub upload one by one. Non-audio files
    // are skipped with a visible message, never silently ignored.
    bool anyAudio = false;
    for (const auto& f : files)
    {
        if (isAudioFile (f))
        {
            anyAudio = true;
            uploadLocalFileInternal (juce::File (f), {}, -1);
        }
    }
    if (! anyAudio)
        setError ("Dropped files were not audio files - nothing was uploaded.");
}

void OnorabAudioProcessorEditor::timerCallback()
{
    ++timerTicks;
    const auto mode = audioProcessor.getMode();
    if (mode == OnorabAudioProcessor::PluginMode::Send)
    {
        // 50 ms meter + status. Decay runs on the message thread: the
        // processor holds the last block's peak, and the display falls
        // towards it instead of flickering to zero between blocks.
        const float peak = audioProcessor.getOutputPeak();
        displayedPeak = juce::jmax (peak, displayedPeak * 0.85f);
        sendMeter.setLevel (displayedPeak);
        juce::String status;
        if (audioProcessor.isPushRecording())
            status = "Recording";
        else if (audioProcessor.isPushPrepared())
            status = "Armed - waiting for Play";
        else if (audioProcessor.hasFinishedPush())
            status = "Recorded";
        else
            status = "Idle";
        sendStatusLabel.setText (status, juce::NotificationType::dontSendNotification);
        sendSessionLabel.setText ("Session: " + audioProcessor.getSessionTag(),
                                  juce::NotificationType::dontSendNotification);
        return;
    }
    if (mode == OnorabAudioProcessor::PluginMode::Undecided)
        return;

    // Manager: the Tracks state machine runs every tick (local only,
    // no network), so Recording / upload progress appears promptly.
    refreshTracksTab();

    // Auto-capture: finished takes (recorded from routed Mixer audio)
    // upload to their project here, on the message thread, one second
    // after each take ends. The processor only records; the editor
    // owns every upload, so Transfers shows them like any upload.
    // Takes are drained FIRST, so a take that finished just as the
    // writer died still uploads instead of being orphaned.
    if (autoCaptureProject.isNotEmpty())
    {
        for (const auto& take : audioProcessor.takeFinishedAutoTakes())
            uploadLocalFileInternal (take.file, take.project, -1);
        if (! audioProcessor.isCapturing())
        {
            // The auto writer died (e.g. it could not create a take
            // file): join its thread object here (a dead-but-unjoined
            // thread must never linger) and never leave the toggle
            // stuck on.
            audioProcessor.stopAutoCapture();
            setError ("Auto-capture stopped unexpectedly for project " + autoCaptureProject
                      + " - turn it on again in Projects to resume.");
            autoCaptureProject.clear();
            if (projectsTab != nullptr)
                projectsTab->setAutoCaptureState (false);
            captureButton.setButtonText ("Capture from FL");
        }
        else
        {
            captureButton.setButtonText ("Auto-capture on");
            captureCounterLabel.setText ("", juce::NotificationType::dontSendNotification);
        }
    }
    // Capture counter + player position update every second.
    if (audioProcessor.isCapturing() && autoCaptureProject.isEmpty())
    {
        const int elapsed = static_cast<int> ((juce::Time::getCurrentTime() - captureStartTime).inSeconds());
        captureCounterLabel.setText (formatDuration (static_cast<double> (elapsed)),
                                     juce::NotificationType::dontSendNotification);
    }
    if (previewSource != nullptr)
    {
        if (libraryTab != nullptr)
            libraryTab->setPlayerPosition (transport.getCurrentPosition(), transport.getLengthInSeconds());
        syncPlayerState();
    }
    // Polling only (Phase 1): health + transfers every 2 seconds,
    // Drive auth status every 10 seconds. While a browser sign-in
    // is in flight (authAwaiting), auth status joins the 2-second
    // poll so "Drive connected" (or the failure + "Try again")
    // appears promptly instead of up to 10 seconds late.
    // Lightweight localhost traffic; a WebSocket /events client
    // replaces it in Phase 2.
    if (timerTicks % 2 == 0)
    {
        checkHealth();
        refreshTransfers (true);
        if (authAwaiting)
            refreshDriveStatus();
    }
    if (timerTicks % 10 == 0)
        refreshDriveStatus();
}

void OnorabAudioProcessorEditor::setError (const juce::String& text)
{
    if (libraryTab != nullptr)
        libraryTab->setError (text);
    if (projectsTab != nullptr)
        projectsTab->setStatus (text, true);
    if (text.isNotEmpty())
    {
        DBG ("Onorab UI error: " << text);
    }
}

void OnorabAudioProcessorEditor::setProgress (const juce::String& text)
{
    if (libraryTab != nullptr)
        libraryTab->setMessage (text);
    if (projectsTab != nullptr)
        projectsTab->setStatus (text, false);
}

void OnorabAudioProcessorEditor::refreshLocalIds()
{
    localIds.clear();
    const juce::File cache = audioProcessor.getCacheDir();
    for (const auto& f : files)
    {
        if (cache.getChildFile (HubClient::sanitiseFileName (f.name)).existsAsFile())
            localIds.push_back (f.id);
    }
    if (hasSelection)
    {
        selectedLocalFile = cache.getChildFile (HubClient::sanitiseFileName (selectedFile.name));
        selectedLocalExists = selectedLocalFile.existsAsFile();
    }
    if (libraryTab != nullptr)
        libraryTab->setFiles (files, localIds);
}

void OnorabAudioProcessorEditor::refreshFiles()
{
    setProgress ("Refreshing Library...");
    hub.listFiles ([this] (bool ok, juce::String error, std::vector<CloudFile> newFiles)
    {
        if (ok)
        {
            // Order rule (user decision 2026-11-03): files arrive in Hub
            // creation order and are NEVER re-sorted here. Library and
            // Projects filters preserve this order, so an invited
            // user's upload appears at the end, in order.
            files = std::move (newFiles);
            refreshLocalIds();
            if (libraryTab != nullptr)
                libraryTab->setError ("");
            setProgress ("Loaded " + juce::String (files.size()) + " file(s) from the Hub.");
            refreshProjectsTab();
        }
        else
        {
            setError ("Could not load the Library: " + error);
            setProgress ("");
        }
    });
}

void OnorabAudioProcessorEditor::onFileSelected (const CloudFile& file)
{
    selectedFile = file;
    hasSelection = true;
    selectedLocalFile = audioProcessor.getCacheDir()
                            .getChildFile (HubClient::sanitiseFileName (file.name));
    selectedLocalExists = selectedLocalFile.existsAsFile();
    selectedDurationSeconds = -1.0;
    if (selectedLocalExists)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (selectedLocalFile));
        if (reader != nullptr && reader->sampleRate > 0)
            selectedDurationSeconds = static_cast<double> (reader->lengthInSamples) / reader->sampleRate;
    }
    if (libraryTab != nullptr)
    {
        libraryTab->setSelected (file, selectedLocalExists);
        libraryTab->setPlayer (file.name, selectedLocalFile, selectedLocalExists, selectedDurationSeconds);
    }
    if (selectedLocalExists)
    {
        loadPreview (selectedLocalFile);
    }
    else
    {
        transport.stop();
        previewSource.reset();
        transport.setSource (nullptr);
        playingFileId.clear();
        syncPlayerState();
        setProgress ("Press Download on \"" + file.name + "\" to fetch it to this computer.");
    }
}

void OnorabAudioProcessorEditor::clearSelection()
{
    hasSelection = false;
    selectedLocalExists = false;
    playingFileId.clear();
    transport.stop();
    previewSource.reset();
    transport.setSource (nullptr);
    if (libraryTab != nullptr)
        libraryTab->clearSelection();
}

void OnorabAudioProcessorEditor::downloadFile (const CloudFile& file)
{
    downloadFileInternal (file, -1);
}

void OnorabAudioProcessorEditor::downloadFileInternal (const CloudFile& file, int reuseEntry)
{
    int entry = reuseEntry;
    if (entry < 0 || entry >= static_cast<int> (localTransfers.size()))
    {
        entry = addLocalTransfer (file.name, "Download");
        localTransfers[static_cast<size_t> (entry)].retry = [this, file] (int idx)
        {
            downloadFileInternal (file, idx);
        };
    }
    else
    {
        updateLocalTransfer (entry, 0.0, "Downloading...", "", false);
    }
    setProgress ("Downloading \"" + file.name + "\"...");
    hub.downloadFile (file, audioProcessor.getCacheDir(),
        [this, file, entry] (bool ok, juce::String error, juce::File localFile)
        {
            if (ok)
            {
                updateLocalTransfer (entry, 1.0, "Done", "", false);
                if (libraryTab != nullptr)
                    libraryTab->setDownloadProgress (file.id, 0.0, false);
                refreshLocalIds();
                if (hasSelection && selectedFile.id == file.id)
                {
                    selectedLocalFile = localFile;
                    selectedLocalExists = true;
                    onFileSelected (file);
                }
                setProgress ("Downloaded: " + file.name + " - Ready for FL");
                if (playAfterDownloadId == file.id)
                {
                    playAfterDownloadId.clear();
                    playRowFile (file);
                }
            }
            else
            {
                updateLocalTransfer (entry, localTransfers[static_cast<size_t> (entry)].progress,
                                     "Failed", error, true);
                if (libraryTab != nullptr)
                    libraryTab->setDownloadProgress (file.id, 0.0, false);
                setError ("Download failed: " + error);
            }
            refreshTransfers (false);
        },
        [this, file, entry] (double progress)
        {
            juce::MessageManager::callAsync ([this, file, entry, progress]
            {
                if (libraryTab != nullptr)
                    libraryTab->setDownloadProgress (file.id, progress, true);
                updateLocalTransfer (entry, progress >= 0 ? progress : 0.0,
                                     "Downloading...", "", false);
            });
        });
}

void OnorabAudioProcessorEditor::playRowFile (const CloudFile& file)
{
    if (! hasSelection || selectedFile.id != file.id)
        onFileSelected (file);
    if (selectedLocalExists)
    {
        if (transport.isPlaying() && playingFileId == file.id)
        {
            pausePreview();
            return;
        }
        if (previewSource == nullptr || playingFileId != file.id)
            loadPreview (selectedLocalFile);
        transport.start();
        playingFileId = file.id;
        syncPlayerState();
        setError ("");
        return;
    }
    // Not local yet: download first, then play automatically.
    playAfterDownloadId = file.id;
    downloadFileInternal (file, -1);
}

void OnorabAudioProcessorEditor::deleteHubFiles (const std::vector<CloudFile>& filesIn)
{
    std::vector<CloudFile> wanted = filesIn;
    if (wanted.empty() && hasSelection)
        wanted.push_back (selectedFile);
    if (wanted.empty())
    {
        setError ("Select a sound first, then Delete.");
        return;
    }

    struct DeleteState
    {
        int total = 0;
        int done = 0;
        int deleted = 0;
        juce::StringArray failed;
        juce::StringArray deletedIds;
    };
    auto state = std::make_shared<DeleteState>();
    state->total = static_cast<int> (wanted.size());
    setProgress ("Deleting " + juce::String (state->total) + " file(s)...");
    for (const auto& file : wanted)
    {
        hub.deleteFile (file, [this, file, state] (bool ok, juce::String error)
        {
            if (ok)
            {
                // Hub success only: now remove the local cache copy too,
                // so the sound is gone from Drive AND from this PC.
                const juce::File local = audioProcessor.getCacheDir()
                                             .getChildFile (HubClient::sanitiseFileName (file.name));
                if (local.existsAsFile())
                    local.deleteFile();
                ++state->deleted;
                state->deletedIds.add (file.id);
            }
            else
            {
                state->failed.add (file.name + ": " + error);
            }
            if (++state->done >= state->total)
            {
                if (state->failed.isEmpty())
                {
                    setProgress ("Deleted " + juce::String (state->deleted)
                                  + " file(s) from Drive and local cache.");
                    setError ("");
                }
                else
                {
                    setError ("Some sounds could not be deleted: " + state->failed.joinIntoString (", "));
                    setProgress ("Deleted " + juce::String (state->deleted)
                                 + " of " + juce::String (state->total) + " file(s).");
                }
                // If the selected sound was deleted, clear the preview
                // instead of pointing at a file that no longer exists.
                if (state->deletedIds.contains (selectedFile.id))
                    clearSelection();
                refreshFiles();
            }
        });
    }
}

void OnorabAudioProcessorEditor::uploadLocalFile (const juce::File& file)
{
    uploadLocalFileInternal (file, {}, -1);
}

void OnorabAudioProcessorEditor::uploadLocalFile (const juce::File& file, const juce::String& project)
{
    uploadLocalFileInternal (file, project, -1);
}

int OnorabAudioProcessorEditor::uploadLocalFileInternal (const juce::File& file,
                                                          const juce::String& project, int reuseEntry)
{
    if (! file.existsAsFile())
    {
        setError ("Upload failed: file does not exist: " + file.getFullPathName());
        return -1;
    }
    int entry = reuseEntry;
    if (entry < 0 || entry >= static_cast<int> (localTransfers.size()))
    {
        entry = addLocalTransfer (file.getFileName(), "Upload");
        localTransfers[static_cast<size_t> (entry)].retry = [this, file, project] (int idx)
        {
            uploadLocalFileInternal (file, project, idx);
        };
    }
    else
    {
        updateLocalTransfer (entry, 0.0, "Uploading...", "", false);
    }
    const juce::String folder = project.isNotEmpty() ? (" to " + HubClient::projectFolder (project)) : juce::String {};
    setProgress ("Uploading \"" + file.getFileName() + "\"" + folder + "...");
    hub.uploadFile (file,
        [this, file, folder, entry] (bool ok, juce::String error)
        {
            if (ok)
            {
                updateLocalTransfer (entry, 1.0, "Done", "", false);
                setProgress ("Uploaded: " + file.getFileName() + folder);
                setError ("");
                refreshFiles();
            }
            else
            {
                updateLocalTransfer (entry, localTransfers[static_cast<size_t> (entry)].progress,
                                     "Failed", error, true);
                setError ("Upload failed: " + error);
                setProgress ("");
            }
            // The capture review hook fires only for the capture's own
            // upload, never for an unrelated upload that finished first.
            if (captureUploadHook != nullptr && file == captureUploadTarget)
            {
                auto hook = captureUploadHook;
                captureUploadHook = nullptr;
                hook (ok, error);
            }
            refreshTransfers (false);
        },
        [this, entry] (double progress)
        {
            juce::MessageManager::callAsync ([this, entry, progress]
            {
                // Hub upload progress is coarse (sent / done); the bar
                // reflects the real callback values, never a fake curve.
                updateLocalTransfer (entry, progress >= 0 ? progress : 0.0,
                                     "Uploading...", "", false);
            });
        },
        project);
    return entry;
}

void OnorabAudioProcessorEditor::uploadFilesToProject (const std::vector<juce::File>& files,
                                                       const juce::String& project)
{
    if (project.isEmpty())
    {
        setError ("Open a project first - uploads go to Onorab_projet/<project>/.");
        return;
    }
    if (files.empty())
    {
        setError ("No file chosen - pick one or more audio files to upload to this project.");
        return;
    }
    setProgress ("Uploading " + juce::String (files.size()) + " file(s) to "
                 + HubClient::projectFolder (project) + "...");
    for (const auto& f : files)
        uploadLocalFileInternal (f, project, -1);
}

void OnorabAudioProcessorEditor::pushProjectFiles (const std::vector<CloudFile>& files,
                                                   const juce::String& project)
{
    if (files.empty())
    {
        setError ("Select a song in this project first, then Push.");
        return;
    }
    // Target folder: the open project wins; otherwise the first
    // selected sound's own project (re-pushing it into its own folder
    // is a no-op target, so an empty target is still an error).
    juce::String target = project;
    if (target.isEmpty())
        target = files.front().projectId;
    if (target.isEmpty())
    {
        setError ("Open a project first - Push goes to Onorab_projet/<project>/.");
        return;
    }

    std::vector<juce::File> toPush;
    juce::StringArray skipped;
    for (const auto& f : files)
    {
        const juce::File local = audioProcessor.getCacheDir()
                                     .getChildFile (HubClient::sanitiseFileName (f.name));
        if (local.existsAsFile())
            toPush.push_back (local);
        else
            skipped.add (f.name);
    }
    if (toPush.empty())
    {
        setError ("Nothing to Push - none of the selected sounds is local yet. "
                  "Download them from the Library first, then Push.");
        return;
    }
    juce::String note = "Pushing " + juce::String (toPush.size()) + " file(s) to "
                      + HubClient::projectFolder (target) + "...";
    if (! skipped.isEmpty())
        note += " (" + juce::String (skipped.size()) + " skipped - not local yet.)";
    setProgress (note);
    for (const auto& local : toPush)
        uploadLocalFileInternal (local, target, -1);
}

void OnorabAudioProcessorEditor::retryTransfer (int entry)
{
    if (entry < 0 || entry >= static_cast<int> (localTransfers.size()))
        return;
    const auto retry = localTransfers[static_cast<size_t> (entry)].retry;
    if (retry != nullptr)
        retry (entry);
}

void OnorabAudioProcessorEditor::connectDrive()
{
    if (settingsTab == nullptr)
        return;
    // No email is asked here, ever: the account is chosen in the
    // browser at Google sign-in, and the Hub reports it back for
    // display (refreshDriveStatus). Asking for a typed Gmail only
    // confused the flow, because the typed address never selected
    // anything — the browser account always did.
    settingsTab->setDriveMessage ("Checking Hub Drive status...");
    hub.getAuthStatus ([this] (bool ok, juce::String error, juce::String detail)
    {
        if (settingsTab == nullptr)
            return;
        if (! ok)
        {
            settingsTab->setDriveMessage ("Drive check failed: " + error);
            return;
        }
        if (detail.contains ("authenticated: 1") || detail.contains ("authenticated: true"))
        {
            authAwaiting = false;
            settingsTab->setDriveMessage ("Drive connected.");
            refreshDriveStatus();
            return;
        }
        // Not connected yet: ask the Hub to start auth. The Hub returns
        // a Google sign-in URL; the plugin opens it in the user's
        // browser (Google's own page - the plugin never sees a
        // password). In Mock mode this returns oauth_unavailable,
        // reported honestly, not faked into a login. Commercial note:
        // every PC signs in with its OWN Gmail; the token stays on
        // that PC's Credential Manager, so a buyer never uses the
        // seller's Google account. One completed sign-in persists
        // across Hub restarts and plugin window reopens; only Google's
        // Testing-mode 7-day refresh-token expiry (until the app is
        // verified / Production) can ask for sign-in again.
        hub.startAuth ([this] (bool authOk, juce::String authError, juce::String authUrl)
        {
            if (settingsTab == nullptr)
                return;
            if (! authOk && authError.containsIgnoreCase ("oauth_unavailable"))
            {
                authAwaiting = false;
                settingsTab->setDriveMessage ("Drive is not connected yet - the Hub is in Mock mode. "
                                              "Start the Hub with ONORAB_DRIVE_PROVIDER=drive, then press Connect Drive again.");
            }
            else if (authOk && authUrl.startsWith ("https://"))
            {
                juce::URL (authUrl).launchInDefaultBrowser();
                // From here until the Hub confirms (or reports a
                // failure), the panel shows "Waiting for Google…"
                // and the timer polls auth status every 2 seconds.
                // The browser success page itself now appears only
                // after the Hub exchanged the code AND stored the
                // token, so "Waiting" never turns into a false
                // "connected".
                authAwaiting = true;
                settingsTab->setConnectButtonText ("Connect Google Drive");
                settingsTab->setDriveMessage ("Waiting for Google…");
            }
            else if (authOk)
            {
                authAwaiting = true;
                settingsTab->setConnectButtonText ("Connect Google Drive");
                settingsTab->setDriveMessage ("Waiting for Google…");
            }
            else
            {
                authAwaiting = false;
                settingsTab->setDriveMessage ("Drive connect failed: " + authError);
            }
        });
    });
}

void OnorabAudioProcessorEditor::disconnectDrive()
{
    if (settingsTab == nullptr)
        return;
    settingsTab->setDriveMessage ("Disconnecting Drive on this PC...");
    hub.disconnectDrive ([this] (bool ok, juce::String error)
    {
        if (settingsTab == nullptr)
            return;
        if (ok)
        {
            // A fresh Connect after this must not inherit a stale
            // "Try again" button or a lingering "Waiting" state.
            authAwaiting = false;
            settingsTab->setConnectButtonText ("Connect Google Drive");
            settingsTab->setDriveMessage ("Drive disconnected on this PC.");
            refreshDriveStatus();
        }
        else
        {
            settingsTab->setDriveMessage ("Drive disconnect failed: " + error);
        }
    });
}

void OnorabAudioProcessorEditor::toggleProjectAutoCapture (const juce::String& project)
{
    if (project.isEmpty())
    {
        setError ("Open a project first - auto-capture records into the open project.");
        return;
    }
    if (autoCaptureProject == project)
    {
        // Turning off: the writer finalizes the open take (if any) and
        // the timer uploads it on the next tick, like any finished take.
        audioProcessor.stopAutoCapture();
        autoCaptureProject.clear();
        if (projectsTab != nullptr)
            projectsTab->setAutoCaptureState (false);
        captureButton.setButtonText ("Capture from FL");
        setProgress ("Auto-capture off for project " + project + ".");
        return;
    }
    if (audioProcessor.isCapturing())
    {
        // Either manual Capture is running, or another project's auto
        // mode is. Manual capture must be stopped by its own Stop
        // button first; another project's auto mode is stopped below.
        if (autoCaptureProject.isEmpty())
        {
            setError ("Stop Capture from FL first, then turn auto-capture on.");
            return;
        }
        audioProcessor.stopAutoCapture();
        autoCaptureProject.clear();
    }
    if (! audioProcessor.startAutoCapture (project))
    {
        setError ("Could not start auto-capture for project " + project + ".");
        if (projectsTab != nullptr)
            projectsTab->setAutoCaptureState (false);
        return;
    }
    autoCaptureProject = project;
    if (projectsTab != nullptr)
        projectsTab->setAutoCaptureState (true);
    setProgress ("Auto-capture on for project " + project
                 + " - audio routed to this Mixer Track will appear in this project.");
}

void OnorabAudioProcessorEditor::inviteToProject (const juce::String& email, const juce::String& project)
{
    if (projectsTab == nullptr)
        return;
    if (email.isEmpty() || ! email.containsChar ('@'))
    {
        projectsTab->setInviteStatus ("Enter an invited Gmail address first (it must contain '@').");
        return;
    }
    if (project.isEmpty())
    {
        projectsTab->setInviteStatus ("Enter a project name to invite someone to.");
        return;
    }
    projectsTab->setInviteStatus ("Recording invite for " + email + "...");
    hub.sendInvite (email, project, [this, email, project] (bool ok, juce::String error)
    {
        if (projectsTab == nullptr)
            return;
        if (ok)
            projectsTab->setInviteStatus ("Invite recorded for " + email + " on project " + project
                                          + ". In Drive mode this also creates a real Drive writer permission on "
                                          + HubClient::projectFolder (project)
                                          + "; in Mock mode it is a local record only.");
        else
            projectsTab->setInviteStatus ("Invite failed: " + error);
    });
}

void OnorabAudioProcessorEditor::loadPreview (const juce::File& file)
{
    transport.stop();
    previewSource.reset();
    transport.setSource (nullptr);

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
    {
        setError ("Could not read the preview file (unsupported format?): " + file.getFileName());
        return;
    }
    const double sampleRate = reader->sampleRate;
    previewSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);
    transport.setSource (previewSource.get(), 32768, &readAheadThread, sampleRate);
    transport.setGain (previewGain);
}

void OnorabAudioProcessorEditor::syncPlayerState()
{
    const bool playing = transport.isPlaying();
    if (libraryTab != nullptr)
    {
        libraryTab->setPlayerPlaying (playing);
        libraryTab->setPlaying (playing ? playingFileId : juce::String {}, playing);
    }
}

void OnorabAudioProcessorEditor::playPreview()
{
    if (previewSource == nullptr)
    {
        setError ("No preview loaded - press Download on a sound first.");
        return;
    }
    transport.start();
    syncPlayerState();
    setError ("");
}

void OnorabAudioProcessorEditor::pausePreview()
{
    // stop() pauses without resetting the playhead; Stop resets it.
    transport.stop();
    syncPlayerState();
}

void OnorabAudioProcessorEditor::stopPreview()
{
    transport.stop();
    transport.setPosition (0.0);
    playingFileId.clear();
    syncPlayerState();
    if (libraryTab != nullptr && previewSource != nullptr)
        libraryTab->setPlayerPosition (0.0, transport.getLengthInSeconds());
}

void OnorabAudioProcessorEditor::togglePlayerPlay()
{
    if (transport.isPlaying())
    {
        pausePreview();
        return;
    }
    if (previewSource != nullptr)
    {
        transport.start();
        syncPlayerState();
        return;
    }
    if (hasSelection && selectedLocalExists)
    {
        loadPreview (selectedLocalFile);
        transport.start();
        playingFileId = selectedFile.id;
        syncPlayerState();
        return;
    }
    setError ("No preview loaded - press Download on a sound first.");
}

void OnorabAudioProcessorEditor::setPreviewGain (float gain)
{
    previewGain = gain;
    transport.setGain (gain);
}

void OnorabAudioProcessorEditor::setCaptureUI (bool capturing)
{
    captureButton.setButtonText (capturing ? "Stop" : "Capture from FL");
    // The Library empty-state button is the manual capture toggle now
    // that the header button is gone, so its text must follow too.
    if (libraryTab != nullptr)
        libraryTab->setEmptyCaptureButtonText (capturing ? "Stop" : "Capture from FL");
    captureCounterLabel.setText (capturing ? "0:00" : "",
                                  juce::NotificationType::dontSendNotification);
}

void OnorabAudioProcessorEditor::startCapture()
{
    // Central guard (covers the header button AND the Library empty
    // state button): auto-capture owns the capture writer while it
    // runs, so a manual capture must never steal it.
    if (autoCaptureProject.isNotEmpty())
    {
        setError ("Auto-capture is on for project " + autoCaptureProject
                  + " - turn it off in Projects before using Capture from FL.");
        return;
    }
    if (audioProcessor.isCapturing())
        return;
    const auto now = juce::Time::getCurrentTime();
    const juce::String fileName = "capture-" + now.formatted ("%Y%m%d-%H%M%S") + ".wav";

    // SafePointer: if the editor is destroyed before the queued callback
    // runs, getComponent() returns nullptr and nothing is touched.
    juce::Component::SafePointer<OnorabAudioProcessorEditor> safe (this);
    audioProcessor.startCapture (fileName,
        [safe, fileName] (juce::File file, juce::String error)
        {
            juce::MessageManager::callAsync ([safe, file, error]
            {
                if (auto* editor = safe.getComponent())
                    editor->onCaptureFinished (file, error);
            });
        });
    captureStartTime = now;
    setCaptureUI (true);
    setError ("");
}

void OnorabAudioProcessorEditor::stopCapture()
{
    // A Push writer is never stopped from here: stopCapture cannot set
    // its done flag or reason, so joining it would hang forever. Push
    // writers end via transport Stop, Finish, Cancel, or instance
    // destruction (see the processor).
    if (audioProcessor.isPushRecording() || audioProcessor.isPushPrepared())
        return;
    // stopCapture joins the writer thread, so the finished callback
    // (queued below) is already posted by the time this returns.
    audioProcessor.stopCapture();
}

void OnorabAudioProcessorEditor::onCaptureFinished (const juce::File& file, const juce::String& error)
{
    setCaptureUI (false);
    if (error.isNotEmpty())
    {
        setError ("Capture failed: " + error);
        return;
    }
    captureReviewFile = file;
    captureUploadTarget = file;
    if (captureReview != nullptr)
    {
        captureReview->showFor (file);
        resized();
    }
    setProgress ("Capture ready: " + file.getFileName());
}

void OnorabAudioProcessorEditor::sendCaptureToDrive (const juce::String& name)
{
    if (captureReview == nullptr)
        return;
    if (name.isEmpty())
    {
        captureReview->setStatus ("Enter a name for this capture first.", true);
        return;
    }
    juce::String clean = HubClient::sanitiseFileName (name);
    if (clean.isEmpty())
    {
        captureReview->setStatus ("Enter a valid name (letters, numbers, spaces, - and _).", true);
        return;
    }
    juce::File target = audioProcessor.getCacheDir().getChildFile (clean).withFileExtension ("wav");
    if (target != captureReviewFile && target.existsAsFile())
    {
        captureReview->setStatus ("A file with that name already exists - choose another name.", true);
        return;
    }
    if (target != captureReviewFile)
    {
        if (! captureReviewFile.moveFileTo (target))
        {
            captureReview->setStatus ("Could not rename the capture file.", true);
            return;
        }
        captureReviewFile = target;
    }
    captureUploadTarget = target;
    captureReview->setSending (true);
    captureUploadHook = [this] (bool ok, juce::String error)
    {
        if (captureReview == nullptr || ! captureReview->isVisible())
            return;
        if (ok)
            captureReview->setUploaded();
        else
        {
            captureReview->setSending (false);
            captureReview->setStatus ("Upload failed: " + error, true);
        }
    };
    // Destination: the open project wins (the routed track's audio
    // belongs to the project the user is looking at); otherwise the
    // general Library. This is what makes a manual capture land in a
    // project without any file picker.
    const juce::String project = projectsTab != nullptr ? projectsTab->getOpenProject() : juce::String {};
    captureReview->setStatus ("Sending to " + (project.isEmpty() ? juce::String ("Library")
                                                                : ("project " + project)) + "...", false);
    uploadLocalFileInternal (target, project, -1);
}

void OnorabAudioProcessorEditor::closeCaptureReview()
{
    // The capture file stays in the local cache. (There is no longer
    // a Library Upload button to re-send it from — the review panel's
    // Send to Drive is the way a capture reaches Drive.)
    if (captureReview != nullptr)
        captureReview->setVisible (false);
}

void OnorabAudioProcessorEditor::discardCapture()
{
    if (captureReviewFile.existsAsFile())
        captureReviewFile.deleteFile();
    if (captureReview != nullptr)
        captureReview->setVisible (false);
}

void OnorabAudioProcessorEditor::applySettings (const juce::String& hubUrl, const juce::File& cacheDir)
{
    if (hubUrl.isEmpty())
    {
        if (settingsTab != nullptr)
            settingsTab->setStatus ("Hub URL must not be empty.");
        return;
    }
    if (! cacheDir.getFullPathName().isEmpty() && ! cacheDir.isDirectory()
        && ! cacheDir.createDirectory())
    {
        if (settingsTab != nullptr)
            settingsTab->setStatus ("Could not create the cache folder: " + cacheDir.getFullPathName());
        return;
    }
    audioProcessor.setHubUrl (hubUrl);
    hub.setBaseUrl (hubUrl);
    if (! cacheDir.getFullPathName().isEmpty())
        audioProcessor.setCacheDir (cacheDir);
    if (settingsTab != nullptr)
        settingsTab->setStatus ("Saved - Hub URL and cache folder updated (saved with the project).");
    // A URL change re-arms the auto-launch: switching back to the
    // default Hub after a custom one deserves a fresh attempt.
    hubLaunchAttempted = false;
    checkHealth();
    refreshFiles();
}

void OnorabAudioProcessorEditor::checkHealth()
{
    hub.checkHealth ([this] (bool ok, juce::String error, juce::String /*detail*/)
    {
        // (void) error: in Release builds DBG expands to nothing, so
        // without this the parameter reads as unreferenced (C4100).
        (void) error;
        hubConnected = ok;
        hubDot.setDotColour (ok ? kGreen : kRed);
        hubStatusLabel.setText (ok ? "Hub connected" : "Hub disconnected",
                                juce::NotificationType::dontSendNotification);
        if (! ok)
        {
            DBG ("Onorab Hub health check failed: " << error);
            tryLaunchInstalledHub();
        }
    });
}

void OnorabAudioProcessorEditor::tryLaunchInstalledHub()
{
    if (hubLaunchAttempted)
        return;
    hubLaunchAttempted = true;
    // Only the default localhost Hub is auto-launched: a custom URL
    // means the user runs their own Hub and this plugin must not
    // start a second one on the default port.
    if (getHubUrl().trim() != "http://127.0.0.1:18741")
        return;
#if JUCE_WINDOWS
    // Well-known installer locations (see dist/windows/installer.nsi):
    // the Hub lives machine-wide under Program Files so every Windows
    // user on the PC finds the same file; the per-user AppData path
    // is kept as a fallback for manual installs.
    const juce::StringArray roots = {
        juce::SystemStats::getEnvironmentVariable ("ProgramFiles", ""),
        juce::SystemStats::getEnvironmentVariable ("ProgramFiles(x86)", ""),
        juce::SystemStats::getEnvironmentVariable ("LOCALAPPDATA", "")
    };
    juce::File hubExe;
    for (const auto& root : roots)
    {
        if (root.isEmpty())
            continue;
        juce::File candidate = juce::File (root).getChildFile ("Onorab")
                                                .getChildFile ("onorab-hub.exe");
        if (candidate.existsAsFile())
        {
            hubExe = candidate;
            break;
        }
    }
    if (hubExe == juce::File())
    {
        // No installed Hub: the musician either has not run the
        // installer, or runs the Hub manually. Say so once (not on
        // every 2-second health poll) instead of failing silently.
        setError ("Onorab Hub is not running and no installed Hub was found - "
                  "run the Onorab installer, then reopen this plugin.");
        return;
    }
    // Launch-and-forget: the ChildProcess object dies here but the
    // child keeps running (JUCE never kills a child on destruction),
    // so the Hub survives FL closing and the next editor open finds
    // it via the health check. The -provider flag replaces the env
    // var the musician must never touch. On Windows JUCE launches
    // with CREATE_NO_WINDOW, so no console window appears.
    juce::ChildProcess launcher;
    const juce::String command = "\"" + hubExe.getFullPathName() + "\" -provider drive";
    if (! launcher.start (command))
        setError ("Onorab Hub could not be started from " + hubExe.getFullPathName());
#endif
}

void OnorabAudioProcessorEditor::refreshDriveStatus()
{
    hub.getAuthStatus ([this] (bool ok, juce::String, juce::String detail)
    {
        if (! ok)
            return;
        driveConnected = detail.contains ("authenticated: 1")
                      || detail.contains ("authenticated: true");
        // Detail format (HubClient): "provider: ..., authenticated: ...,
        // account: <email>, oauthInProgress: ..., authError: <message>".
        // The account is display-only: an empty account with
        // authenticated=true still counts as connected. authError is
        // the tail (its message may contain commas), account ends at
        // ", oauthInProgress:"; both markers are absent for old Hub
        // builds, which fall back to "to end" parsing.
        juce::String account, authError;
        const int accountPos = detail.indexOf ("account: ");
        if (accountPos >= 0)
        {
            const int accountEnd = detail.indexOf (accountPos, ", oauthInProgress:");
            account = (accountEnd >= 0 ? detail.substring (accountPos + 9, accountEnd)
                                       : detail.substring (accountPos + 9)).trim();
        }
        const int errorPos = detail.indexOf ("authError: ");
        if (errorPos >= 0)
            authError = detail.substring (errorPos + 11).trim();
        const bool authInProgress = detail.contains ("oauthInProgress: 1")
                                 || detail.contains ("oauthInProgress: true");
        const bool wasConnected = prevDriveConnected;
        prevDriveConnected = driveConnected;
        driveDot.setDotColour (driveConnected ? kGreen : kGrey);
        driveStatusLabel.setText (driveConnected ? "Drive connected" : "Drive not connected",
                                  juce::NotificationType::dontSendNotification);
        if (settingsTab != nullptr)
            settingsTab->setDriveConnected (driveConnected, account);
        if (driveConnected)
        {
            authAwaiting = false;
            if (settingsTab != nullptr)
            {
                settingsTab->setConnectButtonText ("Connect Google Drive");
                if (! wasConnected)
                    settingsTab->setDriveMessage ("Drive connected.");
            }
        }
        else if (authError.isNotEmpty() && ! authInProgress)
        {
            // A reported failure (cancel, refused exchange, keychain
            // store failure, timeout): show it with "Try again"
            // instead of a silent "Not connected".
            authAwaiting = false;
            if (settingsTab != nullptr)
            {
                settingsTab->setDriveMessage ("Drive sign-in failed: " + authError + " - press Try again.");
                settingsTab->setConnectButtonText ("Try again");
            }
        }
        else if (authInProgress)
        {
            // Sign-in still running: keep "Waiting for Google…" —
            // do not overwrite it with "Not connected" while the
            // browser flow is in flight.
        }
        else if (authAwaiting)
        {
            // The Hub says no session is running, but the plugin is
            // still waiting on one: it ended without a report (old
            // Hub build, or an edge case). Do not wait forever on
            // "Waiting…".
            authAwaiting = false;
            if (settingsTab != nullptr)
                settingsTab->setDriveMessage ("Drive sign-in did not complete - press Connect Drive again.");
        }
        else if (wasConnected && settingsTab != nullptr)
        {
            // Connected -> not connected between polls (e.g. the
            // token was revoked): report the transition once.
            settingsTab->setDriveMessage ("Not connected.");
        }
    });
}

int OnorabAudioProcessorEditor::addLocalTransfer (const juce::String& fileName,
                                                  const juce::String& direction)
{
    LocalTransfer transfer;
    transfer.fileName = fileName;
    transfer.direction = direction;
    transfer.status = direction == "Upload" ? "Uploading..." : "Downloading...";
    transfer.progress = 0.0;
    localTransfers.push_back (std::move (transfer));
    return static_cast<int> (localTransfers.size()) - 1;
}

void OnorabAudioProcessorEditor::updateLocalTransfer (int entry, double progress,
                                                      const juce::String& status,
                                                      const juce::String& error, bool canRetry)
{
    if (entry < 0 || entry >= static_cast<int> (localTransfers.size()))
        return;
    auto& transfer = localTransfers[static_cast<size_t> (entry)];
    const bool statusChanged = (transfer.status != status) || (transfer.error != error)
                            || (transfer.canRetry != canRetry);
    transfer.progress = juce::jlimit (0.0, 1.0, progress);
    transfer.status = status;
    transfer.error = error;
    transfer.canRetry = canRetry;
    // Throttle Transfers tab refreshes during progress callbacks: only
    // rebuild when the bar moved by at least 5% or the state changed.
    if (statusChanged || transfer.progress >= 1.0
        || transfer.progress - transfer.lastReportedProgress >= 0.05)
    {
        transfer.lastReportedProgress = transfer.progress;
        if (transfersTab != nullptr)
        {
            std::vector<TransferRow> rows;
            for (int i = 0; i < static_cast<int> (localTransfers.size()); ++i)
            {
                const auto& t = localTransfers[static_cast<size_t> (i)];
                TransferRow row;
                row.entry = i;
                row.fileName = t.fileName;
                row.direction = t.direction;
                row.status = t.status;
                row.progress = t.progress;
                row.error = t.error;
                row.canRetry = t.canRetry;
                rows.push_back (row);
            }
            for (const auto& t : hubTransfers)
            {
                TransferRow row;
                row.entry = -1;
                row.fileName = t.fileName.isNotEmpty() ? t.fileName : t.id;
                row.direction = t.direction.equalsIgnoreCase ("upload") ? "Upload"
                              : t.direction.equalsIgnoreCase ("download") ? "Download" : t.direction;
                row.status = t.status;
                row.progress = t.progress;
                row.error = (t.status == "failed") ? t.message : juce::String {};
                row.canRetry = false;
                rows.push_back (row);
            }
            transfersTab->setRows (rows);
        }
    }
}

void OnorabAudioProcessorEditor::refreshTransfers (bool silent)
{
    hub.listTransfers ([this, silent] (bool ok, juce::String error, std::vector<TransferInfo> newTransfers)
    {
        if (ok)
        {
            hubTransfers = std::move (newTransfers);
            if (transfersTab != nullptr)
            {
                // Force one rebuild with the fresh Hub records.
                for (auto& t : localTransfers)
                    t.lastReportedProgress = -1.0;
                if (! localTransfers.empty())
                {
                    updateLocalTransfer (0, localTransfers.front().progress, localTransfers.front().status,
                                         localTransfers.front().error, localTransfers.front().canRetry);
                }
                else
                {
                    std::vector<TransferRow> rows;
                    for (const auto& t : hubTransfers)
                    {
                        TransferRow row;
                        row.entry = -1;
                        row.fileName = t.fileName.isNotEmpty() ? t.fileName : t.id;
                        row.direction = t.direction.equalsIgnoreCase ("upload") ? "Upload"
                                      : t.direction.equalsIgnoreCase ("download") ? "Download" : t.direction;
                        row.status = t.status;
                        row.progress = t.progress;
                        row.error = (t.status == "failed") ? t.message : juce::String {};
                        row.canRetry = false;
                        rows.push_back (row);
                    }
                    transfersTab->setRows (rows);
                }
                transfersTab->setNote (juce::String (localTransfers.size() + hubTransfers.size())
                                       + " transfer(s) in this session and on the Hub.");
            }
        }
        else if (! silent && transfersTab != nullptr)
        {
            transfersTab->setNote ("Could not load transfers: " + error);
        }
    });
}

void OnorabAudioProcessorEditor::refreshProjectsTab()
{
    if (projectsTab != nullptr)
        projectsTab->setFiles (files);
}

juce::File OnorabAudioProcessorEditor::getCacheDir() const
{
    return audioProcessor.getCacheDir();
}

juce::String OnorabAudioProcessorEditor::getHubUrl() const
{
    return audioProcessor.getHubUrl();
}

bool OnorabAudioProcessorEditor::isCapturing() const
{
    return audioProcessor.isCapturing();
}

// ============================== Send / Manager + Push ==============================

void OnorabAudioProcessorEditor::buildUIForMode()
{
    const auto mode = audioProcessor.getMode();
    const bool isManager = mode == OnorabAudioProcessor::PluginMode::Manager;
    const bool isSend = mode == OnorabAudioProcessor::PluginMode::Send;

    // Header + tabs exist only in Manager mode. The components are all
    // created once in the constructor; this only toggles visibility,
    // so switching from the choice view never deletes a tab.
    titleLabel.setVisible (isManager);
    byLabel.setVisible (isManager);
    hubDot.setVisible (isManager);
    hubStatusLabel.setVisible (isManager);
    driveDot.setVisible (isManager);
    driveStatusLabel.setVisible (isManager);
    captureCounterLabel.setVisible (isManager);
    tabbedComponent.setVisible (isManager);
    if (captureReview != nullptr)
        captureReview->setVisible (isManager && captureReview->isVisible());

    sendTitleLabel.setVisible (isSend);
    sendNameLabel.setVisible (isSend);
    sendNameBox.setVisible (isSend);
    sendSessionLabel.setVisible (isSend);
    sendMeter.setVisible (isSend);
    sendStatusLabel.setVisible (isSend);
    sendHintLabel.setVisible (isSend);

    const bool isChoice = ! isManager && ! isSend;
    choiceTitleLabel.setVisible (isChoice);
    choiceSendButton.setVisible (isChoice);
    choiceSendHintLabel.setVisible (isChoice);
    choiceManagerButton.setVisible (isChoice);
    choiceManagerHintLabel.setVisible (isChoice);
    choiceNoteLabel.setVisible (isChoice);

    if (isManager)
    {
        setSize (920, 600);
        tabbedComponent.setCurrentTabIndex (0); // Tracks is the first tab
        audioProcessor.setMeterEnabled (false);
        startTimer (1000);
    }
    else if (isSend)
    {
        setSize (360, 240);
        sendNameBox.setText (audioProcessor.getTrackName(),
                             juce::NotificationType::dontSendNotification);
        audioProcessor.setMeterEnabled (true);
        displayedPeak = 0.0f;
        startTimer (50); // meter refresh; the Send branch repaints only
    }
    else
    {
        setSize (480, 300);
        audioProcessor.setMeterEnabled (false);
        startTimer (1000);
    }
    resized();
}

void OnorabAudioProcessorEditor::chooseMode (OnorabAudioProcessor::PluginMode newMode)
{
    if (newMode == OnorabAudioProcessor::PluginMode::Send)
    {
        // Name first (unregistered, so this only sets the member),
        // then register under that name.
        audioProcessor.setTrackName (suggestTrackName());
        audioProcessor.setMode (OnorabAudioProcessor::PluginMode::Send);
    }
    else
    {
        audioProcessor.setMode (OnorabAudioProcessor::PluginMode::Manager);
    }
    buildUIForMode();
}

juce::String OnorabAudioProcessorEditor::suggestTrackName() const
{
    std::set<juce::String> used;
    for (const auto& s : PushSession::getInstance().getSends())
        used.insert (s.trackName);
    for (int n = 1;; ++n)
    {
        const juce::String candidate = "Track " + juce::String (n).paddedLeft ('0', 2);
        if (used.find (candidate) == used.end())
            return candidate;
    }
}

std::vector<juce::String> OnorabAudioProcessorEditor::getKnownProjects() const
{
    std::vector<juce::String> out;
    for (const auto& f : files)
    {
        if (f.projectId.isEmpty())
            continue;
        if (std::find (out.begin(), out.end(), f.projectId) == out.end())
            out.push_back (f.projectId);
    }
    if (pushProject.isNotEmpty()
        && std::find (out.begin(), out.end(), pushProject) == out.end())
        out.push_back (pushProject);
    return out;
}

juce::String OnorabAudioProcessorEditor::getTransferStatus (int entry) const
{
    if (entry < 0 || entry >= static_cast<int> (localTransfers.size()))
        return {};
    return localTransfers[static_cast<size_t> (entry)].status;
}

juce::String OnorabAudioProcessorEditor::takeRowStatus (const PushTakeState& takeState) const
{
    const auto& t = takeState.take;
    if (! t.file.existsAsFile())
    {
        if (t.error.isNotEmpty())
            return "Failed: " + t.error;
        return "No audio recorded";
    }
    juce::String base = t.finishedBy == "destroyed" ? "Partial - Send closed" : "Recorded";
    if (! takeState.uploadStarted)
        return base + " - waiting for upload";
    if (takeState.transferEntry < 0)
        return base + " - Upload failed";
    const juce::String st = getTransferStatus (takeState.transferEntry);
    if (st == "Done")
        return "Uploaded";
    if (st == "Failed")
        return base + " - Upload failed";
    return base + " - Uploading...";
}

void OnorabAudioProcessorEditor::toggleTrackSelection (const juce::String& instanceId)
{
    // Selection locks once a Push is underway; the rows then show the
    // armed set instead of the editable checkboxes.
    if (pushPhase != PushPhase::Idle && pushPhase != PushPhase::Done)
        return;
    if (tracksSelected.erase (instanceId) == 0)
        tracksSelected.insert (instanceId);
    refreshTracksTab();
}

void OnorabAudioProcessorEditor::retryPushUpload (const juce::String& instanceId)
{
    for (auto& ts : pushTakes)
    {
        if (ts.take.instanceId == instanceId && ts.transferEntry >= 0)
        {
            // Re-runs the SAME upload (same file, same project) on the
            // same transfer entry. The Hub updates a same-name file
            // instead of duplicating it, so a retry after a lost
            // confirmation can never create a second Drive copy.
            retryTransfer (ts.transferEntry);
            return;
        }
    }
}

void OnorabAudioProcessorEditor::onPushSelected()
{
    if (pushPhase != PushPhase::Idle && pushPhase != PushPhase::Done)
        return;
    const juce::String project = tracksTab != nullptr ? tracksTab->getSelectedProject() : juce::String {};
    if (project.isEmpty())
    {
        pushNotice = "Choose a Drive project first.";
        refreshTracksTab();
        return;
    }
    if (tracksSelected.empty())
    {
        pushNotice = "Select at least one track.";
        refreshTracksTab();
        return;
    }
    const std::vector<juce::String> selected (tracksSelected.begin(), tracksSelected.end());
    // Hub + Drive check first (async): a Push without a Drive
    // connection would record audio with nowhere to send it.
    hub.getAuthStatus ([this, project, selected] (bool ok, juce::String error, juce::String detail)
    {
        if (! ok)
        {
            pushNotice = "Drive check failed: " + error;
            refreshTracksTab();
            return;
        }
        if (! detail.contains ("authenticated: 1") && ! detail.contains ("authenticated: true"))
        {
            pushNotice = "Drive is not connected - connect it in Settings first.";
            refreshTracksTab();
            return;
        }
        // The passage must start from a stop: Sends begin recording on
        // the Play transition, so starting mid-playback would clip the
        // beginning and misalign the takes.
        if (audioProcessor.isTransportPlaying())
        {
            pushNotice = "FL Studio is already playing - stop playback, move the playhead "
                         "to the start, then press Push selected again.";
            refreshTracksTab();
            return;
        }
        auto& session = PushSession::getInstance();
        std::vector<juce::String> prepared, missing;
        std::vector<std::pair<juce::String, juce::String>> failed;
        if (! session.armPush (selected, project, prepared, missing, failed))
        {
            juce::String message = "No track could be prepared.";
            for (const auto& [id, err] : failed)
                message += " " + id + ": " + err;
            pushNotice = message;
            refreshTracksTab();
            return;
        }
        // Snapshot names now: a Send that disappears later still has
        // a name for its row.
        pushArmedNames.clear();
        for (const auto& s : session.getSends())
            pushArmedNames[s.instanceId] = s.trackName;
        pushArmedIds = prepared;
        pushProject = project;
        pushIdCurrent = session.getPushId();
        pushTakes.clear();
        pushPhase = PushPhase::Armed;
        pushRecordStartSeen = false;
        pushNotice.clear();
        if (! failed.empty() || ! missing.empty())
        {
            pushNotice = "Ready for " + juce::String (prepared.size()) + " track(s).";
            if (! failed.empty())
                pushNotice += " " + juce::String (failed.size()) + " track(s) could not be prepared.";
            if (! missing.empty())
                pushNotice += " " + juce::String (missing.size()) + " track(s) disappeared.";
        }
        refreshTracksTab();
    });
}

void OnorabAudioProcessorEditor::onPushFinish()
{
    if (pushPhase != PushPhase::Recording)
        return;
    // The Sends finalize on their next audio block (or their writer
    // drains on this flag if the host suspended blocks); the timer
    // then resolves the takes and uploads them, exactly like a
    // transport Stop.
    PushSession::getInstance().requestFinish();
    pushNotice.clear();
    refreshTracksTab();
}

void OnorabAudioProcessorEditor::onPushCancel()
{
    if (pushPhase != PushPhase::Armed)
        return;
    const auto notCancelled = PushSession::getInstance().cancelPush();
    if (! notCancelled.empty())
    {
        // A Send started recording between the phase check and this
        // click: the Push stays alive and must be finished, not cut.
        pushNotice = "Recording already started - use Finish & upload instead.";
        refreshTracksTab();
        return;
    }
    pushPhase = PushPhase::Idle;
    pushArmedIds.clear();
    pushArmedNames.clear();
    pushTakes.clear();
    pushNotice.clear();
    refreshTracksTab();
}

void OnorabAudioProcessorEditor::refreshTracksTab()
{
    if (tracksTab == nullptr
        || audioProcessor.getMode() != OnorabAudioProcessor::PluginMode::Manager)
        return;
    auto& session = PushSession::getInstance();
    auto sends = session.getSends();
    std::map<juce::String, PushSession::SendSnapshot> liveById;
    for (const auto& s : sends)
        liveById[s.instanceId] = s;

    // Drain finished takes for the current Push. Takes from older
    // pushes are ignored (their pushId never matches again); their
    // WAV files stay in the cache folder.
    if (pushPhase == PushPhase::Armed || pushPhase == PushPhase::Recording
        || pushPhase == PushPhase::Uploading)
    {
        for (auto& t : session.takeTakes())
        {
            if (t.pushId != pushIdCurrent)
                continue;
            pushTakes.push_back ({ t, -1, false });
        }
    }

    // Phase machine (driven by the Sends' own status, never by a guess).
    if (pushPhase == PushPhase::Armed)
    {
        bool anyRecording = false;
        for (const auto& id : pushArmedIds)
            if (auto it = liveById.find (id); it != liveById.end() && it->second.status == 2)
                anyRecording = true;
        if (anyRecording)
        {
            pushPhase = PushPhase::Recording;
            pushRecordStartTime = juce::Time::getCurrentTime();
            pushRecordStartSeen = true;
            pushNotice.clear();
        }
    }
    if (pushPhase == PushPhase::Recording)
    {
        // Every armed track resolved: it has a take, or its Send is
        // gone (missing is resolved by absence, not by waiting).
        bool allResolved = ! pushArmedIds.empty();
        for (const auto& id : pushArmedIds)
        {
            const bool hasTake = std::any_of (pushTakes.begin(), pushTakes.end(),
                [&id] (const PushTakeState& ts) { return ts.take.instanceId == id; });
            if (! hasTake && liveById.find (id) != liveById.end())
                allResolved = false;
        }
        if (allResolved)
        {
            pushPhase = PushPhase::Uploading;
            for (auto& ts : pushTakes)
            {
                if (ts.take.file.existsAsFile() && ! ts.uploadStarted)
                {
                    ts.uploadStarted = true;
                    ts.transferEntry = uploadLocalFileInternal (ts.take.file, pushProject, -1);
                }
            }
        }
    }
    if (pushPhase == PushPhase::Uploading)
    {
        bool allDone = true;
        for (const auto& ts : pushTakes)
        {
            if (! ts.take.file.existsAsFile() || ! ts.uploadStarted)
                continue;
            // A -1 entry means the file vanished before the upload
            // call: failed, not pending — never wait on it forever.
            if (ts.transferEntry < 0)
                continue;
            const juce::String st = getTransferStatus (ts.transferEntry);
            if (st != "Done" && st != "Failed")
                allDone = false;
        }
        if (allDone)
        {
            int uploaded = 0, failed = 0;
            for (const auto& ts : pushTakes)
            {
                if (! ts.take.file.existsAsFile())
                    continue;
                const juce::String st = getTransferStatus (ts.transferEntry);
                if (st == "Done")
                    ++uploaded;
                else if (st == "Failed")
                    ++failed;
            }
            pushPhase = PushPhase::Done;
            pushNotice = "Upload complete: " + juce::String (uploaded) + " uploaded"
                + (failed > 0 ? ", " + juce::String (failed) + " failed." : ".");
            session.disarmAfterUpload();
        }
    }

    // ---- View ----
    TracksTab::View view;
    view.sessionText = "Session: " + audioProcessor.getSessionTag()
        + " - Sends in this FL project appear here.";
    view.projects = getKnownProjects();
    view.selectedProject = pushPhase == PushPhase::Idle || pushPhase == PushPhase::Done
        ? (tracksTab->getSelectedProject()) : pushProject;
    view.projectEnabled = pushPhase == PushPhase::Idle || pushPhase == PushPhase::Done;
    const bool selectionLocked = pushPhase != PushPhase::Idle && pushPhase != PushPhase::Done;
    view.pushEnabled = (pushPhase == PushPhase::Idle || pushPhase == PushPhase::Done)
        && ! tracksSelected.empty();
    view.selectedCount = selectionLocked ? (int) pushArmedIds.size() : (int) tracksSelected.size();
    view.showFinish = pushPhase == PushPhase::Recording;
    view.finishEnabled = ! session.isFinishRequested();
    view.finishText = session.isFinishRequested() ? "Finishing..." : "Finish & upload";
    view.showCancel = pushPhase == PushPhase::Armed;

    if (! pushNotice.isEmpty())
    {
        view.statusText = pushNotice;
    }
    else if (pushPhase == PushPhase::Armed)
    {
        view.statusText = "Ready. Press Play in FL Studio.";
    }
    else if (pushPhase == PushPhase::Recording)
    {
        const double seconds = pushRecordStartSeen
            ? (juce::Time::getCurrentTime() - pushRecordStartTime).inSeconds() : 0.0;
        view.statusText = "Recording - " + formatDuration (seconds);
    }
    else if (pushPhase == PushPhase::Uploading)
    {
        view.statusText = "Uploading takes to " + HubClient::projectFolder (pushProject) + "...";
    }
    else if (pushPhase == PushPhase::Done)
    {
        view.statusText = "Select tracks for the next Push, or retry a failed upload.";
    }
    else
    {
        view.statusText = "Select tracks and a Drive project, then press Push selected.";
    }

    // One row per track: every live Send, plus every armed track that
    // disappeared (its take, if any, still uploads). During a Push the
    // checkboxes show the armed set; otherwise they show the editable
    // selection.
    std::set<juce::String> rowIds;
    for (const auto& s : sends)
        rowIds.insert (s.instanceId);
    for (const auto& id : pushArmedIds)
        rowIds.insert (id);
    for (const auto& id : rowIds)
    {
        TracksTab::Row row;
        row.instanceId = id;
        const auto liveIt = liveById.find (id);
        const auto takeIt = std::find_if (pushTakes.begin(), pushTakes.end(),
            [&id] (const PushTakeState& ts) { return ts.take.instanceId == id; });
        const bool hasTake = takeIt != pushTakes.end();
        row.selected = selectionLocked
            ? std::find (pushArmedIds.begin(), pushArmedIds.end(), id) != pushArmedIds.end()
            : tracksSelected.find (id) != tracksSelected.end();
        row.selectionEnabled = ! selectionLocked;
        if (liveIt != liveById.end())
        {
            row.trackName = liveIt->second.trackName;
            if (hasTake)
            {
                row.status = takeRowStatus (*takeIt);
            }
            else if (liveIt->second.status == 2)
            {
                row.status = "Recording";
            }
            else if (liveIt->second.status == 1)
            {
                row.status = "Armed - waiting for Play";
            }
            else if (liveIt->second.status == 3)
            {
                row.status = "Recorded";
            }
            else
            {
                row.status = "Idle";
            }
        }
        else
        {
            row.trackName = pushArmedNames.find (id) != pushArmedNames.end()
                ? pushArmedNames[id] : id;
            if (hasTake)
            {
                row.status = takeRowStatus (*takeIt);
                if (takeIt->take.finishedBy == "destroyed")
                    row.status += " - Send closed";
            }
            else
            {
                row.status = "Send disappeared - no take";
            }
        }
        row.canRetry = hasTake && takeIt->uploadStarted
            && getTransferStatus (takeIt->transferEntry) == "Failed";
        view.rows.push_back (row);
    }

    tracksTab->setView (view);
}
