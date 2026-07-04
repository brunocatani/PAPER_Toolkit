#pragma once

namespace redux
{
    /*
     * Minimal INI-backed configuration. The whole plugin is the weapon-part
     * reload runtime, so the only gameplay switch is the master enable; all
     * behavior tuning lives in the policy constants, exactly as it did inside
     * ROCK's bolt-drive sandbox. The INI is optional: missing file or missing
     * keys keep the compiled defaults.
     *
     * Location: Data\F4SE\Plugins\PAPERRedux.ini (standard F4SE plugin
     * config path, resolved against the game working directory).
     */
    struct ReduxConfig
    {
        bool enabled = true;

        // Loads (or reloads) from the INI; safe to call on session resets.
        void load();
    };

    inline ReduxConfig g_reduxConfig{};
}
