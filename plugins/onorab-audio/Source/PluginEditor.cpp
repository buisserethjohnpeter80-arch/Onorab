// OnorabAudioProcessorEditor implementation. See PluginEditor.h for the
// threading and drag-and-drop notes. All Hub interaction goes through
// HubClient (background thread) and all UI updates run on the message
// thread. No network, file I/O or allocation for audio happens here that
// could reach the real-time thread — this file never touches processBlock.

#include "PluginEditor.h"
#include "PluginProcessor.h"

namespace
{
const juce::Colour kGraphite (0xff14171c);
const juce::Colour kPanel (0xff1a1d22);
const juce::Colour kCyan (0xff22d3ee);
const juce::Colour kViolet (0xff8b5cf6);
const juce::Colour kGreen (0xff22c55e);
const juce::Colour kRed (0xffef4444);

void styleButton (juce::TextButton& button, juce::Colour accent = kCyan)
{
    button.setColour (juce::TextButton::buttonColourId, kPanel);
    button.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    button.setColour (juce::TextButton::textColourOnId, accent);
}

void styleLabel (juce::Label& label, juce::Colour colour = juce::Colours::white, float size = 13.0f)
{
    label.setColour (juce::Label::textColourId, colour);
    label.setFont (juce::Font (juce::FontOptions (size)));
    label.setJustificationType (juce::Justification::left);
}

juce::String formatBytes (int64_t bytes)
{
    if (bytes < 1024)
        return juce::String (bytes) + " B";
    if (bytes < 1024 * 1024)
        return juce::String (bytes / 1024.0, 1) + " KB";
    return juce::String (bytes / (1024.0 * 1024.0), 1) + " MB";
}

bool isAudioFile (const juce::String& path)
{
    static const juce::StringArray exts { "wav", "mp3", "flac", "aiff", "aif", "ogg", "m4a" };
    return exts.contains (juce::File (path).getFileExtension().toLowerCase(), true);
}
} // namespace

// ============================== DragSourceComponent ======================

void DragSourceComponent::paint (juce::Graphics& g)
{
    g.fillAll (kViolet.withAlpha (0.25f));
    g.setColour (kViolet);
    g.drawRect (getLocalBounds(), 1);
    g.setColour (juce::Colours::white);
    g.setFont (13.0f);
    g.drawFittedText ("Drag to FL — drag this box into FL Studio",
                      getLocalBounds().reduced (6), juce::Justification::centred, 2);
}

void DragSourceComponent::mouseDrag (const juce::MouseEvent&)
{
    if (isDragging || getFilesToDrag == nullptr)
        return;

    // Only real, existing local files are handed out. Nothing taken
    // yet means nothing to drag — never an empty or missing path.
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

// ============================== LibraryTab ==============================

LibraryTab::LibraryTab (OnorabAudioProcessorEditor& editorIn)
    : owner (editorIn),
      waveform (waveformFormatManager)
{
    waveformFormatManager.registerBasicFormats();

    searchBox.setTextToShowWhenEmpty ("Search files…", juce::Colours::grey);
    searchBox.addListener (this);
    addAndMakeVisible (searchBox);

    styleLabel (syncLabel, juce::Colours::lightgrey);
    syncLabel.setText ("Hub: checking…", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (syncLabel);

    styleLabel (errorLabel, kRed);
    addAndMakeVisible (errorLabel);
    styleLabel (progressLabel, kGreen);
    addAndMakeVisible (progressLabel);
    styleLabel (captureLabel, juce::Colours::lightgrey);
    addAndMakeVisible (captureLabel);
    styleLabel (dragHintLabel, juce::Colours::grey, 12.0f);
    dragHintLabel.setText ("Tip: take a file first, then drag the violet box into FL Studio.",
                           juce::NotificationType::dontSendNotification);
    addAndMakeVisible (dragHintLabel);

    fileList.setRowHeight (22);
    fileList.setMultipleSelectionEnabled (true); // Take one or more sounds.
    addAndMakeVisible (fileList);

    addAndMakeVisible (waveform);

    styleButton (refreshButton);
    refreshButton.onClick = [this] { owner.refreshFiles(); };
    addAndMakeVisible (refreshButton);

    styleButton (takeButton, kCyan);
    takeButton.onClick = [this] { owner.takeSelectedFiles(); };
    addAndMakeVisible (takeButton);

    styleButton (deleteButton, kRed);
    deleteButton.onClick = [this] { owner.deleteHubFiles (getSelectedFiles()); };
    addAndMakeVisible (deleteButton);

    styleButton (uploadButton, kViolet);
    uploadButton.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser> ("Choose an audio file to upload",
                                                           juce::File(), "*.wav;*.mp3;*.flac;*.aiff;*.ogg;*.m4a");
        fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& chooser)
                                  {
                                      if (const juce::File f = chooser.getResult(); f.existsAsFile())
                                          owner.uploadLocalFile (f);
                                  });
    };
    addAndMakeVisible (uploadButton);

    styleButton (uploadCaptureButton, kGreen);
    uploadCaptureButton.onClick = [this] { owner.uploadLastCapture(); };
    addAndMakeVisible (uploadCaptureButton);

    styleButton (playButton, kGreen);
    playButton.onClick = [this] { owner.playPreview(); };
    addAndMakeVisible (playButton);
    styleButton (pauseButton);
    pauseButton.onClick = [this] { owner.pausePreview(); };
    addAndMakeVisible (pauseButton);
    styleButton (stopButton);
    stopButton.onClick = [this] { owner.stopPreview(); };
    addAndMakeVisible (stopButton);

    gainSlider.setRange (0.0, 1.0, 0.01);
    gainSlider.setValue (0.8);
    gainSlider.onValueChange = [this] { owner.setPreviewGain (static_cast<float> (gainSlider.getValue())); };
    addAndMakeVisible (gainSlider);
    styleLabel (gainLabel, juce::Colours::lightgrey, 12.0f);
    gainLabel.setText ("Preview gain", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (gainLabel);

    styleButton (captureButton, kRed);
    captureButton.onClick = [this] { owner.toggleCapture(); };
    addAndMakeVisible (captureButton);

    // Onorab Bus support (user logic, 2026-11-03): the plugin CANNOT
    // create an FL Mixer track or route FL tracks — FL Studio owns its
    // Mixer. What the plugin CAN do is name a capture after the routed
    // track, so a routed track-by-track capture stays identifiable
    // when it is uploaded / pushed to a project.
    captureNameBox.setTextToShowWhenEmpty ("Capture name, e.g. Vocal (empty = auto name)", juce::Colours::grey);
    addAndMakeVisible (captureNameBox);

    styleLabel (busGuideLabel, juce::Colours::grey, 12.0f);
    busGuideLabel.setText ("Onorab Bus: one Mixer track for Onorab, Route this track per track, then Capture.",
                           juce::NotificationType::dontSendNotification);
    addAndMakeVisible (busGuideLabel);

    dragSource.getFilesToDrag = [this]
    {
        if (! takenLocalFiles.isEmpty())
            return takenLocalFiles;
        if (selectedLocalExists)
            return juce::StringArray { selectedLocalFile.getFullPathName() };
        return juce::StringArray {};
    };
    addAndMakeVisible (dragSource);
}

