#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include "redux/MotionPathMode.h"

namespace filewatch
{
    template <class T>
    class FileWatch;
}

namespace redux
{
    /*
     * INI-backed configuration, hot-reloadable like ROCK's: a FileWatch on
     * the INI sets a pending flag from its watcher thread; the frame thread
     * applies it via processPendingReload() (debounced, so editors that
     * fire several change events per save reload once). All value reads
     * happen on the frame thread only.
     *
     * Location follows ROCK's convention — outside the game's Data tree so
     * MO2's VFS cannot interfere with file watching:
     *   Documents\My Games\Fallout4VR\PAPERRedux_Config\PAPERRedux.ini
     * A missing file is created with the compiled defaults on first load so
     * there is always a live-editable file.
     */
    struct ReduxConfig
    {
        bool enabled = true;
        MotionPathMode motionPathMode = MotionPathMode::Hybrid;
        // spdlog level: 0=trace 1=debug 2=info 3=warn 4=error 5=critical 6=off.
        int logLevel = 2;

        // First load: resolve the path, create the default INI if missing,
        // parse, apply the log level, start the file watch.
        void load();

        // Frame-thread poll; applies a debounced pending file change.
        // Returns true when a reload was applied this call.
        bool processPendingReload();

        // Out-of-line where FileWatch is complete: the unique_ptr member
        // makes the implicitly generated ctor/dtor require the full type.
        ReduxConfig();
        ~ReduxConfig();

    private:
        void parseIni(bool logDiff);
        void writeDefaultIniIfMissing();
        void startFileWatch();

        std::string _iniFilePath;
        std::unique_ptr<filewatch::FileWatch<std::string>> _fileWatch;
        std::atomic<bool> _reloadPending{ false };
        std::atomic<std::int64_t> _lastChangeEventMs{ 0 };
    };

    inline ReduxConfig g_reduxConfig{};
}
