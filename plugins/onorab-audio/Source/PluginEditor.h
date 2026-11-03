#pragma once

// OnorabAudioProcessorEditor — the plugin UI.
//
// Layout: the editor is a DragAndDropContainer wrapper holding a
// TabbedComponent with four tabs (Cloud Library, Projects, Transfers,
// Settings). All tabs are declared here and implemented in PluginEditor.cpp.
//
// Threading: every UI update happens on the message thread. Hub results
// arrive via HubClient callbacks (already on the message thread) or, for
// capture completion, via MessageManager::callAsync from the writer thread.
// The audio thread is never touched from here except through the
// processor's atomic capture flags (start/stop/isCapturing).
//
// Drag-and-drop, honestly stated:
//   * Drag OUT: the "Drag to FL" box in the Cloud Library calls
//     DragAndDropContainer::performExternalDragDropOfFiles() with the
//     downloaded file's path. Whether FL Studio accepts that external
//     file drag depends on the host — FL Studio's Playlist/Channel Rack
//     accept dropped audio files, but the plugin cannot force the host
//     to accept it. The plugin's job is to hand the host a real file path.
//   * Drop IN: dropping audio files onto the editor queues them for
//     upload to the Hub (FileDragAndDropTarget, implemented on the editor).

#include <JuceHeader.h>
#include <memory>
#include <vector>
#include "HubClient.h"
#include "WaveformView.h"

class OnorabAudioProcessor;
class OnorabAudioProcessorEditor;

// Small drag-source box: the user drags FROM this box INTO FL Studio.
// It is a plain Component (not a button) because an OS file drag must
// start from a mouse-drag gesture, not a click.
class DragSourceComponent : public juce::Component
{
public:
    // Returns the local files to drag (one or more taken sounds), or an
    // empty array if none is ready yet. FL Studio receives real file
    // paths; whether it accepts the drop is the host's decision.
    std::function<juce::StringArray()> getFilesToDrag;

    void paint (juce::Graphics& g) override;
    void mouseDrag (const juce::MouseEvent& event) override;

private:
    bool isDragging = false;
};

// ---- Tab components (implemented in PluginEditor.cpp) ----

class LibraryTab : public juce::Component,
                   public juce::ListBoxModel,
                   private juce::TextEditor::Listener
{
public:
    explicit LibraryTab (OnorabAudioProcessorEditor& editor);
    ~LibraryTab() override;

    // Data / state setters called by the editor (message thread).
    void setFiles (const std::vector<CloudFile>& files);
    void setSyncStatus (const juce::String& text, bool connected);
    void setError (const juce::String& text);
    void setProgress (const juce::String& text);
    void setCapturing (bool capturing);
    void setCaptureNote (const juce::String& text);
    void setSelectedLocalFile (const juce::File& file, bool exists);
    // Typed capture name for the next Capture from FL ("" = auto name).
    juce::String getCaptureName() const;
    // Files the user selected in this list (multi-select enabled).
    std::vector<CloudFile> getSelectedFiles() const;
    // Local files produced by Take from Drive; these are what the
    // violet drag box hands to FL Studio.
    void setTakenLocalFiles (const juce::StringArray& files);

    void resized() override;

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height,
                           bool rowIsSelected) override;
    void selectedRowsChanged (int lastRowSelected) override;

private:
    void textEditorTextChanged (juce::TextEditor& editor) override;
    void applySearchFilter();
    void updateDragHint();

    OnorabAudioProcessorEditor& owner;
    std::vector<CloudFile> allFiles;
    std::vector<CloudFile> filteredFiles;

    juce::ListBox fileList { "Cloud Library", this };
    juce::TextEditor searchBox;
    juce::Label syncLabel, errorLabel, progressLabel, captureLabel, dragHintLabel;
    juce::TextButton refreshButton { "Refresh" };
    // No separate Download button by design (user decision 2026-11-03):
    // Take from Drive downloads the selected sound(s) to the local
    // cache in one step, ready to preview and to drag into FL Studio.
    juce::TextButton takeButton { "Take from Drive" };
    juce::TextButton deleteButton { "Delete" };
    juce::TextButton uploadButton { "Upload…" };
    juce::TextButton uploadCaptureButton { "Upload last capture" };
    juce::TextButton playButton { "Play" };
    juce::TextButton pauseButton { "Pause" };
    juce::TextButton stopButton { "Stop" };
    juce::TextButton captureButton { "Capture from FL" };
    juce::TextEditor captureNameBox;
    juce::Label busGuideLabel;
    juce::Slider gainSlider;
    juce::Label gainLabel;
    juce::AudioFormatManager waveformFormatManager;
    WaveformView waveform;
    DragSourceComponent dragSource;
    juce::File selectedLocalFile;
    bool selectedLocalExists = false;
    juce::StringArray takenLocalFiles;
    bool capturing = false;

    std::unique_ptr<juce::FileChooser> fileChooser;
};

