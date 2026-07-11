#include "redux/MotionLibraryStore.h"

#include "ReduxLog.h"

#include <ShlObj.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace redux::motion_library
{
    namespace
    {
        std::string resolveLibraryDirectory()
        {
            char documents[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_MYDOCUMENTS, nullptr, 0, documents))) {
                return std::string(documents) + R"(\My Games\Fallout4VR\PAPERRedux_Config\MotionLibrary)";
            }
            RDX_LOG_WARN(Config, "SHGetFolderPath failed — motion library falls back to Data\\F4SE\\Plugins\\PAPERReduxMotionLibrary");
            return R"(Data\F4SE\Plugins\PAPERReduxMotionLibrary)";
        }

        // Plugin names become file-name components; keep letters, digits,
        // dots, dashes, underscores and spaces, replace the rest.
        std::string sanitizeForFileName(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());
            for (const char c : text) {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == ' ';
                out.push_back(ok ? c : '_');
            }
            return out;
        }

        std::uint64_t stableFileNameHash(std::string_view text)
        {
            std::uint64_t hash = 14695981039346656037ull;
            for (const unsigned char byte : text) {
                hash ^= byte;
                hash *= 1099511628211ull;
            }
            return hash;
        }

        std::string collisionSafePluginComponent(std::string_view plugin)
        {
            auto component = sanitizeForFileName(plugin);
            if (component == plugin) {
                return component;
            }
            char suffix[24]{};
            std::snprintf(suffix, sizeof(suffix), "~%016llX",
                static_cast<unsigned long long>(stableFileNameHash(plugin)));
            component += suffix;
            return component;
        }

        std::string libraryFileName(const FormRef& weapon)
        {
            char idText[16]{};
            std::snprintf(idText, sizeof(idText), "%08X", weapon.localFormId);
            return collisionSafePluginComponent(weapon.plugin) + "_" + idText + ".json";
        }

        // Read-only profiles shipped with the mod. A user file in Documents
        // always wins; this location only supplies a profile when no user
        // override exists. Mod-manager deployment maps this relative Data
        // path into the game exactly like the DLL.
        constexpr const char* kBundledLibraryDirectory =
            R"(Data\F4SE\Plugins\PAPERReduxMotionLibrary)";
    }

    MotionLibraryStore::MotionLibraryStore() :
        _directory(resolveLibraryDirectory())
    {
    }

    MotionLibraryStore::~MotionLibraryStore()
    {
        shutdown();
    }

    FormRef MotionLibraryStore::formRefFromRuntimeId(std::uint32_t runtimeFormId)
    {
        if (runtimeFormId == 0) {
            return {};
        }
        auto* form = RE::TESForm::GetFormByID(runtimeFormId);
        if (!form) {
            return {};
        }
        auto* file = form->GetFile(0);
        if (!file) {
            return {};
        }
        FormRef ref;
        ref.plugin = std::string(file->GetFilename());
        ref.localFormId = form->GetLocalFormID();
        if (ref.plugin.empty()) {
            return {};
        }
        return ref;
    }

    std::uint32_t MotionLibraryStore::runtimeIdFromFormRef(const FormRef& ref)
    {
        if (ref.empty()) {
            return 0;
        }
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            return 0;
        }
        auto* form = dataHandler->LookupForm(ref.localFormId, ref.plugin);
        return form ? form->GetFormID() : 0;
    }

    std::string MotionLibraryStore::filePathForWeapon(const FormRef& weapon) const
    {
        return _directory + "\\" + libraryFileName(weapon);
    }

    std::string MotionLibraryStore::captureFilePathForWeapon(const FormRef& weapon) const
    {
        char idText[16]{};
        std::snprintf(idText, sizeof(idText), "%08X", weapon.localFormId);
        return _directory + "\\" + collisionSafePluginComponent(weapon.plugin) + "_" + idText + ".capture.jsonl";
    }

    bool MotionLibraryStore::load(const FormRef& weapon, WeaponLibrary& out, std::string* outError) const
    {
        if (outError) {
            outError->clear();
        }
        if (weapon.empty()) {
            return false;
        }
        auto path = filePathForWeapon(weapon);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            // Migration for the rare plugin filename containing characters
            // the legacy sanitizer collapsed to '_'. New writes use a stable
            // hash suffix so two distinct Unicode/special names cannot share
            // a library. Existing legacy data remains readable.
            char idText[16]{};
            std::snprintf(idText, sizeof(idText), "%08X", weapon.localFormId);
            const auto legacyPath = _directory + "\\" + sanitizeForFileName(weapon.plugin) + "_" + idText + ".json";
            ec.clear();
            if (legacyPath != path && std::filesystem::exists(legacyPath, ec)) {
                path = legacyPath;
                RDX_LOG_WARN(Config,
                    "Motion library: loading legacy sanitized filename '{}' for plugin '{}'; the next save uses a collision-safe hash suffix",
                    std::filesystem::path(path).filename().string(),
                    weapon.plugin);
            } else {
                ec.clear();
                const auto bundledPath =
                    std::filesystem::path(kBundledLibraryDirectory) / libraryFileName(weapon);
                if (!std::filesystem::exists(bundledPath, ec)) {
                    return false;  // absent file: normal, empty error
                }
                path = bundledPath.string();
                RDX_LOG_INFO(Config,
                    "Motion library: loading bundled authoritative profile '{}' (no user override present)",
                    bundledPath.filename().string());
            }
        }
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            if (outError) {
                *outError = "file exists but could not be opened";
            }
            return false;
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        if (!parse(buffer.str(), out, outError)) {
            return false;
        }
        if (out.weapon.plugin != weapon.plugin || out.weapon.localFormId != weapon.localFormId) {
            if (outError) {
                *outError = "file weapon identity does not match requested weapon (possible legacy filename collision)";
            }
            out = WeaponLibrary{};
            return false;
        }
        return true;
    }

    void MotionLibraryStore::save(const WeaponLibrary& library)
    {
        if (library.weapon.empty()) {
            return;
        }
        PendingWrite write{ filePathForWeapon(library.weapon), serialize(library) };
        {
            std::lock_guard lock(_mutex);
            // Latest-wins per file: replace a still-pending write of the
            // same weapon instead of queueing behind it.
            bool replaced = false;
            for (auto& pending : _queue) {
                if (pending.path == write.path) {
                    pending.content = std::move(write.content);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                _queue.push_back(std::move(write));
            }
        }
        ensureWriterStarted();
        _wake.notify_one();
    }

    bool MotionLibraryStore::appendCapture(rich_capture::Event event)
    {
        const auto& context = rich_capture::contextOf(event);
        if (context.weapon.ref.empty()) {
            return false;
        }
        const auto estimatedBytes = rich_capture::estimateOwnedBytes(event);
        PendingCapture capture{
            captureFilePathForWeapon(context.weapon.ref),
            std::move(event),
            estimatedBytes,
        };
        {
            std::lock_guard lock(_mutex);
            if (_captureQueue.size() >= kMaxPendingCaptures || estimatedBytes > kMaxPendingCaptureBytes ||
                _captureQueuedBytes > kMaxPendingCaptureBytes - estimatedBytes) {
                return false;
            }
            _captureQueue.push_back(std::move(capture));
            _captureQueuedBytes += estimatedBytes;
        }
        ensureWriterStarted();
        _wake.notify_one();
        return true;
    }

    std::uint32_t MotionLibraryStore::deleteAllExceptCurated()
    {
        // Quiesce the writer first: clearing only the queued writes cannot
        // cancel a serving write already popped by the worker, whose atomic
        // replace could otherwise resurrect learned data after this wipe.
        // Capture appends drain too but their .jsonl files are intentionally
        // retained. The writer restarts lazily on the next save/capture.
        shutdown();
        {
            std::lock_guard lock(_mutex);
            _queue.clear();
            _captureQueuedBytes = 0;
        }
        std::uint32_t deleted = 0;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(_directory, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
                continue;
            }
            std::ifstream stream(entry.path(), std::ios::binary);
            if (stream) {
                std::ostringstream buffer;
                buffer << stream.rdbuf();
                WeaponLibrary library;
                if (parse(buffer.str(), library, nullptr) && library.curated) {
                    RDX_LOG_INFO(Config, "Motion library wipe: keeping curated file '{}'", entry.path().filename().string());
                    continue;
                }
            }
            stream.close();
            if (std::filesystem::remove(entry.path(), ec)) {
                ++deleted;
            }
        }
        return deleted;
    }

    void MotionLibraryStore::ensureWriterStarted()
    {
        std::lock_guard lock(_mutex);
        if (_writerStarted) {
            return;
        }
        _writerStarted = true;
        _stop = false;
        _writer = std::thread([this]() { writerLoop(); });
    }

    void MotionLibraryStore::writerLoop()
    {
        const auto recordCaptureFailure = [this](const PendingCapture& failed) {
            auto found = _captureFailures.find(failed.path);
            if (found == _captureFailures.end()) {
                if (_captureFailures.size() >= kMaxPendingCaptureFailures) {
                    RDX_LOG_WARN(Config,
                        "Motion capture: writer-failure table full; loss for '{}' cannot be journaled",
                        failed.path);
                    return;
                }
                const auto& context = rich_capture::contextOf(failed.event);
                PendingCaptureFailure failure{};
                failure.context = context;
                failure.firstSequence = context.sequence;
                found = _captureFailures.emplace(failed.path, std::move(failure)).first;
            }
            const auto sequence = rich_capture::contextOf(failed.event).sequence;
            found->second.lastSequence = sequence;
            if (found->second.count < (std::numeric_limits<std::uint32_t>::max)()) {
                ++found->second.count;
            }
        };

        for (;;) {
            PendingWrite write;
            PendingCapture capture;
            bool writeCapture = false;
            {
                std::unique_lock lock(_mutex);
                _wake.wait(lock, [this]() { return _stop || !_queue.empty() || !_captureQueue.empty(); });
                if (_queue.empty() && _captureQueue.empty()) {
                    if (_stop) {
                        return;
                    }
                    continue;
                }
                // Serving-map writes are rare and equip-facing; do not let a
                // burst of rich evidence delay them. Capture order remains
                // FIFO because it has its own queue.
                if (!_queue.empty()) {
                    write = std::move(_queue.front());
                    _queue.pop_front();
                } else {
                    capture = std::move(_captureQueue.front());
                    _captureQueue.pop_front();
                    _captureQueuedBytes -= (std::min)(_captureQueuedBytes, capture.estimatedBytes);
                    writeCapture = true;
                }
            }

            std::error_code ec;
            if (writeCapture) {
                std::filesystem::create_directories(std::filesystem::path(capture.path).parent_path(), ec);
                ec.clear();
                std::uintmax_t existingSize = 0;
                const bool captureExists = std::filesystem::exists(capture.path, ec);
                bool existingSizeKnown = !ec;
                if (captureExists && existingSizeKnown) {
                    existingSize = std::filesystem::file_size(capture.path, ec);
                    if (ec) {
                        existingSize = 0;
                        existingSizeKnown = false;
                    }
                }
                bool success = false;
                bool recoveredPartialTail = false;
                bool appendAttempted = false;
                try {
                    if (!existingSizeKnown) {
                        throw std::runtime_error("could not determine existing archive length safely");
                    }
                    bool needsLineBoundary = false;
                    if (!ec && existingSize > 0) {
                        std::ifstream existing(capture.path, std::ios::binary);
                        if (!existing) {
                            throw std::runtime_error("could not inspect existing archive tail safely");
                        }
                        existing.seekg(-1, std::ios::end);
                        char last = '\0';
                        existing.get(last);
                        if (!existing) {
                            throw std::runtime_error("could not read existing archive tail safely");
                        }
                        needsLineBoundary = last != '\n';
                    }

                    const auto line = rich_capture::serializeLine(capture.event);
                    std::string recoveryLine;
                    if (needsLineBoundary) {
                        auto recoveryContext = rich_capture::contextOf(capture.event);
                        recoveryContext.sequence = 0;  // reserved health/recovery row
                        rich_capture::CaptureGapEvent recovery{
                            .context = std::move(recoveryContext),
                            .firstDroppedSequence = 0,
                            .lastDroppedSequence = 0,
                            .droppedEventCount = 1,
                            .reason = "recovered unterminated JSONL tail from a prior interrupted append",
                        };
                        recoveryLine = rich_capture::serializeLine(rich_capture::Event{ std::move(recovery) });
                    }
                    std::string writerFailureLine;
                    const auto writerFailure = _captureFailures.find(capture.path);
                    if (writerFailure != _captureFailures.end()) {
                        auto failureContext = writerFailure->second.context;
                        failureContext.sequence = 0;  // reserved writer-health row
                        rich_capture::CaptureGapEvent failure{
                            .context = std::move(failureContext),
                            .firstDroppedSequence = writerFailure->second.firstSequence,
                            .lastDroppedSequence = writerFailure->second.lastSequence,
                            .droppedEventCount = writerFailure->second.count,
                            .reason = "previously accepted capture event(s) failed after writer retries",
                        };
                        writerFailureLine = rich_capture::serializeLine(
                            rich_capture::Event{ std::move(failure) });
                    }

                    appendAttempted = true;
                    std::ofstream stream(capture.path, std::ios::binary | std::ios::app);
                    success = static_cast<bool>(stream);
                    if (success) {
                        if (needsLineBoundary) {
                            stream.put('\n');
                            stream.write(recoveryLine.data(), static_cast<std::streamsize>(recoveryLine.size()));
                            stream.put('\n');
                        }
                        if (!writerFailureLine.empty()) {
                            stream.write(
                                writerFailureLine.data(),
                                static_cast<std::streamsize>(writerFailureLine.size()));
                            stream.put('\n');
                        }
                        stream.write(line.data(), static_cast<std::streamsize>(line.size()));
                        stream.put('\n');
                        stream.flush();
                        success = static_cast<bool>(stream);
                        recoveredPartialTail = success && needsLineBoundary;
                    }
                    if (success && !writerFailureLine.empty()) {
                        _captureFailures.erase(capture.path);
                    }
                } catch (const std::exception& error) {
                    RDX_LOG_WARN(Config,
                        "Motion capture: serialization/append of '{}' raised '{}'; event will retry",
                        capture.path,
                        error.what());
                    success = false;
                } catch (...) {
                    RDX_LOG_WARN(Config,
                        "Motion capture: serialization/append of '{}' raised an unknown exception; event will retry",
                        capture.path);
                    success = false;
                }
                if (recoveredPartialTail) {
                    RDX_LOG_WARN(Config,
                        "Motion capture: '{}' had an unterminated tail; isolated it and appended a captureGap recovery row",
                        capture.path);
                }
                if (!success) {
                    // Append may have written a prefix before failure. Roll
                    // back to the exact pre-attempt length so retry cannot
                    // concatenate or duplicate a partial JSON object.
                    ec.clear();
                    if (existingSizeKnown && appendAttempted &&
                        std::filesystem::exists(capture.path, ec)) {
                        std::filesystem::resize_file(capture.path, existingSize, ec);
                    }
                    ++capture.attempts;
                    if (capture.attempts < 3) {
                        std::lock_guard lock(_mutex);
                        if (_captureQueue.size() < kMaxPendingCaptures &&
                            _captureQueuedBytes <= kMaxPendingCaptureBytes - capture.estimatedBytes) {
                            _captureQueuedBytes += capture.estimatedBytes;
                            _captureQueue.push_front(std::move(capture));
                            _wake.notify_one();
                        } else {
                            recordCaptureFailure(capture);
                            RDX_LOG_WARN(Config,
                                "Motion capture: retry for '{}' exceeded the queue byte budget; event was not persisted",
                                capture.path);
                        }
                    } else {
                        recordCaptureFailure(capture);
                        RDX_LOG_WARN(Config,
                            "Motion capture: append to '{}' failed after {} attempts; this event was not persisted",
                            capture.path,
                            capture.attempts);
                    }
                }
                continue;
            }

            std::filesystem::create_directories(std::filesystem::path(write.path).parent_path(), ec);
            const auto tempPath = write.path + ".tmp";
            {
                std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
                if (!stream) {
                    RDX_LOG_WARN(Config, "Motion library: could not open '{}' for writing", tempPath);
                    continue;
                }
                stream.write(write.content.data(), static_cast<std::streamsize>(write.content.size()));
                if (!stream) {
                    RDX_LOG_WARN(Config, "Motion library: write to '{}' failed", tempPath);
                    continue;
                }
            }
            // std::filesystem::rename does not portably replace an existing
            // destination on Windows. MoveFileEx gives the serving map a
            // real atomic replace and requests metadata flush to disk.
            if (!::MoveFileExA(
                    tempPath.c_str(),
                    write.path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                RDX_LOG_WARN(Config,
                    "Motion library: atomic replace of '{}' failed (Win32 error {})",
                    write.path,
                    ::GetLastError());
            }
        }
    }

    void MotionLibraryStore::shutdown()
    {
        {
            std::lock_guard lock(_mutex);
            if (!_writerStarted) {
                return;
            }
            _stop = true;
        }
        _wake.notify_one();
        if (_writer.joinable()) {
            _writer.join();
        }
        std::lock_guard lock(_mutex);
        _writerStarted = false;
    }
}
