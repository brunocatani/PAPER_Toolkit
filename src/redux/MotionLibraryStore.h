#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "redux/MotionLibraryFormat.h"

namespace redux::motion_library
{
    /*
     * Disk side of the motion library: file naming, runtime-formId <->
     * plugin-local identity conversion, and the write path.
     *
     * Threading model (documented per the runtime rules):
     *  - Every public method is FRAME-THREAD ONLY.
     *  - save() enqueues a pre-serialized string for a single background
     *    writer thread (latest-wins per file, bounded queue) — writes never
     *    touch the frame thread. Files are written to a .tmp sibling and
     *    renamed into place so a crash mid-write cannot corrupt a library.
     *  - load() reads synchronously ON THE WEAPON-EQUIP EVENT only (never
     *    per frame): a few KB, the same class as config loads.
     *  - shutdown() drains the queue and joins the writer thread.
     */
    class MotionLibraryStore
    {
    public:
        MotionLibraryStore();
        ~MotionLibraryStore();

        // Runtime formId -> load-order-independent ref (empty on failure /
        // formId 0). Engine lookup; frame thread only.
        [[nodiscard]] static FormRef formRefFromRuntimeId(std::uint32_t runtimeFormId);
        // Load-order-independent ref -> runtime formId (0 when the plugin is
        // not loaded or the form does not exist). Frame thread only.
        [[nodiscard]] static std::uint32_t runtimeIdFromFormRef(const FormRef& ref);

        [[nodiscard]] const std::string& directory() const { return _directory; }
        [[nodiscard]] std::string filePathForWeapon(const FormRef& weapon) const;

        // Synchronous read+parse; false when the file is absent or unusable
        // (outError says which; absent file sets an empty error).
        bool load(const FormRef& weapon, WeaponLibrary& out, std::string* outError) const;

        // Serialize on the calling (frame) thread, write on the writer
        // thread. No-op when the weapon ref is empty.
        void save(const WeaponLibrary& library);

        // Re-record wipe support: delete every library file EXCEPT curated
        // ones (hand-tuned data survives a wipe). Synchronous — this runs on
        // an explicit, rare user action, not in normal play. Returns the
        // number of files deleted.
        std::uint32_t deleteAllExceptCurated();

        void shutdown();

    private:
        struct PendingWrite
        {
            std::string path;
            std::string content;
        };

        void ensureWriterStarted();
        void writerLoop();

        std::string _directory;
        std::thread _writer;
        std::mutex _mutex;
        std::condition_variable _wake;
        std::deque<PendingWrite> _queue;
        bool _stop{ false };
        bool _writerStarted{ false };
    };
}