// Projects tab: one project, track by track. The project name box is
// BOTH the filter (the list shows only this project's sounds) and the
// target for Upload / Push, which file sounds under
// Onorab_projet/<project>/ on Drive. FL Studio cannot list its own
// project tracks automatically (a VST sees only its Mixer Track audio),
// so local sounds enter a project only via Upload one / Upload all /
// Push — never by silent auto-discovery.
class ProjectsTab : public juce::Component,
                    public juce::ListBoxModel,
                    private juce::TextEditor::Listener
{
public:
    explicit ProjectsTab (OnorabAudioProcessorEditor& editor);
    void setFiles (const std::vector<CloudFile>& files);
    std::vector<CloudFile> getSelectedFiles() const;
    juce::String getProjectName() const;
    void resized() override;

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height,
                           bool rowIsSelected) override;
    void selectedRowsChanged (int lastRowSelected) override;

private:
    void textEditorTextChanged (juce::TextEditor& editor) override;
    void applyFilter();

    OnorabAudioProcessorEditor& owner;
    std::vector<CloudFile> allFiles;
    std::vector<CloudFile> filteredFiles;
    juce::Label infoLabel;
    juce::TextEditor projectNameBox;
    juce::TextEditor searchBox;
    juce::ListBox fileList { "Project sounds", this };
    juce::TextButton uploadOneButton { "Upload one" };
    juce::TextButton uploadAllButton { "Upload all" };
    juce::TextButton pushButton { "Push" };
    juce::TextButton deleteButton { "Delete" };
    juce::Label noteLabel;
    std::unique_ptr<juce::FileChooser> fileChooser;
};

class TransfersTab : public juce::Component,
                     public juce::ListBoxModel
{
public:
    explicit TransfersTab (OnorabAudioProcessorEditor& editor);
    void setTransfers (const std::vector<TransferInfo>& transfers);
    void setNote (const juce::String& text);
    void resized() override;

    int getNumRows() override;
    void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height,
                           bool rowIsSelected) override;

private:
    OnorabAudioProcessorEditor& owner;
    std::vector<TransferInfo> transfers;
    juce::ListBox transferList { "Transfers", this };
    juce::Label noteLabel;
};

class SettingsTab : public juce::Component
{
public:
    explicit SettingsTab (OnorabAudioProcessorEditor& editor);
    void setValues (const juce::String& hubUrl, const juce::String& cacheDir);
    void setStatus (const juce::String& text);
    void setDriveStatus (const juce::String& text);
    void setInviteStatus (const juce::String& text);
    void resized() override;

private:
    OnorabAudioProcessorEditor& owner;
    juce::Label hubUrlLabel, cacheDirLabel, noteLabel, statusLabel;
    juce::TextEditor hubUrlBox, cacheDirBox;
    juce::TextButton saveButton { "Save" };
    juce::TextButton browseButton { "Browse…" };
    std::unique_ptr<juce::FileChooser> dirChooser;
    // Google Drive connect (Hub-side; real Google sign-in is Phase 2).
    juce::Label driveLabel;
    juce::TextEditor driveEmailBox;
    juce::TextButton connectDriveButton { "Connect Drive" };
    juce::Label driveStatusLabel;
    // Invite another user to a project (Hub-side record; real Drive
    // sharing of Onorab_projet/<project>/ is Phase 2).
    juce::Label inviteLabel;
    juce::TextEditor inviteEmailBox, inviteProjectBox;
    juce::TextButton inviteButton { "Invite" };
    juce::Label inviteStatusLabel;
};

