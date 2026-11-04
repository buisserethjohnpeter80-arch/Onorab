#pragma once

// OnorabAudioProcessorEditor - the plugin UI (redesign 2026-11-04).
//
// Reference: the "Interface premium du plugin Onorab" mockup (black
// background, readable text, light purple for selection and primary
// actions). The mockup shows several screens at once for presentation;
// the real plugin shows ONE tab at a time: Library, Projects,
// Transfers, Settings. A persistent header above the tabs holds the
// title, the Hub / Drive status and the Capture from FL button.
//
// What the mockup does NOT contribute: its sounds, projects, durations,
// waveforms, "Phone" labels and statuses are examples only and are
// NEVER shown here. Every list shows real Hub data only. Empty lists
// show an honest empty state ("No sounds yet" / "No projects yet" /
// "No transfers yet") with a useful action, never placeholder content.
// "Ready for FL" appears only for a file that really exists in the
// local cache; "Drive connected" appears only after the Hub confirms
// authentication; "Uploaded" appears only after a transfer succeeds.
//
// Threading: every UI update happens on the message thread. Hub results
// arrive via HubClient callbacks (already on the message thread) or,
// for capture completion, via MessageManager::callAsync from the writer
// thread. The audio thread is never touched from here except through
// the processor's atomic capture flags (start/stop/isCapturing).
//
// Drag-and-drop, honestly stated:
//   * Drag OUT: the grip on a "Ready for FL" Library row calls
//     DragAndDropContainer::performExternalDragDropOfFiles() with the
//     downloaded file's path. Whether FL Studio accepts that external
//     file drag depends on the host - FL Studio's Playlist / Channel
//     Rack accept dropped audio files, but the plugin cannot force the
//     host to accept it. The plugin's job is to hand the host a real
//     file path. Rows for sounds that are not local yet have a
//     disabled (grey) grip: nothing to drag until the download
//     finishes.
//   * Drop IN: dropping audio files onto the editor queues them for
//     upload to the Hub (FileDragAndDropTarget, implemented on the
//     editor).
//
// Text encoding: ALL user-visible strings in this file and in
// PluginEditor.cpp are plain ASCII on purpose. The plugin font on some
// systems lacks glyphs for the ellipsis, em dash and middle dot, which
// previously rendered as mojibake ("a" with accents). CMake also passes
// /utf-8 to MSVC, but ASCII-only strings make the fix independent of
// any compiler flag.

#include <JuceHeader.h>
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include "HubClient.h"
#include "WaveformView.h"

class OnorabAudioProcessor;
class OnorabAudioProcessorEditor;

// Small round play / pause button drawn with vector shapes (no icon
// font dependency): a circle with a triangle, or two bars when it
// represents pause. Clicking fires onClick.
class IconButton : public juce::Component
{
public:
    std::function<void()> onClick;
    void setPauseIcon (bool isPause);
    void setStopIcon (bool isStop);
    void paint (juce::Graphics& g) override;
    void mouseUp (const juce::MouseEvent& event) override;

private:
    bool pauseIcon = false;
    bool stopIcon = false;
};

// Drag grip: six dots in two columns. Enabled (purple) only when the
// row's file exists locally; grey and inert otherwise. A mouse drag
// starts an external file drag through the editor's DragAndDropContainer.
class DragGrip : public juce::Component
{
public:
    std::function<juce::StringArray()> getFilesToDrag;
    void setEnabledGrip (bool enabled);
    void paint (juce::Graphics& g) override;
    void mouseDrag (const juce::MouseEvent& event) override;

private:
    bool gripEnabled = false;
    bool isDragging = false;
};

// Small filled status dot (green / grey / red) used in the header and
// in Settings next to the Hub and Drive status texts.
class StatusDot : public juce::Component
{
public:
    void setDotColour (juce::Colour colour);
    void paint (juce::Graphics& g) override;

private:
    juce::Colour dotColour = juce::Colours::grey;
};

