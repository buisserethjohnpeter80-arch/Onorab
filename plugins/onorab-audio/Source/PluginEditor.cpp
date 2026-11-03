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
    label.setFont (juce::Font (size));
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
    if (isDragging || getFileToDrag == nullptr)
        return;

    const juce::File file = getFileToDrag();
    if (! file.existsAsFile())
        return; // Nothing downloaded yet: there is no real file to hand out.

    auto* container = findParentComponentOfClass<juce::DragAndDropContainer>();
    if (container == nullptr)
        return;

    // Hand the host a real file path. Whether FL Studio accepts the drop
    // is the host's decision (see the header note); this call is the
    // plugin's side of the contract, and it blocks until the drag ends.
    isDragging = true;
    container->performExternalDragDropOfFiles (juce::StringArray { file.getFullPathName() }, false);
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
    dragHintLabel.setText ("Tip: download a file first, then drag the violet box into FL Studio.",
                           juce::NotificationType::dontSendNotification);
    addAndMakeVisible (dragHintLabel);

    fileList.setRowHeight (22);
    addAndMakeVisible (fileList);

    addAndMakeVisible (waveform);

    styleButton (refreshButton);
    refreshButton.onClick = [this] { owner.refreshFiles(); };
    addAndMakeVisible (refreshButton);

    styleButton (downloadButton, kCyan);
    downloadButton.onClick = [this] { owner.downloadSelectedFile(); };
    addAndMakeVisible (downloadButton);

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

    dragSource.getFileToDrag = [this]
    {
        return selectedLocalExists ? selectedLocalFile : juce::File {};
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

void LibraryTab::updateDragHint()
{
    dragHintLabel.setText (selectedLocalExists
                               ? "Ready to drag: " + selectedLocalFile.getFileName()
                               : "Tip: download a file first, then drag the violet box into FL Studio.",
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
    downloadButton.setBounds (buttonRow1.removeFromLeft (110));
    buttonRow1.removeFromLeft (6);
    uploadButton.setBounds (buttonRow1.removeFromLeft (110));
    buttonRow1.removeFromLeft (6);
    uploadCaptureButton.setBounds (buttonRow1.removeFromLeft (170));

    area.removeFromTop (6);
    fileList.setBounds (area.removeFromTop (130));
    area.removeFromTop (6);
    waveform.setBounds (area.removeFromTop (90));
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

    area.removeFromTop (6);
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
    g.setColour (juce::Colours::white);
    g.setFont (13.0f);
    g.drawText (file.name, 6, 0, width - 110, height, juce::Justification::left, true);
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
    infoLabel.setText ("Projects — Phase 1: project grouping is not synced to the Hub yet. "
                       "Use a name below to organise your own notes; the local cache "
                       "contents are listed underneath so you can see what is on disk.",
                       juce::NotificationType::dontSendNotification);
    infoLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    infoLabel.setFont (juce::Font (13.0f));
    infoLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (infoLabel);

    projectNameBox.setTextToShowWhenEmpty ("Project name (local note only, Phase 1)", juce::Colours::grey);
    addAndMakeVisible (projectNameBox);

    projectNoteLabel.setText ("Project names are not uploaded in Phase 1 — this is a placeholder "
                              "for the Phase 2 project model.",
                              juce::NotificationType::dontSendNotification);
    projectNoteLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    projectNoteLabel.setFont (juce::Font (12.0f));
    addAndMakeVisible (projectNoteLabel);

    cacheBox.setReadOnly (true);
    cacheBox.setMultiLine (true);
    cacheBox.setTextToShowWhenEmpty ("No cached files yet.", juce::Colours::grey);
    addAndMakeVisible (cacheBox);
    (void) owner;
}

void ProjectsTab::setCachedFiles (const std::vector<juce::String>& names)
{
    juce::String text;
    for (const auto& n : names)
        text += n + "\n";
    cacheBox.setText (text);
}

void ProjectsTab::resized()
{
    auto area = getLocalBounds().reduced (8);
    infoLabel.setBounds (area.removeFromTop (60));
    area.removeFromTop (6);
    projectNameBox.setBounds (area.removeFromTop (26));
    area.removeFromTop (4);
    projectNoteLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (6);
    cacheBox.setBounds (area);
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
    noteLabel.setFont (juce::Font (12.0f));
    noteLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (noteLabel);

    styleLabel (statusLabel, kGreen);
    addAndMakeVisible (statusLabel);
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
        setProgress ("Select Download to fetch \"" + file.name + "\" to this computer.");
}

void OnorabAudioProcessorEditor::downloadSelectedFile()
{
    if (! hasSelection)
    {
        setError ("Select a file in the Cloud Library first.");
        return;
    }
    const CloudFile file = selectedFile;
    setProgress ("Downloading \"" + file.name + "\"…");
    auto lastPosted = std::make_shared<double> (-1.0);
    hub.downloadFile (file, audioProcessor.getCacheDir(),
        [this, file] (bool ok, juce::String error, juce::File localFile)
        {
            if (ok)
            {
                selectedLocalFile = localFile;
                selectedLocalExists = true;
                if (libraryTab != nullptr)
                    libraryTab->setSelectedLocalFile (localFile, true);
                loadPreview (localFile);
                setProgress ("Downloaded: " + file.name);
                setError ("");
                refreshProjectsTab();
            }
            else
            {
                setError ("Download failed: " + error);
                setProgress ("");
            }
        },
        [this, lastPosted] (double progress)
        {
            // Throttle message-thread hops: post on >=10% steps or completion.
            if (progress < 0.0)
                return;
            if (progress - *lastPosted >= 0.1 || progress >= 1.0)
            {
                *lastPosted = progress;
                juce::MessageManager::callAsync ([this, progress]
                {
                    setProgress ("Downloading… " + juce::String (static_cast<int> (progress * 100.0)) + "%");
                });
            }
        });
}

void OnorabAudioProcessorEditor::uploadLocalFile (const juce::File& file)
{
    if (! file.existsAsFile())
    {
        setError ("Upload failed: file does not exist: " + file.getFullPathName());
        return;
    }
    setProgress ("Uploading \"" + file.getFileName() + "\"…");
    hub.uploadFile (file,
        [this, file] (bool ok, juce::String error)
        {
            if (ok)
            {
                setProgress ("Uploaded: " + file.getFileName());
                setError ("");
                refreshFiles();
                refreshTransfers (false);
            }
            else
            {
                setError ("Upload failed: " + error);
                setProgress ("");
            }
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
        setError ("No preview loaded — download a file from the Cloud Library first.");
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
    const juce::File dir = audioProcessor.getCacheDir();
    std::vector<juce::String> names;
    if (dir.isDirectory())
    {
        for (const auto& f : dir.findChildFiles (juce::File::findFiles, false))
            names.push_back (f.getFileName());
    }
    if (projectsTab != nullptr)
        projectsTab->setCachedFiles (names);
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
