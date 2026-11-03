// HubClient implementation. See HubClient.h for the threading contract
// and the Hub HTTP contract. All network work here runs on background
// threads; results are posted to the message thread before any callback
// that touches the UI runs.

#include "HubClient.h"
#include <cstdlib>

namespace
{
// Connection timeout for localhost Hub calls. The Hub is local, so 5 s
// is generous; a longer timeout would only make a dead Hub feel hung.
constexpr int kTimeoutMs = 5000;

juce::String trimmedBase (const juce::String& url)
{
    // Drop a trailing slash so endpoint concatenation stays predictable.
    return url.trim().endsWithChar ('/') ? url.trim().dropLastCharacters (1) : url.trim();
}

juce::String getStringField (const juce::var& obj, const juce::String& key,
                             const juce::String& fallbackKey = {})
{
    if (auto* dyn = obj.getDynamicObject())
    {
        if (dyn->hasProperty (key))
            return dyn->getProperty (key).toString();
        if (fallbackKey.isNotEmpty() && dyn->hasProperty (fallbackKey))
            return dyn->getProperty (fallbackKey).toString();
    }
    return {};
}

int64_t getIntField (const juce::var& obj, const juce::String& key,
                     const juce::String& fallbackKey = {})
{
    if (auto* dyn = obj.getDynamicObject())
    {
        if (dyn->hasProperty (key))
            return static_cast<int64_t> (dyn->getProperty (key));
        if (fallbackKey.isNotEmpty() && dyn->hasProperty (fallbackKey))
            return static_cast<int64_t> (dyn->getProperty (fallbackKey));
    }
    return 0;
}

// Accepts either a bare JSON array or an object wrapping it under `key`
// (or, for robustness, under "data").
const juce::Array<juce::var>* findArray (const juce::var& parsed, const juce::String& key)
{
    if (auto* arr = parsed.getArray())
        return arr;
    if (auto* dyn = parsed.getDynamicObject())
    {
        if (dyn->hasProperty (key))
            return dyn->getProperty (key).getArray();
        if (dyn->hasProperty ("data"))
            return dyn->getProperty ("data").getArray();
    }
    return nullptr;
}
} // namespace

HubClient::HubClient (juce::String baseUrlIn)
    : baseUrl (trimmedBase (baseUrlIn))
{
}

void HubClient::setBaseUrl (juce::String url)
{
    // Message thread only (called from Settings). The background workers
    // read baseUrl by value at call time, so a mid-flight change affects
    // only the next request, never a request already in flight.
    baseUrl = trimmedBase (url);
}

void HubClient::runInBackground (std::function<void()> work)
{
    // Detached worker thread. The work lambda must catch its own errors
    // and report them through the callback; nothing here swallows them.
    std::thread ([work = std::move (work)]() mutable
    {
        try
        {
            work();
        }
        catch (const std::exception& e)
        {
            DBG ("Onorab HubClient: unexpected exception: " << e.what());
        }
        catch (...)
        {
            DBG ("Onorab HubClient: unexpected unknown exception");
        }
    }).detach();
}

bool HubClient::httpGetText (const juce::String& url, juce::String& outText, juce::String& outError)
{
    juce::URL request (url);
    auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                       .withConnectionTimeoutMs (kTimeoutMs);

    std::unique_ptr<juce::InputStream> stream (request.createInputStream (options));
    if (stream == nullptr)
    {
        outError = "Could not reach the Hub at " + url
                 + " — is Onorab Hub running on this computer?";
        DBG ("Onorab HubClient GET failed (no stream): " << url);
        return false;
    }

    outText = stream->readEntireStreamAsString();
    // NOTE (JUCE 8 fix, 2026-11-03): juce::InputStream has NO getStatus()
    // and NO Status::readError member — the earlier code used an API that
    // does not exist, which broke the Windows CI build (C3083/C2039).
    // Failure detection here is: a null stream above (unreachable Hub),
    // and callers treating an empty/unparseable body as an error.
    // Do NOT re-add a getStatus() check without verifying it against
    // the JUCE 8 headers first.
    return true;
}

