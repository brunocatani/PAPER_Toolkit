#pragma once

#include <cstdint>

namespace RE
{
    class Actor;
}

namespace paper_toolkit
{
    enum class ShellEjectResult : std::uint8_t
    {
        // The engine eject call was made (the engine itself still no-ops
        // silently when the weapon has no casing model or P-Casing node).
        Ejected,
        // No weapon resolved at the weapon equip index.
        NoWeapon,
        // A weapon resolved but its formID is not the weapon the runtime is
        // path driving — fail closed rather than eject the wrong thing.
        WeaponMismatch,
    };

    /*
     * Fires the engine's own shell-casing ejection for the actor's currently
     * equipped weapon — the exact call chain the "EjectShellCasing" anim
     * event runs on every fired shot: resolve the equipped weapon instance,
     * spawn the casing debris at the weapon's P-Casing node, release the
     * instance-data ref. Main-thread only (the spawn touches the shooter's
     * loaded 3D and parent cell).
     */
    ShellEjectResult ejectShellCasingForEquippedWeapon(RE::Actor& shooter, std::uint32_t expectedWeaponFormId);
}