// One transfer row for the Transfers tab. `entry` is the editor's local
// transfer index (>= 0, retryable) or -1 for a Hub-side record (not
// retryable from here). `error` is shown only on failure, together
// with the Retry button.
struct TransferRow
{
    int entry = -1;
    juce::String fileName;
    juce::String direction; // "Upload" | "Download" | "" if unknown
    juce::String status;    // "Uploading..." | "Downloading..." | "Done" | "Failed"
    double progress = 0.0;  // 0.0 .. 1.0
    juce::String error;
    bool canRetry = false;
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
    void setFiles (const std::vector<CloudFile>& files,
                   const std::vector<juce::String>& localFileIds);
    void setSelected (const CloudFile& file, bool localExists);
    void clearSelection();
    void setDownloadProgress (const juce::String& fileId, double progress, bool downloading);
    void setPlaying (const juce::String& fileId, bool playing);
    void setMessage (const juce::String& text);
    void setError (const juce::String& text);
    // Player area: name + waveform + duration of the selected sound.
    // durationSeconds < 0 means unknown (sound not local yet).
    void setPlayer (const juce::String& name, const juce::File& localFile,
                    bool localExists, double durationSeconds);
    void setPlayerPosition (double currentSeconds, double totalSeconds);
    void setPlayerPlaying (bool playing);

    void resized() override;

    // ListBoxModel (custom row components; paintListBoxItem is unused
    // but required by the base class).
    int getNumRows() override;
    void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height,
                           bool rowIsSelected) override;
    juce::Component* refreshComponentForRow (int rowNumber, bool isRowSelected,
                                             juce::Component* existingComponentToUpdate) override;
    void selectedRowsChanged (int lastRowSelected) override;

private:
    void textEditorTextChanged (juce::TextEditor& editor) override;
    void applySearchFilter();
    void updateEmptyState();

    OnorabAudioProcessorEditor& owner;
    std::vector<CloudFile> allFiles;
    std::vector<CloudFile> filteredFiles;
    std::vector<juce::String> localIds;
    std::map<juce::String, double> downloadProgress;
    std::map<juce::String, bool> downloading;
    juce::String selectedId;
    juce::String playingId;

    juce::ListBox fileList { "Library", this };
    juce::TextEditor searchBox;
    juce::TextButton refreshButton { "Refresh" };
    juce::TextButton deleteButton { "Delete" };
    juce::Label messageLabel, errorLabel;
    juce::Label emptyLabel;
    juce::TextButton emptyCaptureButton { "Capture from FL" };

    // Player card for the selected sound.
    juce::Label playerNameLabel, playerHintLabel, timeLabel, gainLabel;
    juce::AudioFormatManager waveformFormatManager;
    WaveformView waveform;
    IconButton playerPlayButton;
    IconButton playerStopButton;
    juce::Slider gainSlider;
};

// Projects tab: a project LIST (one row per real project found in the
// Hub file list) and a project DETAIL view (the open project's sounds,
// Auto-capture, Invite, Push, Delete). Projects come only from real
// file.projectId values - never invented. FL Studio cannot list its
// own Mixer Tracks or clips to a plugin (a VST sees only its own
// Mixer Track audio), so sounds enter a project ONLY as audio routed
// to this plugin's Mixer Track: press Capture from FL, or turn
// Auto-capture on for the open project and play the routed tracks.
// There is deliberately no laptop file picker here: the Mixer is the
// source of project sounds.
class ProjectsTab : public juce::Component,
                    public juce::ListBoxModel,
                    private juce::TextEditor::Listener
{
public:
    explicit ProjectsTab (OnorabAudioProcessorEditor& editor);
    void setFiles (const std::vector<CloudFile>& files);
    void setStatus (const juce::String& text, bool isError);
    void setInviteStatus (const juce::String& text);
    std::vector<CloudFile> getDetailSelectedFiles() const;
    juce::String getOpenProject() const { return openProject; }
    // Reflects whether auto-capture is currently running for the open
    // project (the editor owns the state; the tab only displays it).
    void setAutoCaptureState (bool on);
    void resized() override;

    // ListBoxModel: one model serves both lists (project list when no
    // project is open, detail list when one is).
    int getNumRows() override;
    void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height,
                           bool rowIsSelected) override;
    void selectedRowsChanged (int lastRowSelected) override;