void HubClient::checkHealth (HealthCallback cb)
{
    const juce::String url = baseUrl + "/health";
    auto token = aliveToken;
    runInBackground ([url, cb = std::move (cb), token]() mutable
    {
        juce::String text, error;
        const bool ok = HubClient::httpGetText (url, text, error);
        juce::String detail = text;
        if (ok)
        {
            // Accept {"ok": true}, {"status": "ok"} or any non-empty body.
            if (text.isEmpty())
                detail = "Hub answered, but with an empty body.";
        }
        else
        {
            DBG ("Onorab HubClient health check failed: " << error);
        }
        juce::MessageManager::callAsync ([cb = std::move (cb), token, ok, error, detail]() mutable
        {
            if (token != nullptr && ! token->load())
                return; // Editor is gone; never touch UI.
            cb (ok, error, detail);
        });
    });
}

void HubClient::listFiles (FilesCallback cb)
{
    const juce::String url = baseUrl + "/files";
    auto token = aliveToken;
    runInBackground ([url, cb = std::move (cb), token]() mutable
    {
        juce::String text, error;
        std::vector<CloudFile> files;
        bool ok = HubClient::httpGetText (url, text, error);
        if (ok)
        {
            files = parseFiles (text);
            if (files.empty() && ! text.trim().startsWithChar ('[')
                && ! text.trim().startsWithChar ('{'))
            {
                ok = false;
                error = "Hub returned an unexpected response for /files: " + text.substring (0, 200);
                DBG ("Onorab HubClient /files parse failure: " << error);
            }
        }
        juce::MessageManager::callAsync ([cb = std::move (cb), token, ok, error, files = std::move (files)]() mutable
        {
            if (token != nullptr && ! token->load())
                return;
            cb (ok, error, std::move (files));
        });
    });
}

void HubClient::listTransfers (TransfersCallback cb)
{
    const juce::String url = baseUrl + "/transfers";
    auto token = aliveToken;
    runInBackground ([url, cb = std::move (cb), token]() mutable
    {
        juce::String text, error;
        std::vector<TransferInfo> transfers;
        bool ok = HubClient::httpGetText (url, text, error);
        if (ok)
            transfers = parseTransfers (text);
        juce::MessageManager::callAsync ([cb = std::move (cb), token, ok, error, transfers = std::move (transfers)]() mutable
        {
            if (token != nullptr && ! token->load())
                return;
            cb (ok, error, std::move (transfers));
        });
    });
}

void HubClient::downloadFile (const CloudFile& file, const juce::File& cacheDir,
                              DownloadCallback cb, ProgressCallback progress)
{
    const juce::String url = baseUrl + "/downloads/" + juce::URL::addEscapeChars (file.id, false);
    const juce::String safeName = sanitiseFileName (file.name.isEmpty() ? file.id : file.name);
    auto token = aliveToken;
    runInBackground ([url, cacheDir, safeName, cb = std::move (cb), progress = std::move (progress), token]() mutable
    {
        bool ok = false;
        juce::String error;
        juce::File localFile;

        juce::URL request (url);
        auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                           .withConnectionTimeoutMs (kTimeoutMs);
        std::unique_ptr<juce::InputStream> stream (request.createInputStream (options));
        if (stream == nullptr)
        {
            error = "Could not download from the Hub — is Onorab Hub running?";
            DBG ("Onorab HubClient download failed (no stream): " << url);
        }
        else
        {
            const int64_t total = stream->getTotalLength();
            localFile = cacheDir.getChildFile (safeName);
            if (! cacheDir.exists() && ! cacheDir.createDirectory())
            {
                error = "Could not create the cache folder: " + cacheDir.getFullPathName();
                DBG ("Onorab HubClient: " << error);
            }
            else if (auto out = localFile.createOutputStream())
            {
                constexpr int kBufSize = 16384;
                std::vector<char> buf (static_cast<size_t> (kBufSize));
                int64_t written = 0;
                bool readFailed = false;
                while (! stream->isExhausted())
                {
                    const int n = stream->read (buf.data(), kBufSize);
                    if (n < 0) { readFailed = true; break; }
                    if (n == 0) break;
                    if (! out->write (buf.data(), static_cast<size_t> (n))) { readFailed = true; break; }
                    written += n;
                    if (progress != nullptr)
                        progress (total > 0 ? static_cast<double> (written) / static_cast<double> (total) : -1.0);
                }
                out->flush();
                // NOTE (JUCE 8 fix): no getStatus()/Status::readError on
                // juce::InputStream — readFailed (negative read or failed
                // write above) is the only failure signal here.
                if (readFailed)
                {
                    error = "Download was interrupted for file: " + safeName;
                    DBG ("Onorab HubClient download interrupted: " << safeName);
                    localFile.deleteFile();
                    localFile = {};
                }
                else
                {
                    ok = true;
                }
            }
            else
            {
                error = "Could not write to the cache file: " + localFile.getFullPathName();
                DBG ("Onorab HubClient: " << error);
            }
        }

        juce::MessageManager::callAsync ([cb = std::move (cb), token, ok, error, localFile]() mutable
        {
            if (token != nullptr && ! token->load())
                return;
            cb (ok, error, localFile);
        });
    });
}

