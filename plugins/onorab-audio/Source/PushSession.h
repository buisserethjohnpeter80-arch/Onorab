#pragma once

// PushSession — the in-process coordinator for Send / Manager multi-track Push.
//
// WHY A PROCESS SINGLETON (and not the Hub): every Onorab instance in one
// FL project lives in the same FL process — FL loads 64-bit VST3s
// in-process by default — so this DLL's statics are shared by exactly the
// instances that must coordinate. Two FL projects open on one PC are two
// processes, so their singletons are separate and their tracks can never
// mix. The Hub stays out of coordination entirely: it only does what it
// already does (Drive auth + upload). No network call is ever needed to
// list Sends, arm a Push, or record.
//
// Session identity: sessionTag ("FL-XXXX", random per process) is shown in
// the Send and Manager UIs so the user can SEE which FL session a Send
// belongs to. It is not the .flp name — the host never gives a VST3 the
// project name — and it is never sent to Drive or the Hub. If FL ever
// bridges Onorab (each instance in its own bridge process), Sends in
// different bridge processes cannot see each other; the default
// unbridged load is what the Tracks list assumes.
//
// REAL-TIME RULES (a violation here is an audio dropout, not a bug):
//   * The registry (instance list) is guarded by a mutex and touched ONLY
//     on the message thread (registration, rename, arm, editor reads).
//     The audio thread NEVER touches it.
//   * Arm state (armed, pushId, finishRequested, pushStartSample) is
//     plain atomics: the audio thread may read them, nothing else.
//   * Each Send's status / frames counters live on its registry entry as
//     atomics: the owning instance's audio or writer thread writes them,
//     the Manager editor reads them. No lock on either side.
//   * The finished-take queue is guarded by a mutex and touched ONLY by
//     a writer thread (push) and the message thread (Manager drain).
//     The audio thread NEVER touches it.
//   * A Send's processor pointer is only dereferenced while the registry
//     lock is held (arming): destruction unregisters under the same
//     lock, so an armed Send cannot be destroyed mid-arm.

#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

struct PushSendEntry
{
    juce::String instanceId;
    juce::String trackName;
    // Hooks into the owning processor. Called ONLY while the registry
    // lock is held, so the processor cannot be destroyed mid-call.
    std::function<juce::String (int pushId)> prepareHook; // "" = prepared, else error
    std::function<void()> cancelHook;
    // 0 Idle, 1 Prepared (armed, writer waiting), 2 Recording, 3 Finished.
    std::atomic<int> status { 0 };
    std::atomic<int64_t> framesWritten { 0 };
};

// One finished Push take. `file` is empty exactly when no audio was
// recorded (prepared but never started) or when writing failed; in the
// failure case `error` names the reason. A take is queued exactly once
// per prepared Send per Push — even an empty one — so the Manager can
// tell "writer finished, no audio" apart from "writer still running".
struct PushTake
{
    juce::String instanceId;
    juce::String trackName;
    juce::File file;
    int64_t frames = 0;
    int droppedSamples = 0;
    // "stop" (transport stopped) | "finish" (Finish & upload) |
    // "destroyed" (instance closed mid-recording: the file is partial).
    // "jumped" (playback looped, sought, or restarted mid-take: the
    // take stops at the jump so it stays aligned to its measures).
    juce::String finishedBy;
    int pushId = 0;
    juce::String error;
};

class PushSession
{
public:
    struct SendSnapshot
    {
        juce::String instanceId;
        juce::String trackName;
        int status = 0;
        int64_t frames = 0;
    };

    static PushSession& getInstance()
    {
        static PushSession instance;
        return instance;
    }

    PushSession (const PushSession&) = delete;
    PushSession& operator= (const PushSession&) = delete;

    juce::String getSessionTag() const { return sessionTag; }

    // ---- Registry (message thread only) ----
    // Returns the entry (shared with the caller) or nullptr when the id
    // is already live — the caller must regenerate its id in that case
    // (this is the clone case: FL copied a saved state onto a new
    // instance while the original still exists).
    std::shared_ptr<PushSendEntry> registerSend (const juce::String& instanceId,
                                                 const juce::String& trackName,
                                                 std::function<juce::String (int)> prepareHook,
                                                 std::function<void()> cancelHook)
    {
        std::lock_guard<std::mutex> lock (registryMutex);
        if (sends.find (instanceId) != sends.end())
            return nullptr;
        auto entry = std::make_shared<PushSendEntry>();
        entry->instanceId = instanceId;
        entry->trackName = trackName;
        entry->prepareHook = std::move (prepareHook);
        entry->cancelHook = std::move (cancelHook);
        sends[instanceId] = entry;
        return entry;
    }

    void unregisterSend (const juce::String& instanceId)
    {
        std::lock_guard<std::mutex> lock (registryMutex);
        sends.erase (instanceId);
    }

    bool isIdLive (const juce::String& instanceId)
    {
        std::lock_guard<std::mutex> lock (registryMutex);
        return sends.find (instanceId) != sends.end();
    }

    void renameSend (const juce::String& instanceId, const juce::String& trackName)
    {
        std::lock_guard<std::mutex> lock (registryMutex);
        if (auto it = sends.find (instanceId); it != sends.end())
            it->second->trackName = trackName;
    }

    std::vector<SendSnapshot> getSends()
    {
        std::lock_guard<std::mutex> lock (registryMutex);
        std::vector<SendSnapshot> out;
        out.reserve (sends.size());
        for (const auto& [id, entry] : sends)
            out.push_back ({ id, entry->trackName, entry->status.load (std::memory_order_relaxed),
                             entry->framesWritten.load (std::memory_order_relaxed) });
        return out;
    }