private:
    void textEditorTextChanged (juce::TextEditor& editor) override;
    void updateView();
    void applyDetailFilter();

    OnorabAudioProcessorEditor& owner;
    std::vector<CloudFile> allFiles;
    std::vector<juce::String> projectNames;
    std::vector<CloudFile> detailFiles;
    juce::String openProject;

    juce::Label titleLabel, emptyLabel, statusLabel;
    juce::ListBox projectList { "Projects", this };
    juce::ListBox detailList { "Project sounds", this };
    juce::TextButton backButton { "< Projects" };
    juce::Label detailTitleLabel;
    juce::TextEditor detailSearchBox;
    // Auto-capture for the open project: when on, audio routed to
    // this plugin's Mixer Track is recorded in takes and uploaded to
    // this project automatically (see the processor's auto mode).
    juce::TextButton autoCaptureButton { "Auto-capture: Off" };
    juce::Label busHintLabel;
    juce::TextButton inviteButton { "Invite" };
    juce::TextButton pushButton { "Push" };
    juce::TextButton deleteButton { "Delete" };

    // Invite panel: shown in the detail view. It names the project
    // BEFORE the user confirms, so an invite can never go to the
    // wrong project by accident.
    juce::Label inviteProjectLabel, inviteStatusLabel;
    juce::TextEditor inviteEmailBox;
    juce::TextButton inviteSendButton { "Send invite" };
    juce::TextButton inviteCancelButton { "Cancel" };
    bool invitePanelVisible = false;
};

class TransfersTab : public juce::Component,
                     public juce::ListBoxModel
{
public:
    explicit TransfersTab (OnorabAudioProcessorEditor& editor);
    void setRows (const std::vector<TransferRow>& rows);
    void setNote (const juce::String& text);
    void resized() override;

    int getNumRows() override;
    void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height,
                           bool rowIsSelected) override;
    juce::Component* refreshComponentForRow (int rowNumber, bool isRowSelected,
                                             juce::Component* existingComponentToUpdate) override;

private:
    OnorabAudioProcessorEditor& owner;
    std::vector<TransferRow> rows;
    juce::ListBox transferList { "Transfers", this };
    juce::Label noteLabel, emptyLabel;
};

class SettingsTab : public juce::Component
{
public:
    explicit SettingsTab (OnorabAudioProcessorEditor& editor);
    void setValues (const juce::String& hubUrl, const juce::String& cacheDir);
    void setStatus (const juce::String& text);
    // Connected state comes only from the Hub (/auth/status, polled
    // by the editor). `account` is the Gmail the Hub learned from
    // Google at sign-in; it is shown, never typed: there is NO email
    // field in this panel, because the browser sign-in — not a text
    // box — is what chooses the account. Disconnect is offered only
    // while connected.
    void setDriveConnected (bool connected, const juce::String& account = {});
    void setConnectButtonText (const juce::String& text);
    void setDriveMessage (const juce::String& text);
    void resized() override;

private:
    void updateAdvancedVisibility();

    OnorabAudioProcessorEditor& owner;
    // Google Drive section (top). The Hub holds the OAuth flow, not
    // this plugin: Connect asks the Hub for a Google sign-in URL and
    // opens it in the browser. In Mock mode the Hub reports
    // oauth_unavailable honestly instead of faking a login; in Drive
    // mode it starts the real PKCE flow (drive.file only, token in
    // Windows Credential Manager on this PC only). "Drive connected"
    // is shown only after the Hub confirms authentication (the editor
    // polls the Hub status); it is never assumed from the button press.
    // The token persists in the OS keychain, so closing or minimizing
    // the plugin window never asks for sign-in again while it exists.
    juce::Label driveTitleLabel, driveStatusLabel, driveMessageLabel, driveAccountLabel;
    StatusDot driveDot;
    juce::TextButton connectDriveButton { "Connect Google Drive" };
    juce::TextButton disconnectDriveButton { "Disconnect" };
    // Advanced section (collapsed by default): Hub URL + cache folder.
    juce::TextButton advancedButton { "Advanced" };
    bool advancedVisible = false;
    juce::Label hubUrlLabel, cacheDirLabel, savedLabel;
    juce::TextEditor hubUrlBox, cacheDirBox;
    juce::TextButton saveButton { "Save" };
    juce::TextButton browseButton { "Browse..." };
    juce::Label noteLabel;
    std::unique_ptr<juce::FileChooser> dirChooser;
};

