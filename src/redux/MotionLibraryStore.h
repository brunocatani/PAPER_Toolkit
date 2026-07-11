#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "redux/MotionLibraryFormat.h"
#include "redux/RichMotionCaptureFormat.h"

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
     *    atomically replaced so a crash mid-write cannot corrupt a library.
     *  - appendCapture() moves an owned rich-capture event into a separate
     *    bounded queue. JSON construction and append I/O both happen on the
     *    writer thread. Capture events are never coalesced or replaced.
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
        [[nodiscard]] std::string captureFilePathForWeapon(const FormRef& weapon) const;

        // Synchronous read+parse; false when the file is absent or unusable
        // (outError says which; absent file sets an empty error).
        bool load(const FormRef& weapon, WeaponLibrary& out, std::string* outError) const;

        // Serialize on the calling (frame) thread, write on the writer
        // thread. No-op when the weapon ref is empty.
        void save(const WeaponLibrary& library);

        // Move one immutable capture event to the background append queue.
        // False means the bounded queue was full (the runtime records a
        // visible capture-gap event on the next successful enqueue).
        bool appendCapture(rich_capture::Event event);

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

        struct PendingCapture
        {
            std::string path;
            rich_capture::Event event;
            std::size_t estimatedBytes{ 0 };
            std::uint32_t attempts{ 0 };
        };

        struct PendingCaptureFailure
        {
            rich_capture::EventContext context{};
            std::uint64_t firstSequence{ 0 };
            std::uint64_t lastSequence{ 0 };
            std::uint32_t count{ 0 };
        };

        static constexpr std::size_t kMaxPendingCaptures = 256;
        static constexpr std::size_t kMaxPendingCaptureBytes = 128u * 1024u * 1024u;
        static constexpr std::size_t kMaxPendingCaptureFailures = 64;

        void ensureWriterStarted();
        void writerLoop();

        std::string _directory;
        std::thread _writer;
        std::mutex _mutex;
        std::condition_variable _wake;
        std::deque<PendingWrite> _queue;
        std::deque<PendingCapture> _captureQueue;
        std::size_t _captureQueuedBytes{ 0 };
        // Writer-thread-owned recovery state. A later successful append to
        // the same weapon archive emits a captureGap before its event.
        std::unordered_map<std::string, PendingCaptureFailure> _captureFailures;
        bool _stop{ false };
        bool _writerStarted{ false };
    };
}