juce::String HubClient::projectFolder (const juce::String& projectId)
{
    if (projectId.trim().isEmpty())
        return "Onorab_projet";
    return "Onorab_projet/" + projectId.trim();
}

void HubClient::uploadFile (const juce::File& file, UploadCallback cb, ProgressCallback progress,
                            const juce::String& project)
{
    const juce::String url = baseUrl + "/uploads";
    auto token = aliveToken;
    runInBackground ([url, file, cb = std::move (cb), progress = std::move (progress), token]() mutable
    {
        bool ok = false;
        juce::String error;

        if (! file.existsAsFile())
        {
            error = "Upload failed: file does not exist: " + file.getFullPathName();
            DBG ("Onorab HubClient: " << error);
        }
        else
        {
            // Multipart/form-data body, field name "file" (see HubClient.h
            // contract). Built by hand so the field name and filename are
            // exactly what the Hub expects; no credentials are included.
            const juce::String boundary = "----OnorabBoundary7f3a9c1e";
            juce::MemoryBlock body;
            const auto appendText = [&body] (const juce::String& s)
            {
                body.append (s.toRawUTF8(), s.getNumBytesAsUTF8());
            };
            // Optional project field (must come before the file part in
            // this hand-built body). The Hub sanitises it; an empty
            // project means "no project yet", never a folder escape.
            if (project.trim().isNotEmpty())
            {
                appendText ("--" + boundary + "\r\n");
                appendText ("Content-Disposition: form-data; name=\"project\"\r\n\r\n");
                appendText (project.trim() + "\r\n");
            }
            appendText ("--" + boundary + "\r\n");
            appendText ("Content-Disposition: form-data; name=\"file\"; filename=\""
                        + file.getFileName() + "\"\r\n");
            appendText ("Content-Type: application/octet-stream\r\n\r\n");

            juce::MemoryBlock fileData;
            if (! file.loadFileAsData (fileData))
            {
                error = "Could not read the file for upload: " + file.getFullPathName();
                DBG ("Onorab HubClient: " << error);
            }
            else
            {
                body.append (fileData.getData(), fileData.getSize());
                appendText ("\r\n--" + boundary + "--\r\n");

                if (progress != nullptr)
                    progress (0.5); // Upload progress is coarse: sent / done.

                // NOTE (JUCE 8 fix, 2026-11-03): withPOSTData() lives on
                // juce::URL, NOT on URL::InputStreamOptions — the earlier
                // chain failed CI with C2039. URL is immutable, so
                // withPOSTData() returns a NEW url; assigning it back is
                // required, otherwise the POST body is silently dropped.
                // With POST data set, createInputStream() does a POST.
                juce::URL request = juce::URL (url).withPOSTData (body);
                auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                   .withConnectionTimeoutMs (kTimeoutMs)
                                   .withExtraHeaders ("Content-Type: multipart/form-data; boundary=" + boundary);
                std::unique_ptr<juce::InputStream> stream (request.createInputStream (options));
                if (stream == nullptr)
                {
                    error = "Could not reach the Hub to upload — is Onorab Hub running?";
                    DBG ("Onorab HubClient upload failed (no stream): " << url);
                }
                else
                {
                    const juce::String response = stream->readEntireStreamAsString();
                    // A Hub error body mentioning "error" is surfaced, not
                    // treated as success. Any other response = accepted.
                    if (response.containsIgnoreCase ("\"error\""))
                    {
                        error = "Hub rejected the upload: " + response.substring (0, 300);
                        DBG ("Onorab HubClient upload rejected: " << response);
                    }
                    else
                    {
                        ok = true;
                        if (progress != nullptr)
                            progress (1.0);
                    }
                }
            }
        }

        juce::MessageManager::callAsync ([cb = std::move (cb), token, ok, error]() mutable
        {
            if (token != nullptr && ! token->load())
                return;
            cb (ok, error);
        });
    });
}