// Capture review panel: appears (centred over the tabs) when a Capture
// from FL stops. It shows the captured waveform, an editable name and
// Send to Drive. "Uploaded" is shown ONLY after the Hub confirms the
// upload succeeded; a failure shows the reason and Send stays available
// as Retry. Close keeps the capture file in the cache (it can be
// uploaded later from the Library Upload button); Discard deletes it.
class CaptureReviewPanel : public juce::Component
{
public:
    CaptureReviewPanel (juce::AudioFormatManager& formatManagerToUse,
                        std::function<void (const juce::String&)> onSend,
                        std::function<void()> onClose,
                        std::function<void()> onDiscard);
    void showFor (const juce::File& captureFile);
    void setStatus (const juce::String& text, bool isError);
    void setUploaded();
    void setSending (bool sending);
    void resized() override;

private:
    std::function<void (const juce::String&)> sendCallback;
    std::function<void()> closeCallback;
    std::function<void()> discardCallback;
    juce::Label titleLabel, statusLabel;
    WaveformView waveform;
    juce::TextEditor nameBox;
    juce::TextButton sendButton { "Send to Drive" };
    juce::TextButton closeButton { "Close" };
    juce::TextButton discardButton { "Discard" };
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

    // Called by the tabs and the header (message thread).
    void refreshFiles();
    void onFileSelected (const CloudFile& file);
    void clearSelection();
    void downloadFile (const CloudFile& file);
    void playRowFile (const CloudFile& file);
    void deleteHubFiles (const std::vector<CloudFile>& files);
    void uploadLocalFile (const juce::File& file);
    void uploadLocalFile (const juce::File& file, const juce::String& project);
    void uploadFilesToProject (const std::vector<juce::File>& files, const juce::String& project);
    void pushProjectFiles (const std::vector<CloudFile>& files, const juce::String& project);
    void retryTransfer (int entry);
    // Drive sign-in (no email argument: the browser sign-in chooses
    // the account; the Hub reports it back for display). Disconnect
    // signs out on this PC via the Hub.
    void connectDrive();
    void disconnectDrive();
    // Auto-capture for one project: toggles recording of routed Mixer
    // audio into that project (see the processor's auto mode). Only
    // one project can auto-capture at a time: enabling another stops
    // the first. Empty while off.
    void toggleProjectAutoCapture (const juce::String& project);
    juce::String getAutoCaptureProject() const { return autoCaptureProject; }
    void inviteToProject (const juce::String& email, const juce::String& project);
    void playPreview();
    void pausePreview();
    void stopPreview();
    void togglePlayerPlay();
    void setPreviewGain (float gain);
    void startCapture();
    void stopCapture();
    void sendCaptureToDrive (const juce::String& name);
    void closeCaptureReview();
    void discardCapture();
    void applySettings (const juce::String& hubUrl, const juce::File& cacheDir);

    // Read by the tabs.
    juce::File getCacheDir() const;
    juce::String getHubUrl() const;
    bool isCapturing() const;

private:
    // One transfer launched from this plugin (session-local, always
    // real). `retry` re-runs the same operation on the same entry.
    struct LocalTransfer
    {
        juce::String fileName;
        juce::String direction; // "Upload" | "Download"
        juce::String status;    // "Uploading..." | "Downloading..." | "Done" | "Failed"
        double progress = 0.0;
        double lastReportedProgress = -1.0;
        juce::String error;
        bool canRetry = false;
        std::function<void (int)> retry;
    };