LibraryTab::~LibraryTab() = default;

void LibraryTab::setFiles (const std::vector<CloudFile>& files)
{
    allFiles = files;
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
}

void LibraryTab::textEditorTextChanged (juce::TextEditor&)
{
    applySearchFilter();
}

void LibraryTab::setSyncStatus (const juce::String& text, bool connected)
{
    syncLabel.setText ("Hub: " + text, juce::NotificationType::dontSendNotification);
    syncLabel.setColour (juce::Label::textColourId, connected ? kGreen : kRed);
}

void LibraryTab::setError (const juce::String& text)
{
    errorLabel.setText (text, juce::NotificationType::dontSendNotification);
    if (text.isNotEmpty())
        DBG ("Onorab UI error: " << text);
}

void LibraryTab::setProgress (const juce::String& text)
{
    progressLabel.setText (text, juce::NotificationType::dontSendNotification);
}

void LibraryTab::setCapturing (bool isCapturing)
{
    capturing = isCapturing;
    captureButton.setButtonText (isCapturing ? "Stop capture" : "Capture from FL");
    if (! isCapturing)
        captureLabel.setText ("Capture stopped.", juce::NotificationType::dontSendNotification);
}

void LibraryTab::setCaptureNote (const juce::String& text)
{
    captureLabel.setText (text, juce::NotificationType::dontSendNotification);
}

void LibraryTab::setSelectedLocalFile (const juce::File& file, bool exists)
{
    selectedLocalFile = file;
    selectedLocalExists = exists;
    waveform.setFile (file); // Shows a placeholder when the file is not local yet.
    updateDragHint();
}

juce::String LibraryTab::getCaptureName() const
{
    return captureNameBox.getText().trim();
}

void LibraryTab::setTakenLocalFiles (const juce::StringArray& files)
{
    takenLocalFiles = files;
    updateDragHint();
}

std::vector<CloudFile> LibraryTab::getSelectedFiles() const
{
    std::vector<CloudFile> out;
    // juce::SparseSet has no begin()/end() iterators, so a range-for
    // over getSelectedRows() does not compile (MSVC C3312). Index it:
    // rows[i] is the i-th selected row, rows.size() the count.
    const juce::SparseSet<int> rows = fileList.getSelectedRows();
    for (int i = 0; i < rows.size(); ++i)
    {
        const int row = rows[i];
        if (row >= 0 && row < static_cast<int> (filteredFiles.size()))
            out.push_back (filteredFiles[static_cast<size_t> (row)]);
    }
    return out;
}

void LibraryTab::updateDragHint()
{
    if (! takenLocalFiles.isEmpty())
    {
        dragHintLabel.setText ("Ready to drag: " + juce::String (takenLocalFiles.size())
                                   + (takenLocalFiles.size() == 1 ? " file" : " files"),
                               juce::NotificationType::dontSendNotification);
        return;
    }
    dragHintLabel.setText (selectedLocalExists
                               ? "Ready to drag: " + selectedLocalFile.getFileName()
                               : "Tip: take a file first, then drag the violet box into FL Studio.",
                           juce::NotificationType::dontSendNotification);
}

void LibraryTab::resized()
{
    auto area = getLocalBounds().reduced (8);
    searchBox.setBounds (area.removeFromTop (26));
    area.removeFromTop (4);
    syncLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (4);

    auto buttonRow1 = area.removeFromTop (28);
    refreshButton.setBounds (buttonRow1.removeFromLeft (90));
    buttonRow1.removeFromLeft (6);
    takeButton.setBounds (buttonRow1.removeFromLeft (140));
    buttonRow1.removeFromLeft (6);
    uploadButton.setBounds (buttonRow1.removeFromLeft (110));
    buttonRow1.removeFromLeft (6);
    uploadCaptureButton.setBounds (buttonRow1.removeFromLeft (170));
    buttonRow1.removeFromLeft (6);
    deleteButton.setBounds (buttonRow1.removeFromLeft (110));

    area.removeFromTop (6);
    fileList.setBounds (area.removeFromTop (100));
    area.removeFromTop (6);
    waveform.setBounds (area.removeFromTop (80));
    area.removeFromTop (6);

    auto transportRow = area.removeFromTop (28);
    playButton.setBounds (transportRow.removeFromLeft (70));
    transportRow.removeFromLeft (6);
    pauseButton.setBounds (transportRow.removeFromLeft (70));
    transportRow.removeFromLeft (6);
    stopButton.setBounds (transportRow.removeFromLeft (70));
    transportRow.removeFromLeft (12);
    gainLabel.setBounds (transportRow.removeFromLeft (90));
    gainSlider.setBounds (transportRow);

    area.removeFromTop (6);
    auto captureRow = area.removeFromTop (28);
    captureButton.setBounds (captureRow.removeFromLeft (150));
    captureRow.removeFromLeft (8);
    captureLabel.setBounds (captureRow);

    area.removeFromTop (4);
    captureNameBox.setBounds (area.removeFromTop (26));
    area.removeFromTop (4);
    busGuideLabel.setBounds (area.removeFromTop (18));

    area.removeFromTop (4);
    dragSource.setBounds (area.removeFromTop (40));
    area.removeFromTop (4);
    dragHintLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (4);
    progressLabel.setBounds (area.removeFromTop (18));
    errorLabel.setBounds (area.removeFromTop (18));
}

