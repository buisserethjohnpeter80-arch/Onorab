#pragma once

// HubClient — HTTP client for the Onorab Hub (localhost only).
//
// Threading contract (hard rule for this whole plugin):
//   * Every public method here returns immediately and does its network
//     work on a detached background thread. Results are delivered on the
//     JUCE message thread via MessageManager::callAsync, so UI code can
//     safely update components in the callback.
//   * NOTHING in this file is ever called from the real-time audio thread.
//   * Errors are never swallowed: every callback carries an `ok` flag and
//     a human-readable `error` string, and failures are also DBG-logged.
//
// Hub contract (Phase 1; the Hub side is owned by apps/onorab-hub):
//   GET  {base}/health             -> {"ok": true, ...} (or {"status": ...})
//   GET  {base}/files              -> [{"id","name","sizeBytes",...}]
//                                    or {"files": [...]}
//   GET  {base}/transfers          -> [{"id","fileName","direction",
//                                    "status","progress",...}] or
//                                    {"transfers": [...]}
//   GET  {base}/downloads/{id}     -> raw audio bytes (saved to cache dir)
//   POST {base}/uploads            -> multipart/form-data, single field
//                                    named "file" (Content-Disposition
//                                    filename = the file name). The Hub
//                                    must accept this field name.
// Phase 1 polling: there is deliberately NO WebSocket client here yet.
// The editor polls GET /transfers every 2 seconds (see PluginEditor).
// A WebSocket /events client is a Phase 2 item; polling is the honest
// Phase 1 behaviour and is stated as such in README.md.
//
// Security: the base URL is localhost by default and is user-editable in
// Settings. No credentials of any kind are stored or sent by this client;
// Google OAuth lives entirely in the Hub. No credential is compiled in.

#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

struct CloudFile
{
    juce::String id;
    juce::String name;
    int64_t sizeBytes = 0;
    juce::String updatedAt;
    juce::String mimeType;
};

struct TransferInfo
{
    juce::String id;
    juce::String fileId;
    juce::String fileName;
    juce::String direction; // "upload" | "download" | "" if unknown
    juce::String status;    // e.g. "queued" | "active" | "done" | "failed"
    double progress = 0.0;  // 0.0 .. 1.0 (a 0..100 value is normalised)
    juce::String message;
};

class HubClient
{
public:
    using AliveToken = std::shared_ptr<std::atomic<bool>>;

    using HealthCallback = std::function<void (bool ok, juce::String error, juce::String detail)>;
    using FilesCallback = std::function<void (bool ok, juce::String error, std::vector<CloudFile> files)>;
    using TransfersCallback = std::function<void (bool ok, juce::String error, std::vector<TransferInfo> transfers)>;
    // ok + error + local file path (empty on failure).
    using DownloadCallback = std::function<void (bool ok, juce::String error, juce::File localFile)>;
    using UploadCallback = std::function<void (bool ok, juce::String error)>;
    // Progress in 0.0..1.0, or < 0 when the total size is unknown.
    // Invoked from the background thread; the caller must hop to the
    // message thread before touching UI (the editor does this).
    using ProgressCallback = std::function<void (double progress)>;

    explicit HubClient (juce::String baseUrl = "http://127.0.0.1:18741");

    // The editor sets this to a token it flips to false in its destructor,
    // so a late background result can never touch a destroyed editor.
    void setAliveToken (AliveToken token) { aliveToken = std::move (token); }

    void setBaseUrl (juce::String url);
    juce::String getBaseUrl() const { return baseUrl; }

    // All async; callbacks run on the message thread (except the progress
    // callback of downloadFile/uploadFile, which runs on the worker thread).
    void checkHealth (HealthCallback cb);
    void listFiles (FilesCallback cb);
    void listTransfers (TransfersCallback cb);
    void downloadFile (const CloudFile& file, const juce::File& cacheDir,
                       DownloadCallback cb, ProgressCallback progress = nullptr);
    void uploadFile (const juce::File& file, UploadCallback cb,
                     ProgressCallback progress = nullptr);

    // Default cache directory. Precedence:
    //   1. ONORAB_CACHE_DIR environment variable (used by tests / CI);
    //      when set, it IS the cache directory, used exactly as given.
    //   2. %LOCALAPPDATA%\Onorab\cache on Windows.
    //   3. The OS user-application-data directory + Onorab/cache.
    static juce::File getDefaultCacheDir();

    // File names coming from the Hub must never escape the cache dir
    // (e.g. "../evil.wav"). Strips path separators.
    static juce::String sanitiseFileName (const juce::String& name);

private:
    void runInBackground (std::function<void()> work);

    // Synchronous helpers — background thread ONLY. Return false + error
    // text on any failure; never throw across the thread boundary.
    // Static on purpose: a GET needs no client state, so the background
    // lambdas do not have to keep a client object alive.
    static bool httpGetText (const juce::String& url, juce::String& outText, juce::String& outError);
    static std::vector<CloudFile> parseFiles (const juce::String& jsonText);
    static std::vector<TransferInfo> parseTransfers (const juce::String& jsonText);
    static double normaliseProgress (const juce::var& value);

    juce::String baseUrl;
    AliveToken aliveToken;
};