// ---- The editor itself ----

class OnorabAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   public juce::DragAndDropContainer,
                                   private juce::Timer,
                                   public juce::FileDragAndDropTarget
{
public:
    explicit OnorabAudioProcessorEditor (OnorabAudioProcessor& processor);
    ~OnorabAudioProcessorEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    // FileDragAndDropTarget: drop audio files onto the editor to upload.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    // Called by the tabs (message thread).
    void refreshFiles();
    void onFileSelected (const CloudFile& file);
    // Take from Drive: download every selected Library sound to the
    // local cache, load the first as preview, and make all of them
    // draggable into FL Studio. This replaces the old single Download.
    void takeSelectedFiles();
    void uploadLocalFile (const juce::File& file);
    void uploadLocalFile (const juce::File& file, const juce::String& project);
    // Upload picked local files into a project folder.
    void uploadFilesToProject (const std::vector<juce::File>& files, const juce::String& project);
    // Push: upload the LOCAL copies of selected project sounds into
    // the project folder (Onorab_projet/<project>/). A sound with no
    // local copy yet is skipped with a visible message — Take it first.
    void pushProjectFiles (const std::vector<CloudFile>& files, const juce::String& project);
    // Delete: remove selected sounds from the Hub (Drive) AND their
    // local cache copies (local copy only on Hub success, so a failed
    // Hub delete keeps the recoverable local file). Permanent on the
    // Hub side — in Mock mode a Hub restart re-seeds the 3 mock files.
    void deleteHubFiles (const std::vector<CloudFile>& files);
    // Settings: Drive connect + project invite (Hub-side, Phase 2 for
    // real Google sign-in / Drive sharing; see SettingsTab notes).
    void connectDrive (const juce::String& email);
    void inviteToProject (const juce::String& email, const juce::String& project);
    void uploadLastCapture();
    void playPreview();
    void pausePreview();
    void stopPreview();
    void setPreviewGain (float gain);
    void toggleCapture();
    void applySettings (const juce::String& hubUrl, const juce::File& cacheDir);

    // Read by the tabs.
    juce::File getCacheDir() const;
    juce::String getHubUrl() const;
    bool isCapturing() const;
    bool hasLastCapture() const;
    juce::File getLastCaptureFile() const;
    juce::File getSelectedLocalFile() const;

private:
    void timerCallback() override;
    void checkHealth();
    void refreshTransfers (bool silent);
    void loadPreview (const juce::File& file);
    void onCaptureFinished (const juce::File& file, const juce::String& error);
    void setError (const juce::String& text);
    void setProgress (const juce::String& text);
    void refreshProjectsTab();

    OnorabAudioProcessor& audioProcessor;

    HubClient hub;
    HubClient::AliveToken aliveToken = std::make_shared<std::atomic<bool>> (true);

    juce::AudioFormatManager formatManager;
    juce::AudioTransportSource transport;
    juce::TimeSliceThread readAheadThread { "Onorab preview read-ahead" };
    std::unique_ptr<juce::AudioFormatReaderSource> previewSource;

    std::vector<CloudFile> files;
    CloudFile selectedFile;
    bool hasSelection = false;
    juce::File selectedLocalFile;
    bool selectedLocalExists = false;
    std::vector<TransferInfo> transfers;
    bool hubConnected = false;
    float previewGain = 0.8f;

    // Tab components are owned by the TabbedComponent; these raw pointers
    // are only used while the editor is alive (the tabs die with it).
    LibraryTab* libraryTab = nullptr;
    ProjectsTab* projectsTab = nullptr;
    TransfersTab* transfersTab = nullptr;
    SettingsTab* settingsTab = nullptr;
    juce::TabbedComponent tabbedComponent { juce::TabbedButtonBar::TabsAtTop };
    // NOTE (JUCE 8 fix, 2026-11-03): DragAndDropContainer is NOT a
    // Component in JUCE 8, so it must be a BASE CLASS of the editor,
    // not a member — a member cannot host children (no addAndMakeVisible
    // / setBounds) and findParentComponentOfClass<DragAndDropContainer>
    // would never find it. The editor itself is the container.

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OnorabAudioProcessorEditor)
};