int LibraryTab::getNumRows()
{
    return static_cast<int> (filteredFiles.size());
}

void LibraryTab::paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height,
                                   bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= static_cast<int> (filteredFiles.size()))
        return;
    const auto& file = filteredFiles[static_cast<size_t> (rowNumber)];
    if (rowIsSelected)
        g.fillAll (kCyan.withAlpha (0.2f));
    // Project shown inline: "name  ·  project". Unassigned sounds show
    // only their name, so old Hub files still render cleanly.
    juce::String label = file.name;
    if (file.projectId.isNotEmpty())
        label += "  ·  " + file.projectId;
    g.setColour (juce::Colours::white);
    g.setFont (13.0f);
    g.drawText (label, 6, 0, width - 110, height, juce::Justification::left, true);
    g.setColour (juce::Colours::lightgrey);
    g.drawText (formatBytes (file.sizeBytes), width - 100, 0, 94, height,
                juce::Justification::right, true);
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
    infoLabel.setText ("Projects live on Drive under Onorab_projet/<project>/ — "
                       "type a project name to filter this list AND to target "
                       "Upload / Push. FL Studio cannot list its own tracks "
                       "automatically, so local sounds enter a project only "
                       "through Upload or Push below.",
                       juce::NotificationType::dontSendNotification);
    infoLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    infoLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    infoLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (infoLabel);

    projectNameBox.setTextToShowWhenEmpty ("Project name, e.g. marcus music", juce::Colours::grey);
    projectNameBox.addListener (this);
    addAndMakeVisible (projectNameBox);

    searchBox.setTextToShowWhenEmpty ("Search sounds in this project…", juce::Colours::grey);
    searchBox.addListener (this);
    addAndMakeVisible (searchBox);

    fileList.setRowHeight (22);
    fileList.setMultipleSelectionEnabled (true); // Push one or more sounds.
    addAndMakeVisible (fileList);

    styleButton (uploadOneButton, kViolet);
    uploadOneButton.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser> ("Choose an audio file to upload to this project",
                                                           juce::File(), "*.wav;*.mp3;*.flac;*.aiff;*.ogg;*.m4a");
        fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& chooser)
                                  {
                                      if (const juce::File f = chooser.getResult(); f.existsAsFile())
                                          owner.uploadFilesToProject ({ f }, getProjectName());
                                  });
    };
    addAndMakeVisible (uploadOneButton);

    styleButton (uploadAllButton, kViolet);
    uploadAllButton.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser> ("Choose audio files to upload to this project",
                                                           juce::File(), "*.wav;*.mp3;*.flac;*.aiff;*.ogg;*.m4a");
        fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::canSelectMultipleItems,
                                  [this] (const juce::FileChooser& chooser)
                                  {
                                      std::vector<juce::File> picked;
                                      for (const auto& f : chooser.getResults())
                                          if (f.existsAsFile())
                                              picked.push_back (f);
                                      owner.uploadFilesToProject (picked, getProjectName());
                                  });
    };
    addAndMakeVisible (uploadAllButton);

    styleButton (pushButton, kGreen);
    pushButton.onClick = [this] { owner.pushProjectFiles (getSelectedFiles(), getProjectName()); };
    addAndMakeVisible (pushButton);

    styleButton (deleteButton, kRed);
    deleteButton.onClick = [this] { owner.deleteHubFiles (getSelectedFiles()); };
    addAndMakeVisible (deleteButton);

    styleLabel (noteLabel, juce::Colours::grey, 12.0f);
    noteLabel.setText ("Push uploads the LOCAL copies of the selected sounds into "
                       "Onorab_projet/<project>/. A sound with no local copy yet "
                       "is skipped — Take it from the Cloud Library first.",
                       juce::NotificationType::dontSendNotification);
    addAndMakeVisible (noteLabel);
}

void ProjectsTab::setFiles (const std::vector<CloudFile>& files)
{
    allFiles = files;
    applyFilter();
}

void ProjectsTab::applyFilter()
{
    const juce::String projectQuery = projectNameBox.getText().trim().toLowerCase();
    const juce::String query = searchBox.getText().trim().toLowerCase();
    filteredFiles.clear();
    for (const auto& f : allFiles)
    {
        if (projectQuery.isNotEmpty() && ! f.projectId.toLowerCase().contains (projectQuery))
            continue;
        if (query.isNotEmpty() && ! f.name.toLowerCase().contains (query))
            continue;
        filteredFiles.push_back (f);
    }
    fileList.updateContent();
    fileList.repaint();
}

void ProjectsTab::textEditorTextChanged (juce::TextEditor&)
{
    applyFilter();
}

juce::String ProjectsTab::getProjectName() const
{
    return projectNameBox.getText().trim();
}

