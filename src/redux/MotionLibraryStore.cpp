#include "redux/MotionLibraryStore.h"

#include "ReduxLog.h"

#include <ShlObj.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

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
        char idText[16]{};
        std::snprintf(idText, sizeof(idText), "%08X", weapon.localFormId);
        return _directory + "\\" + sanitizeForFileName(weapon.plugin) + "_" + idText + ".json";
    }

    bool MotionLibraryStore::load(const FormRef& weapon, WeaponLibrary& out, std::string* outError) const
    {
        if (outError) {
            outError->clear();
        }
        if (weapon.empty()) {
            return false;
        }
        const auto path = filePathForWeapon(weapon);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            return false;  // absent file: normal, empty error
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
        return parse(buffer.str(), out, outError);
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

    std::uint32_t MotionLibraryStore::deleteAllExceptCurated()
    {
        // Nothing pending may resurrect a deleted file after the wipe.
        {
            std::lock_guard lock(_mutex);
            _queue.clear();
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
        for (;;) {
            PendingWrite write;
            {
                std::unique_lock lock(_mutex);
                _wake.wait(lock, [this]() { return _stop || !_queue.empty(); });
                if (_queue.empty()) {
                    if (_stop) {
                        return;
                    }
                    continue;
                }
                write = std::move(_queue.front());
                _queue.pop_front();
            }

            std::error_code ec;
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
            std::filesystem::rename(tempPath, write.path, ec);
            if (ec) {
                RDX_LOG_WARN(Config, "Motion library: rename to '{}' failed: {}", write.path, ec.message());
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