    // ---- Push arming (message thread, Manager only) ----
    // Prepares every live Send in `trackIds` (registry lock held for the
    // whole call, so no Send can vanish mid-prepare). Returns false and
    // arms nothing when no Send could be prepared. A prepared Send whose
    // writer later finishes with no audio still queues an empty take, so
    // the Manager never waits forever on a prepared Send.
    // Takes from older pushes are deliberately NOT cleared here: a new
    // Push only ever drains takes matching its own pushId, and the
    // abandoned WAV files stay in the cache folder.
    bool armPush (const std::vector<juce::String>& trackIds,
                  const juce::String& project,
                  std::vector<juce::String>& preparedIds,
                  std::vector<juce::String>& missingIds,
                  std::vector<std::pair<juce::String, juce::String>>& failed)
    {
        std::lock_guard<std::mutex> lock (registryMutex);
        if (armed.load (std::memory_order_relaxed))
            return false;
        const int newPushId = pushId.load (std::memory_order_relaxed) + 1;
        preparedIds.clear();
        missingIds.clear();
        failed.clear();
        for (const auto& id : trackIds)
        {
            auto it = sends.find (id);
            if (it == sends.end())
            {
                missingIds.push_back (id);
                continue;
            }
            if (juce::String error = it->second->prepareHook (newPushId); error.isNotEmpty())
                failed.emplace_back (id, error);
            else
                preparedIds.push_back (id);
        }
        if (preparedIds.empty())
            return false;
        pushId.store (newPushId, std::memory_order_relaxed);
        pushProject = project;
        armedTrackIds = preparedIds;
        finishRequested.store (false, std::memory_order_relaxed);
        pushStartSample.store (-1, std::memory_order_relaxed);
        armed.store (true, std::memory_order_relaxed);
        return true;
    }

    // Cancels a Push that has not started recording yet. Returns the ids
    // that could NOT be cancelled because they are already recording —
    // in that case nothing is disarmed and the Manager must offer
    // Finish & upload instead.
    std::vector<juce::String> cancelPush()
    {
        std::lock_guard<std::mutex> lock (registryMutex);
        std::vector<juce::String> notCancelled;
        for (const auto& id : armedTrackIds)
            if (auto it = sends.find (id); it != sends.end() && it->second->status.load() == 2)
                notCancelled.push_back (id);
        if (! notCancelled.empty())
            return notCancelled;
        for (const auto& id : armedTrackIds)
            if (auto it = sends.find (id); it != sends.end() && it->second->cancelHook)
                it->second->cancelHook();
        armed.store (false, std::memory_order_relaxed);
        finishRequested.store (false, std::memory_order_relaxed);
        armedTrackIds.clear();
        pushProject.clear();
        return {};
    }

    void requestFinish()
    {
        finishRequested.store (true, std::memory_order_relaxed);
    }

    // Called by the Manager after the upload phase completes. The
    // editor keeps its own copy of the takes (for Retry display); the
    // singleton only forgets the arm state.
    void disarmAfterUpload()
    {
        std::lock_guard<std::mutex> lock (registryMutex);
        armed.store (false, std::memory_order_relaxed);
        finishRequested.store (false, std::memory_order_relaxed);
        armedTrackIds.clear();
        pushProject.clear();
    }

    bool isArmed() const { return armed.load (std::memory_order_relaxed); }
    int getPushId() const { return pushId.load (std::memory_order_relaxed); }
    bool isFinishRequested() const { return finishRequested.load (std::memory_order_acquire); }

    juce::String getPushProject()
    {
        std::lock_guard<std::mutex> lock (registryMutex);
        return pushProject;
    }

    // First transport position (in samples) at which any armed Send
    // started recording. The first starter wins (CAS); later starters
    // pad leading silence up to it, so every take starts at the same
    // timeline point and stays aligned when re-imported together.
    // -1 = no starter yet (or no playhead information).
    bool compareExchangePushStart (int64_t& expected, int64_t value)
    {
        return pushStartSample.compare_exchange_strong (expected, value,
                                                        std::memory_order_acq_rel);
    }

    int64_t getPushStartSample() const { return pushStartSample.load (std::memory_order_acquire); }

    // ---- Finished takes (writer thread pushes, message thread drains) ----
    void queueTake (PushTake take)
    {
        std::lock_guard<std::mutex> lock (takesMutex);
        takes.push_back (std::move (take));
    }

    std::vector<PushTake> takeTakes()
    {
        std::lock_guard<std::mutex> lock (takesMutex);
        std::vector<PushTake> out;
        out.swap (takes);
        return out;
    }

private:
    PushSession()
    {
        // Random per process, stable for the life of the process.
        // Built nibble by nibble so the tag never depends on a
        // formatting helper's negative-number behaviour.
        const uint32_t r = static_cast<uint32_t> (juce::Random::getSystemRandom().nextInt());
        juce::String hex;
        for (int shift = 12; shift >= 0; shift -= 4)
            hex += "0123456789ABCDEF"[(r >> shift) & 0xF];
        sessionTag = "FL-" + hex;
    }

    juce::String sessionTag;

    std::mutex registryMutex;
    std::map<juce::String, std::shared_ptr<PushSendEntry>> sends;
    juce::String pushProject;
    std::vector<juce::String> armedTrackIds;

    std::atomic<bool> armed { false };
    std::atomic<int> pushId { 0 };
    std::atomic<bool> finishRequested { false };
    std::atomic<int64_t> pushStartSample { -1 };

    std::mutex takesMutex;
    std::vector<PushTake> takes;
};