std::vector<CloudFile> ProjectsTab::getSelectedFiles() const
{
    std::vector<CloudFile> out;
    // Same SparseSet note as LibraryTab: no iterators, index it.
    const juce::SparseSet<int> rows = fileList.getSelectedRows();
    for (int i = 0; i < rows.size(); ++i)
    {
        const int row = rows[i];
        if (row >= 0 && row < static_cast<int> (filteredFiles.size()))
            out.push_back (filteredFiles[static_cast<size_t> (row)]);
    }
    return out;
}

int ProjectsTab::getNumRows()
{
    return static_cast<int> (filteredFiles.size());
}

void ProjectsTab::paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height,
                                    bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= static_cast<int> (filteredFiles.size()))
        return;
    const auto& file = filteredFiles[static_cast<size_t> (rowNumber)];
    if (rowIsSelected)
        g.fillAll (kCyan.withAlpha (0.2f));
    g.setColour (juce::Colours::white);
    g.setFont (13.0f);
    g.drawText (file.name, 6, 0, width - 110, height, juce::Justification::left, true);
    g.setColour (juce::Colours::lightgrey);
    g.drawText (formatBytes (file.sizeBytes), width - 100, 0, 94, height,
                juce::Justification::right, true);
}

void ProjectsTab::selectedRowsChanged (int lastRowSelected)
{
    if (lastRowSelected >= 0 && lastRowSelected < static_cast<int> (filteredFiles.size()))
        owner.onFileSelected (filteredFiles[static_cast<size_t> (lastRowSelected)]);
}

void ProjectsTab::resized()
{
    auto area = getLocalBounds().reduced (8);
    infoLabel.setBounds (area.removeFromTop (52));
    area.removeFromTop (6);
    projectNameBox.setBounds (area.removeFromTop (26));
    area.removeFromTop (4);
    searchBox.setBounds (area.removeFromTop (26));
    area.removeFromTop (6);
    auto buttonRow = area.removeFromTop (28);
    uploadOneButton.setBounds (buttonRow.removeFromLeft (120));
    buttonRow.removeFromLeft (6);
    uploadAllButton.setBounds (buttonRow.removeFromLeft (120));
    buttonRow.removeFromLeft (6);
    pushButton.setBounds (buttonRow.removeFromLeft (90));
    buttonRow.removeFromLeft (6);
    deleteButton.setBounds (buttonRow.removeFromLeft (110));
    area.removeFromTop (6);
    fileList.setBounds (area.removeFromTop (180));
    area.removeFromTop (6);
    noteLabel.setBounds (area.removeFromTop (36));
}

// ============================== TransfersTab ==============================

TransfersTab::TransfersTab (OnorabAudioProcessorEditor& editorIn)
    : owner (editorIn)
{
    styleLabel (noteLabel, juce::Colours::lightgrey);
    noteLabel.setText ("Transfers refresh every 2 seconds from the Hub (Phase 1 polling; "
                       "no WebSocket yet).",
                       juce::NotificationType::dontSendNotification);
    addAndMakeVisible (noteLabel);
    transferList.setRowHeight (22);
    addAndMakeVisible (transferList);
    (void) owner;
}

void TransfersTab::setTransfers (const std::vector<TransferInfo>& newTransfers)
{
    transfers = newTransfers;
    transferList.updateContent();
    transferList.repaint();
}

void TransfersTab::setNote (const juce::String& text)
{
    noteLabel.setText (text, juce::NotificationType::dontSendNotification);
}

void TransfersTab::resized()
{
    auto area = getLocalBounds().reduced (8);
    noteLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (6);
    transferList.setBounds (area);
}

int TransfersTab::getNumRows()
{
    return static_cast<int> (transfers.size());
}

void TransfersTab::paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height,
                                     bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= static_cast<int> (transfers.size()))
        return;
    const auto& t = transfers[static_cast<size_t> (rowNumber)];
    if (rowIsSelected)
        g.fillAll (kCyan.withAlpha (0.2f));
    g.setColour (juce::Colours::white);
    g.setFont (13.0f);
    const juce::String left = (t.fileName.isNotEmpty() ? t.fileName : t.id)
                            + " — " + t.direction + " — " + t.status;
    g.drawText (left, 6, 0, width - 80, height, juce::Justification::left, true);
    g.setColour (kGreen);
    g.drawText (juce::String (static_cast<int> (t.progress * 100.0)) + "%",
                width - 70, 0, 64, height, juce::Justification::right, true);
    if (t.message.isNotEmpty())
    {
        g.setColour (kRed);
        g.setFont (11.0f);
        g.drawText (t.message, 6, height - 12, width - 12, 12, juce::Justification::left, true);
    }
}

// ============================== SettingsTab ==============================