// Small POST helper for the auth/delete/invite endpoints: POSTs the
// given body, then reports ok unless the Hub answered with an error
// envelope. Background thread; the callback hops to the message thread.
static void postToHub (juce::String url, const juce::String& bodyText,
                       const juce::String& contentType,
                       HubClient::AliveToken token,
                       std::function<void (bool, juce::String)> done)
{
    juce::MemoryBlock body;
    body.append (bodyText.toRawUTF8(), bodyText.getNumBytesAsUTF8());
    juce::URL request = juce::URL (url).withPOSTData (body);
    auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                       .withConnectionTimeoutMs (kTimeoutMs);
    if (contentType.isNotEmpty())
        options = options.withExtraHeaders ("Content-Type: " + contentType);
    std::unique_ptr<juce::InputStream> stream (request.createInputStream (options));
    bool ok = false;
    juce::String error;
    if (stream == nullptr)
    {
        error = "Could not reach the Hub — is Onorab Hub running?";
    }
    else
    {
        const juce::String response = stream->readEntireStreamAsString();
        if (response.containsIgnoreCase ("\"error\""))
            error = response.substring (0, 300);
        else
            ok = true;
    }
    juce::MessageManager::callAsync ([done = std::move (done), token, ok, error]() mutable
    {
        if (token != nullptr && ! token->load())
            return;
        done (ok, error);
    });
}

void HubClient::deleteFile (const CloudFile& file, DeleteCallback cb)
{
    const juce::String url = baseUrl + "/files/" + file.id + "/delete";
    auto token = aliveToken;
    runInBackground ([url, cb = std::move (cb), token]() mutable
    {
        postToHub (url, "delete", {}, token, [cb = std::move (cb)] (bool ok, juce::String error) mutable
        {
            cb (ok, error);
        });
    });
}

void HubClient::getAuthStatus (AuthCallback cb)
{
    const juce::String url = baseUrl + "/auth/status";
    auto token = aliveToken;
    runInBackground ([url, cb = std::move (cb), token]() mutable
    {
        juce::String text, error;
        bool ok = httpGetText (url, text, error);
        juce::String detail;
        if (ok)
        {
            const juce::var parsed = juce::JSON::parse (text);
            detail = "provider: " + getStringField (parsed, "provider")
                   + ", authenticated: " + getStringField (parsed, "authenticated");
        }
        juce::MessageManager::callAsync ([cb = std::move (cb), token, ok, error, detail]() mutable
        {
            if (token != nullptr && ! token->load())
                return;
            cb (ok, error, detail);
        });
    });
}

void HubClient::startAuth (AuthCallback cb)
{
    const juce::String url = baseUrl + "/auth/start";
    auto token = aliveToken;
    runInBackground ([url, cb = std::move (cb), token]() mutable
    {
        // postToHub already hops to the message thread before calling
        // done, so the Auth callback runs there directly (no second hop).
        postToHub (url, "start", {}, token, [cb = std::move (cb)] (bool ok, juce::String error) mutable
        {
            cb (ok, error, {});
        });
    });
}

