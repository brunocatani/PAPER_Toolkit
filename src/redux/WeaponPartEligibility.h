#pragma once

#include <cstdint>

#include "api/ROCKProviderApi.h"

namespace redux
{
    /*
     * INI-selected allowlist of part CLASSES that may become AttachOnly
     * grips — one boolean per class (bAttachOnlyBolt, bAttachOnlyStock, ...).
     * This is only half the gate: a concrete part must ALSO have a motion
     * path under the active MotionPathMode (mapped by the clip harvest or
     * taught to the learner) to be whitelisted with ROCK — a part that does
     * not move is never attach-only and never grouped, no matter what the
     * allowlist says. Eligibility is therefore resolved per PART, per
     * weapon, at runtime (ReduxRuntime::refreshEligibleParts) and encoded
     * as generation-pinned scene-source provider targets.
     */
    struct AttachOnlyAllowList
    {
        // Bit N = RockProviderWeaponActionRoleV1 / ...PartKindV1 value N.
        std::uint32_t actionRoleMask{ 0 };
        std::uint32_t partKindMask{ 0 };

        [[nodiscard]] constexpr bool allows(
            rock::provider::RockProviderWeaponActionRoleV1 actionRole,
            rock::provider::RockProviderWeaponPartKindV1 partKind) const
        {
            return ((actionRoleMask >> static_cast<std::uint32_t>(actionRole)) & 1u) != 0 ||
                   ((partKindMask >> static_cast<std::uint32_t>(partKind)) & 1u) != 0;
        }

        [[nodiscard]] constexpr bool operator==(const AttachOnlyAllowList&) const = default;
    };

    /*
     * One INI boolean per part class. Keys that name both an action role and
     * a part kind (bolt, slide, pump, ...) enable both, since either
     * classification can carry the part. Defaults reproduce the
     * pre-allowlist behavior: every reciprocating/hinged action part plus
     * the feed chain on, everything else off. (Latch and Receiver default
     * off — Receiver's name bucket is a catch-all that hijacked support
     * grips when it was tried on 2026-07-03; both stay INI-selectable.)
     */
    struct AttachOnlyPartKey
    {
        // Full INI key, e.g. "bAttachOnlyBolt".
        const char* iniKey;
        // Short class name for logs, e.g. "bolt".
        const char* name;
        bool hasActionRole;
        rock::provider::RockProviderWeaponActionRoleV1 actionRole;
        bool hasPartKind;
        rock::provider::RockProviderWeaponPartKindV1 partKind;
        bool defaultOn;
    };

    namespace detail
    {
        using ActionRole = rock::provider::RockProviderWeaponActionRoleV1;
        using PartKind = rock::provider::RockProviderWeaponPartKindV1;
    }

    inline constexpr AttachOnlyPartKey kAttachOnlyPartKeys[] = {
        // Reciprocating / hinged action parts (role + kind).
        { "bAttachOnlyBolt", "bolt", true, detail::ActionRole::Bolt, true, detail::PartKind::Bolt, true },
        { "bAttachOnlySlide", "slide", true, detail::ActionRole::Slide, true, detail::PartKind::Slide, true },
        { "bAttachOnlyChargingHandle", "chargingHandle", true, detail::ActionRole::ChargingHandle, true, detail::PartKind::ChargingHandle, true },
        { "bAttachOnlyPump", "pump", true, detail::ActionRole::Pump, true, detail::PartKind::Pump, true },
        { "bAttachOnlyBreakAction", "breakAction", true, detail::ActionRole::BreakAction, true, detail::PartKind::BreakAction, true },
        { "bAttachOnlyCylinder", "cylinder", true, detail::ActionRole::Cylinder, true, detail::PartKind::Cylinder, true },
        { "bAttachOnlyLever", "lever", true, detail::ActionRole::Lever, true, detail::PartKind::Lever, true },
        { "bAttachOnlyLatch", "latch", true, detail::ActionRole::Latch, false, detail::PartKind::Other, false },
        // Feed chain (kind only).
        { "bAttachOnlyMagazine", "magazine", false, detail::ActionRole::None, true, detail::PartKind::Magazine, true },
        { "bAttachOnlyMagwell", "magwell", false, detail::ActionRole::None, true, detail::PartKind::Magwell, true },
        { "bAttachOnlyChamber", "chamber", false, detail::ActionRole::None, true, detail::PartKind::Chamber, true },
        { "bAttachOnlyShell", "shell", false, detail::ActionRole::None, true, detail::PartKind::Shell, true },
        { "bAttachOnlyRound", "round", false, detail::ActionRole::None, true, detail::PartKind::Round, true },
        { "bAttachOnlyLaserCell", "laserCell", false, detail::ActionRole::None, true, detail::PartKind::LaserCell, true },
        { "bAttachOnlyCosmeticAmmo", "cosmeticAmmo", false, detail::ActionRole::None, true, detail::PartKind::CosmeticAmmo, true },
        // Structural / accessory classes, default off.
        { "bAttachOnlyReceiver", "receiver", false, detail::ActionRole::None, true, detail::PartKind::Receiver, false },
        { "bAttachOnlyBarrel", "barrel", false, detail::ActionRole::None, true, detail::PartKind::Barrel, false },
        { "bAttachOnlyHandguard", "handguard", false, detail::ActionRole::None, true, detail::PartKind::Handguard, false },
        { "bAttachOnlyForegrip", "foregrip", false, detail::ActionRole::None, true, detail::PartKind::Foregrip, false },
        { "bAttachOnlyStock", "stock", false, detail::ActionRole::None, true, detail::PartKind::Stock, false },
        { "bAttachOnlyGrip", "grip", false, detail::ActionRole::None, true, detail::PartKind::Grip, false },
        { "bAttachOnlySight", "sight", false, detail::ActionRole::None, true, detail::PartKind::Sight, false },
        { "bAttachOnlyAccessory", "accessory", false, detail::ActionRole::None, true, detail::PartKind::Accessory, false },
        { "bAttachOnlyOther", "other", false, detail::ActionRole::None, true, detail::PartKind::Other, false },
    };

    // Fold one key's boolean into the masks.
    inline constexpr void applyAttachOnlyPartKey(const AttachOnlyPartKey& key, bool enabled, AttachOnlyAllowList& list)
    {
        if (!enabled) {
            return;
        }
        if (key.hasActionRole) {
            list.actionRoleMask |= 1u << static_cast<std::uint32_t>(key.actionRole);
        }
        if (key.hasPartKind) {
            list.partKindMask |= 1u << static_cast<std::uint32_t>(key.partKind);
        }
    }

    [[nodiscard]] inline constexpr AttachOnlyAllowList defaultAttachOnlyAllowList()
    {
        AttachOnlyAllowList list{};
        for (const auto& key : kAttachOnlyPartKeys) {
            applyAttachOnlyPartKey(key, key.defaultOn, list);
        }
        return list;
    }
}