SettingsTab::SettingsTab (OnorabAudioProcessorEditor& editorIn)
    : owner (editorIn)
{
    styleLabel (hubUrlLabel);
    hubUrlLabel.setText ("Hub URL", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (hubUrlLabel);
    addAndMakeVisible (hubUrlBox);

    styleLabel (cacheDirLabel);
    cacheDirLabel.setText ("Cache folder", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (cacheDirLabel);
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

    noteLabel.setText ("Notes: the Hub runs on localhost only. Google Drive OAuth lives in "
                       "the Hub, not in this plugin, and no tokens are ever saved in an "
                       "FL Studio project. Default cache on Windows: "
                       "%LOCALAPPDATA%\\Onorab\\cache (override with ONORAB_CACHE_DIR). "
                       "Mock Drive mode is a Hub-side setting.",
                       juce::NotificationType::dontSendNotification);
    noteLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    noteLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    noteLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (noteLabel);

    styleLabel (statusLabel, kGreen);
    addAndMakeVisible (statusLabel);

    // Google Drive connect. The Hub holds the OAuth flow, not this
    // plugin: Connect Drive asks the Hub for a Google sign-in URL and
    // opens it in the browser. In Mock mode the Hub reports
    // oauth_unavailable honestly instead of faking a login; in Drive
    // mode it starts the real PKCE flow (drive.file only, token in
    // Windows Credential Manager on this PC only).
    styleLabel (driveLabel);
    driveLabel.setText ("Google Drive", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (driveLabel);
    driveEmailBox.setTextToShowWhenEmpty ("Gmail, e.g. you@gmail.com", juce::Colours::grey);
    addAndMakeVisible (driveEmailBox);
    styleButton (connectDriveButton, kCyan);
    connectDriveButton.onClick = [this] { owner.connectDrive (driveEmailBox.getText().trim()); };
    addAndMakeVisible (connectDriveButton);
    styleLabel (driveStatusLabel, juce::Colours::lightgrey, 12.0f);
    addAndMakeVisible (driveStatusLabel);

    // Invite another user to a project. In Mock mode this records a
    // pending invite on the Hub only. In Drive mode the Hub creates a
    // real Drive writer permission on Onorab_projet/<project>/ for
    // that Gmail. drive.file limitation, stated honestly in the
    // status text: the invitee's own Hub lists the shared project
    // once Drive shows it in their shared view.
    styleLabel (inviteLabel);
    inviteLabel.setText ("Invite to a project", juce::NotificationType::dontSendNotification);
    addAndMakeVisible (inviteLabel);
    inviteEmailBox.setTextToShowWhenEmpty ("Invited Gmail, e.g. friend@gmail.com", juce::Colours::grey);
    addAndMakeVisible (inviteEmailBox);
    inviteProjectBox.setTextToShowWhenEmpty ("Project name, e.g. marcus music", juce::Colours::grey);
    addAndMakeVisible (inviteProjectBox);
    styleButton (inviteButton, kViolet);
    inviteButton.onClick = [this] { owner.inviteToProject (inviteEmailBox.getText().trim(),
                                                           inviteProjectBox.getText().trim()); };
    addAndMakeVisible (inviteButton);
    styleLabel (inviteStatusLabel, juce::Colours::lightgrey, 12.0f);
    addAndMakeVisible (inviteStatusLabel);
}

void SettingsTab::setValues (const juce::String& hubUrl, const juce::String& cacheDir)
{
    hubUrlBox.setText (hubUrl);
    cacheDirBox.setText (cacheDir);
}

void SettingsTab::setStatus (const juce::String& text)
{
    statusLabel.setText (text, juce::NotificationType::dontSendNotification);
}

void SettingsTab::setDriveStatus (const juce::String& text)
{
    driveStatusLabel.setText (text, juce::NotificationType::dontSendNotification);
}

void SettingsTab::setInviteStatus (const juce::String& text)
{
    inviteStatusLabel.setText (text, juce::NotificationType::dontSendNotification);
}

void SettingsTab::resized()
{
    auto area = getLocalBounds().reduced (8);
    hubUrlLabel.setBounds (area.removeFromTop (20));
    hubUrlBox.setBounds (area.removeFromTop (26));
    area.removeFromTop (8);
    cacheDirLabel.setBounds (area.removeFromTop (20));
    cacheDirBox.setBounds (area.removeFromTop (26));
    area.removeFromTop (8);
    auto row = area.removeFromTop (28);
    saveButton.setBounds (row.removeFromLeft (90));
    row.removeFromLeft (6);
    browseButton.setBounds (row.removeFromLeft (110));
    area.removeFromTop (8);
    statusLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (8);
    driveLabel.setBounds (area.removeFromTop (20));
    driveEmailBox.setBounds (area.removeFromTop (26));
    area.removeFromTop (4);
    auto driveRow = area.removeFromTop (28);
    connectDriveButton.setBounds (driveRow.removeFromLeft (140));
    driveRow.removeFromLeft (8);
    driveStatusLabel.setBounds (driveRow);
    area.removeFromTop (8);
    inviteLabel.setBounds (area.removeFromTop (20));
    inviteEmailBox.setBounds (area.removeFromTop (26));
    area.removeFromTop (4);
    inviteProjectBox.setBounds (area.removeFromTop (26));
    area.removeFromTop (4);
    auto inviteRow = area.removeFromTop (28);
    inviteButton.setBounds (inviteRow.removeFromLeft (90));
    inviteRow.removeFromLeft (8);
    inviteStatusLabel.setBounds (inviteRow);
    area.removeFromTop (8);
    noteLabel.setBounds (area);
}

// ============================== Editor ==============================

OnorabAudioProcessorEditor::OnorabAudioProcessorEditor (OnorabAudioProcessor& processorIn)
    : AudioProcessorEditor (processorIn),
      audioProcessor (processorIn),
      hub (processorIn.getHubUrl())
{
    setSize (760, 520);

    formatManager.registerBasicFormats();
    readAheadThread.startThread();
    hub.setAliveToken (aliveToken);

    libraryTab = new LibraryTab (*this);
    projectsTab = new ProjectsTab (*this);
    transfersTab = new TransfersTab (*this);
    settingsTab = new SettingsTab (*this);

    tabbedComponent.addTab ("Cloud Library", kGraphite, libraryTab, true);
    tabbedComponent.addTab ("Projects", kGraphite, projectsTab, true);
    tabbedComponent.addTab ("Transfers", kGraphite, transfersTab, true);
    tabbedComponent.addTab ("Settings", kGraphite, settingsTab, true);

    settingsTab->setValues (audioProcessor.getHubUrl(),
                            audioProcessor.getCacheDir().getFullPathName());

    // The editor itself IS the DragAndDropContainer (see header), so the
    // tabbed component is added directly to the editor.
    addAndMakeVisible (tabbedComponent);

    refreshFiles();
    checkHealth();
    refreshTransfers (true);
    refreshProjectsTab();
    startTimer (2000); // Phase 1 polling for transfers + Hub health.
}

OnorabAudioProcessorEditor::~OnorabAudioProcessorEditor()
{
    // Flip the token FIRST so no queued Hub callback can touch this editor
    // after destruction starts. Then stop capture (joins the writer thread)
    // before any member is destroyed.
    aliveToken->store (false);
    stopTimer();
    if (audioProcessor.isCapturing())
        audioProcessor.stopCapture();
    transport.stop();
    transport.setSource (nullptr);
    previewSource.reset();
    readAheadThread.stopThread (500);
}

void OnorabAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kGraphite);
}

void OnorabAudioProcessorEditor::resized()
{
    tabbedComponent.setBounds (getLocalBounds());
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
            uploadLocalFile (juce::File (f));
        }
    }
    if (! anyAudio)
        setError ("Dropped files were not audio files — nothing was uploaded.");
}