    void timerCallback() override;
    void checkHealth();
    void refreshDriveStatus();
    // Auto-launch for the installed Hub (Windows installer build):
    // when the default localhost Hub does not answer, start the
    // installed onorab-hub.exe in Drive mode in the background, once
    // per editor instance. JUCE ChildProcess launches with
    // CREATE_NO_WINDOW on Windows, so no console window appears, and
    // the child keeps running after FL closes, so reopening FL finds
    // a Hub already running. Multiple plugin instances share the one
    // Hub: the health check succeeds for the second instance, so it
    // never launches another. Custom Hub URLs are never auto-
    // launched (the user owns that Hub).
    void tryLaunchInstalledHub();
    void refreshTransfers (bool silent);
    void refreshProjectsTab();
    void refreshLocalIds();
    void loadPreview (const juce::File& file);
    void syncPlayerState();
    void onCaptureFinished (const juce::File& file, const juce::String& error);
    void setCaptureUI (bool capturing);
    void setError (const juce::String& text);
    void setProgress (const juce::String& text);
    int addLocalTransfer (const juce::String& fileName, const juce::String& direction);
    void updateLocalTransfer (int entry, double progress, const juce::String& status,
                              const juce::String& error, bool canRetry);
    void uploadLocalFileInternal (const juce::File& file, const juce::String& project, int reuseEntry);
    void downloadFileInternal (const CloudFile& file, int reuseEntry);

    OnorabAudioProcessor& audioProcessor;

    HubClient hub;
    HubClient::AliveToken aliveToken = std::make_shared<std::atomic<bool>> (true);

    juce::AudioFormatManager formatManager;
    juce::AudioTransportSource transport;
    juce::TimeSliceThread readAheadThread { "Onorab preview read-ahead" };
    std::unique_ptr<juce::AudioFormatReaderSource> previewSource;

    std::vector<CloudFile> files;
    std::vector<juce::String> localIds;
    CloudFile selectedFile;
    bool hasSelection = false;
    juce::File selectedLocalFile;
    bool selectedLocalExists = false;
    double selectedDurationSeconds = -1.0;
    juce::String playingFileId;
    juce::String playAfterDownloadId;
    std::vector<TransferInfo> hubTransfers;
    std::vector<LocalTransfer> localTransfers;
    bool hubConnected = false;
    bool driveConnected = false;
    bool prevDriveConnected = false;
    // Set while a browser sign-in is in flight (from Connect until
    // the Hub confirms connected or reports a failure). While set,
    // the timer polls auth status every 2 seconds so "Drive
    // connected" (or the failure + "Try again") appears promptly
    // instead of waiting for the 10-second poll.
    bool authAwaiting = false;
    // tryLaunchInstalledHub runs at most once per editor instance.
    bool hubLaunchAttempted = false;
    float previewGain = 0.8f;

    // Header (always visible above the tabs).
    juce::Label titleLabel, byLabel, hubStatusLabel, driveStatusLabel, captureCounterLabel;
    StatusDot hubDot, driveDot;
    juce::TextButton captureButton { "Capture from FL" };
    juce::Time captureStartTime;
    int timerTicks = 0;

    // Capture review state.
    CaptureReviewPanel* captureReview = nullptr;
    juce::File captureReviewFile;
    juce::File captureUploadTarget;
    std::function<void (bool, juce::String)> captureUploadHook;

    // Project currently auto-capturing routed Mixer audio (empty = off).
    // Owned by the editor; the processor only knows the target while
    // its auto writer runs.
    juce::String autoCaptureProject;

    // Tab components are owned by the TabbedComponent; these raw pointers
    // are only used while the editor is alive (the tabs die with it).
    LibraryTab* libraryTab = nullptr;
    ProjectsTab* projectsTab = nullptr;
    TransfersTab* transfersTab = nullptr;
    SettingsTab* settingsTab = nullptr;
    juce::TabbedComponent tabbedComponent { juce::TabbedButtonBar::TabsAtTop };
    // NOTE (JUCE 8 fix, 2026-11-03): DragAndDropContainer is NOT a
    // Component in JUCE 8, so it must be a BASE CLASS of the editor,
    // not a member - a member cannot host children (no addAndMakeVisible
    // / setBounds) and findParentComponentOfClass<DragAndDropContainer>
    // would never find it. The editor itself is the container.

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OnorabAudioProcessorEditor)
};