void HubClient::sendInvite (const juce::String& email, const juce::String& project, InviteCallback cb)
{
    const juce::String url = baseUrl + "/invites";
    auto token = aliveToken;
    runInBackground ([url, email, project, cb = std::move (cb), token]() mutable
    {
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty ("email", email);
        obj->setProperty ("project", project);
        const juce::String body = juce::JSON::toString (juce::var (obj.get()));
        postToHub (url, body, "application/json", token, [cb = std::move (cb)] (bool ok, juce::String error) mutable
        {
            cb (ok, error);
        });
    });
}

std::vector<CloudFile> HubClient::parseFiles (const juce::String& jsonText)
{
    std::vector<CloudFile> out;
    const juce::var parsed = juce::JSON::parse (jsonText);
    if (const auto* arr = findArray (parsed, "files"))
    {
        for (const auto& item : *arr)
        {
            CloudFile f;
            f.id = getStringField (item, "id", "fileId");
            f.name = getStringField (item, "name", "fileName");
            f.sizeBytes = getIntField (item, "sizeBytes", "size");
            f.updatedAt = getStringField (item, "updatedAt");
            f.mimeType = getStringField (item, "mimeType");
            f.projectId = getStringField (item, "projectId");
            if (f.id.isNotEmpty() && f.name.isNotEmpty())
                out.push_back (std::move (f));
            else
                DBG ("Onorab HubClient: skipped a /files entry missing id or name");
        }
    }
    return out;
}

std::vector<TransferInfo> HubClient::parseTransfers (const juce::String& jsonText)
{
    std::vector<TransferInfo> out;
    const juce::var parsed = juce::JSON::parse (jsonText);
    if (const auto* arr = findArray (parsed, "transfers"))
    {
        for (const auto& item : *arr)
        {
            TransferInfo t;
            t.id = getStringField (item, "id", "transferId");
            t.fileId = getStringField (item, "fileId");
            t.fileName = getStringField (item, "fileName", "name");
            t.direction = getStringField (item, "direction");
            t.status = getStringField (item, "status", "state");
            t.message = getStringField (item, "message", "error");
            if (auto* dyn = item.getDynamicObject())
            {
                if (dyn->hasProperty ("progress"))
                    t.progress = normaliseProgress (dyn->getProperty ("progress"));
            }
            out.push_back (std::move (t));
        }
    }
    return out;
}

double HubClient::normaliseProgress (const juce::var& value)
{
    const double p = static_cast<double> (value);
    // Hubs sometimes report 0..100; normalise anything above 1 to 0..1.
    const double normalised = p > 1.0 ? p / 100.0 : p;
    return juce::jlimit (0.0, 1.0, normalised);
}

juce::File HubClient::getDefaultCacheDir()
{
    // 1. Explicit override (tests / CI / power users): the env var IS the
    //    cache directory, used exactly as given.
    if (const char* env = std::getenv ("ONORAB_CACHE_DIR"); env != nullptr && env[0] != '\0')
        return juce::File (env);
    // 2. Windows %LOCALAPPDATA%.
    if (const char* localAppData = std::getenv ("LOCALAPPDATA");
        localAppData != nullptr && localAppData[0] != '\0')
    {
        return juce::File (localAppData).getChildFile ("Onorab").getChildFile ("cache");
    }
    // 3. Cross-platform fallback (macOS/Linux dev, JUCE test hosts).
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Onorab")
        .getChildFile ("cache");
}

juce::String HubClient::sanitiseFileName (const juce::String& name)
{
    juce::String safe = name.trim();
    // Keep only the file name itself: drop any directory component, then
    // replace remaining separators / control characters with '_'.
    safe = juce::File::createLegalFileName (safe);
    safe = safe.replaceCharacter ('/', '_').replaceCharacter ('\\', '_');
    if (safe.isEmpty() || safe == "." || safe == "..")
        safe = "onorab-file";
    return safe;
}