void OnorabAudioProcessorEditor::timerCallback()
{
    // Polling only (Phase 1). This is deliberately lightweight localhost
    // traffic; a WebSocket /events client replaces it in Phase 2.
    checkHealth();
    refreshTransfers (true);
}

void OnorabAudioProcessorEditor::setError (const juce::String& text)
{
    if (libraryTab != nullptr)
        libraryTab->setError (text);
    if (text.isNotEmpty())
        DBG ("Onorab UI error: " << text);
}

void OnorabAudioProcessorEditor::setProgress (const juce::String& text)
{
    if (libraryTab != nullptr)
        libraryTab->setProgress (text);
}

void OnorabAudioProcessorEditor::refreshFiles()
{
    setProgress ("Refreshing Cloud Library…");
    hub.listFiles ([this] (bool ok, juce::String error, std::vector<CloudFile> newFiles)
    {
        if (ok)
        {
            // Order rule (user decision 2026-11-03): files arrive in Hub
            // creation order and are NEVER re-sorted here. Library and
            // Projects filters preserve this order, so an invited
            // user's upload appears at the end, in order.
            files = std::move (newFiles);
            if (libraryTab != nullptr)
            {
                libraryTab->setFiles (files);
                libraryTab->setError ("");
            }
            setProgress ("Loaded " + juce::String (files.size()) + " file(s) from the Hub.");
            refreshProjectsTab();
        }
        else
        {
            setError ("Could not load the Cloud Library: " + error);
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
    if (libraryTab != nullptr)
        libraryTab->setSelectedLocalFile (selectedLocalFile, selectedLocalExists);
    if (selectedLocalExists)
        loadPreview (selectedLocalFile);
    else
        setProgress ("Take from Drive to fetch \"" + file.name + "\" to this computer.");
}

void OnorabAudioProcessorEditor::takeSelectedFiles()
{
    std::vector<CloudFile> wanted;
    if (libraryTab != nullptr)
        wanted = libraryTab->getSelectedFiles();
    if (wanted.empty() && hasSelection)
        wanted.push_back (selectedFile);
    if (wanted.empty())
    {
        setError ("Select one or more files in the Cloud Library first, then Take from Drive.");
        return;
    }

    // Shared completion state: downloads run in parallel on HubClient
    // background threads, but every callback lands on the message
    // thread, so this counter needs no lock.
    struct TakeState
    {
        int total = 0;
        int done = 0;
        juce::StringArray taken;
        juce::StringArray failed;
        bool previewLoaded = false;
    };
    auto state = std::make_shared<TakeState>();
    state->total = static_cast<int> (wanted.size());
    setProgress ("Taking " + juce::String (state->total) + " file(s) from Drive…");

    for (const auto& file : wanted)
    {
        hub.downloadFile (file, audioProcessor.getCacheDir(),
            [this, file, state] (bool ok, juce::String error, juce::File localFile)
            {
                if (ok)
                {
                    state->taken.add (localFile.getFullPathName());
                    if (! state->previewLoaded)
                    {
                        state->previewLoaded = true;
                        selectedLocalFile = localFile;
                        selectedLocalExists = true;
                        if (libraryTab != nullptr)
                            libraryTab->setSelectedLocalFile (localFile, true);
                        loadPreview (localFile);
                    }
                }
                else
                {
                    state->failed.add (file.name + ": " + error);
                }
                if (++state->done >= state->total)
                {
                    if (libraryTab != nullptr)
                        libraryTab->setTakenLocalFiles (state->taken);
                    if (state->failed.isEmpty())
                    {
                        setProgress ("Took " + juce::String (state->taken.size())
                                      + " file(s) — drag the violet box into FL Studio.");
                        setError ("");
                    }
                    else
                    {
                        setError ("Some sounds could not be taken: " + state->failed.joinIntoString (", "));
                        setProgress ("Took " + juce::String (state->taken.size())
                                     + " of " + juce::String (state->total) + " file(s).");
                    }
                    refreshProjectsTab();
                }
            });
    }
}

void OnorabAudioProcessorEditor::uploadLocalFile (const juce::File& file)
{
    uploadLocalFile (file, {});
}

void OnorabAudioProcessorEditor::uploadLocalFile (const juce::File& file, const juce::String& project)
{
    if (! file.existsAsFile())
    {
        setError ("Upload failed: file does not exist: " + file.getFullPathName());
        return;
    }
    const juce::String folder = project.isNotEmpty() ? (" to " + HubClient::projectFolder (project)) : juce::String {};
    setProgress ("Uploading \"" + file.getFileName() + "\"" + folder + "…");
    hub.uploadFile (file,
        [this, file, folder] (bool ok, juce::String error)
        {
            if (ok)
            {
                setProgress ("Uploaded: " + file.getFileName() + folder);
                setError ("");
                refreshFiles();
                refreshTransfers (false);
            }
            else
            {
                setError ("Upload failed: " + error);
                setProgress ("");
            }
        },
        nullptr, project);
}

void OnorabAudioProcessorEditor::uploadFilesToProject (const std::vector<juce::File>& files,
                                                       const juce::String& project)
{
    if (project.isEmpty())
    {
        setError ("Enter a project name first (e.g. marcus music) — uploads go to Onorab_projet/<project>/.");
        return;
    }
    if (files.empty())
    {
        setError ("No file chosen — pick one or more audio files to upload to this project.");
        return;
    }
    // Each uploadLocalFile reports its own completion and refreshes the
    // list; no synchronous "done" message here, because the uploads
    // themselves are asynchronous — a counter decremented here would
    // fire before any upload actually finished.
    setProgress ("Uploading " + juce::String (files.size()) + " file(s) to "
                 + HubClient::projectFolder (project) + "…");
    for (const auto& f : files)
        uploadLocalFile (f, project);
}

void OnorabAudioProcessorEditor::pushProjectFiles (const std::vector<CloudFile>& files,
                                                   const juce::String& project)
{
    if (files.empty())
    {
        setError ("Select a song in this project first, then Push.");
        return;
    }
    // Target folder: the typed project name wins; otherwise the first
    // selected sound's own project (re-pushing it into its own folder
    // is a no-op target, so an empty target is still an error).
    juce::String target = project;
    if (target.isEmpty())
        target = files.front().projectId;
    if (target.isEmpty())
    {
        setError ("Enter a project name first (e.g. marcus music) — Push goes to Onorab_projet/<project>/.");
        return;
    }

    struct PushState
    {
        int total = 0;
        int done = 0;
        int pushed = 0;
        juce::StringArray skipped;
    };
    auto state = std::make_shared<PushState>();
    std::vector<juce::File> toPush;
    for (const auto& f : files)
    {
        const juce::File local = audioProcessor.getCacheDir()
                                     .getChildFile (HubClient::sanitiseFileName (f.name));
        if (local.existsAsFile())
            toPush.push_back (local);
        else
            state->skipped.add (f.name);
    }
    if (toPush.empty())
    {
        setError ("Nothing to Push — none of the selected sounds is local yet. "
                  "Take them from the Cloud Library first, then Push.");
        return;
    }
    state->total = static_cast<int> (toPush.size());
    setProgress ("Pushing " + juce::String (state->total) + " file(s) to "
                 + HubClient::projectFolder (target) + "…");
    for (const auto& local : toPush)
    {
        hub.uploadFile (local,
            [this, target, state] (bool ok, juce::String error)
            {
                if (ok)
                    ++state->pushed;
                else
                    state->skipped.add (error);
                if (++state->done >= state->total)
                {
                    if (state->skipped.isEmpty())
                    {
                        setProgress ("Pushed " + juce::String (state->pushed)
                                      + " file(s) to " + HubClient::projectFolder (target) + ".");
                        setError ("");
                    }
                    else
                    {
                        setError ("Push skipped: " + state->skipped.joinIntoString (", "));
                        setProgress ("Pushed " + juce::String (state->pushed)
                                     + " of " + juce::String (state->total) + " file(s).");
                    }
                    refreshFiles();
                    refreshTransfers (false);
                }
            },
            nullptr, target);
    }
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
    setProgress ("Deleting " + juce::String (state->total) + " file(s)…");
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
                {
                    hasSelection = false;
                    selectedLocalExists = false;
                    if (libraryTab != nullptr)
                        libraryTab->setSelectedLocalFile (juce::File {}, false);
                }
                refreshFiles();
            }
        });
    }
}

void OnorabAudioProcessorEditor::connectDrive (const juce::String& email)
{
    if (settingsTab == nullptr)
        return;
    if (email.isEmpty() || ! email.containsChar ('@'))
    {
        settingsTab->setDriveStatus ("Enter a Gmail address first (it must contain '@').");
        return;
    }
    settingsTab->setDriveStatus ("Checking Hub Drive status for " + email + "…");
    hub.getAuthStatus ([this, email] (bool ok, juce::String error, juce::String detail)
    {
        if (settingsTab == nullptr)
            return;
        if (! ok)
        {
            settingsTab->setDriveStatus ("Drive check failed: " + error);
            return;
        }
        if (detail.contains ("authenticated: 1") || detail.contains ("authenticated: true"))
        {
            settingsTab->setDriveStatus ("Drive connected (" + detail + ").");
            return;
        }
        // Not connected yet: ask the Hub to start auth. The Hub returns
        // a Google sign-in URL; the plugin opens it in the user's
        // browser (Google's own page — the plugin never sees a
        // password). In Mock mode this returns oauth_unavailable,
        // reported honestly, not faked into a login. Commercial note:
        // every PC signs in with its OWN Gmail; the token stays on
        // that PC's Credential Manager, so a buyer never uses the
        // seller's Google account.
        hub.startAuth ([this, email] (bool authOk, juce::String authError, juce::String authUrl)
        {
            if (settingsTab == nullptr)
                return;
            if (! authOk && authError.containsIgnoreCase ("oauth_unavailable"))
            {
                settingsTab->setDriveStatus ("Drive is not connected yet — the Hub is in Mock mode. "
                                             "Start the Hub with ONORAB_DRIVE_PROVIDER=drive, then press Connect Drive again.");
            }
            else if (authOk && authUrl.startsWith ("https://"))
            {
                juce::URL (authUrl).launchInDefaultBrowser();
                settingsTab->setDriveStatus ("Browser opened — sign in with YOUR Gmail (" + email + "), "
                                             "approve drive.file access, then press Refresh in Cloud Library. "
                                             "Your token stays on this PC only.");
            }
            else if (authOk)
            {
                settingsTab->setDriveStatus ("Drive sign-in started — follow the browser prompt, then press Refresh.");
            }
            else
            {
                settingsTab->setDriveStatus ("Drive connect failed: " + authError);
            }
        });
    });
}

void OnorabAudioProcessorEditor::inviteToProject (const juce::String& email, const juce::String& project)
{
    if (settingsTab == nullptr)
        return;
    if (email.isEmpty() || ! email.containsChar ('@'))
    {
        settingsTab->setInviteStatus ("Enter an invited Gmail address first (it must contain '@').");
        return;
    }
    if (project.isEmpty())
    {
        settingsTab->setInviteStatus ("Enter a project name to invite someone to.");
        return;
    }
    settingsTab->setInviteStatus ("Recording invite for " + email + "…");
    hub.sendInvite (email, project, [this, email, project] (bool ok, juce::String error)
    {
        if (settingsTab == nullptr)
            return;
        if (ok)
            settingsTab->setInviteStatus ("Invite recorded for " + email + " on project " + project
                                          + ". In Drive mode this also creates a real Drive writer permission on "
                                          + HubClient::projectFolder (project)
                                          + "; in Mock mode it is a local record only.");
        else
            settingsTab->setInviteStatus ("Invite failed: " + error);
    });
}

void OnorabAudioProcessorEditor::uploadLastCapture()
{
    const juce::File capture = audioProcessor.getLastCaptureFile();
    if (! capture.existsAsFile())
    {
        setError ("No capture yet — press “Capture from FL”, let audio play through "
                  "the Mixer Track, then press “Stop capture” first.");
        return;
    }
    uploadLocalFile (capture);
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

void OnorabAudioProcessorEditor::playPreview()
{
    if (previewSource == nullptr)
    {
        setError ("No preview loaded — take a file from the Cloud Library first.");
        return;
    }
    transport.start();
    setError ("");
}

void OnorabAudioProcessorEditor::pausePreview()
{
    // stop() pauses without resetting the playhead; Stop resets it.
    transport.stop();
}

void OnorabAudioProcessorEditor::stopPreview()
{
    transport.stop();
    transport.setPosition (0.0);
}

void OnorabAudioProcessorEditor::setPreviewGain (float gain)
{
    previewGain = gain;
    transport.setGain (gain);
}

void OnorabAudioProcessorEditor::toggleCapture()
{
    if (audioProcessor.isCapturing())
    {
        // stopCapture joins the writer thread, so the finished callback
        // (queued below) is already posted by the time this returns.
        audioProcessor.stopCapture();
        if (libraryTab != nullptr)
            libraryTab->setCapturing (false);
        return;
    }

    const auto now = juce::Time::getCurrentTime();
    // Bus capture naming: a typed track name becomes the file prefix
    // (sanitised, so it can never escape the cache dir), followed by
    // the timestamp so two captures of the same track never overwrite
    // each other. Empty name = the old auto name.
    juce::String fileName;
    if (libraryTab != nullptr && libraryTab->getCaptureName().isNotEmpty())
        fileName = HubClient::sanitiseFileName (libraryTab->getCaptureName())
                 + "-" + now.formatted ("%Y%m%d-%H%M%S") + ".wav";
    if (fileName.isEmpty())
        fileName = "capture-" + now.formatted ("%Y%m%d-%H%M%S") + ".wav";

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
    if (libraryTab != nullptr)
    {
        libraryTab->setCapturing (true);
        libraryTab->setCaptureNote ("Capturing Mixer Track audio to " + fileName + "…");
    }
    setError ("");
}

void OnorabAudioProcessorEditor::onCaptureFinished (const juce::File& file, const juce::String& error)
{
    if (libraryTab != nullptr)
        libraryTab->setCapturing (false);
    if (error.isNotEmpty())
    {
        setError ("Capture failed: " + error);
        return;
    }
    const int dropped = audioProcessor.getDroppedSamples();
    const int64_t samples = audioProcessor.getCapturedSamples();
    juce::String note = "Capture saved: " + file.getFileName()
                      + " (" + juce::String (samples) + " samples";
    if (dropped > 0)
        note += ", " + juce::String (dropped) + " samples dropped (writer fell behind)";
    note += ") — press “Upload last capture” to send it to the Hub.";
    if (libraryTab != nullptr)
        libraryTab->setCaptureNote (note);
    setProgress ("Capture ready: " + file.getFileName());
    refreshProjectsTab();
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
        settingsTab->setStatus ("Saved — Hub URL and cache folder updated (saved with the project).");
    checkHealth();
    refreshFiles();
}

void OnorabAudioProcessorEditor::checkHealth()
{
    hub.checkHealth ([this] (bool ok, juce::String error, juce::String /*detail*/)
    {
        hubConnected = ok;
        if (libraryTab != nullptr)
            libraryTab->setSyncStatus (ok ? "Connected" : "Disconnected — Hub not reachable", ok);
        if (! ok)
            DBG ("Onorab Hub health check failed: " << error);
    });
}

void OnorabAudioProcessorEditor::refreshTransfers (bool silent)
{
    hub.listTransfers ([this, silent] (bool ok, juce::String error, std::vector<TransferInfo> newTransfers)
    {
        if (ok)
        {
            transfers = std::move (newTransfers);
            if (transfersTab != nullptr)
            {
                transfersTab->setTransfers (transfers);
                transfersTab->setNote (juce::String (transfers.size()) + " transfer(s) from the Hub.");
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
    // Projects now show the Hub file list (with projectId), not the
    // local cache contents — the cache is only the Take/Push staging
    // area, not the project model.
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

bool OnorabAudioProcessorEditor::hasLastCapture() const
{
    return audioProcessor.getLastCaptureFile().existsAsFile();
}

juce::File OnorabAudioProcessorEditor::getLastCaptureFile() const
{
    return audioProcessor.getLastCaptureFile();
}

juce::File OnorabAudioProcessorEditor::getSelectedLocalFile() const
{
    return selectedLocalFile;
}
