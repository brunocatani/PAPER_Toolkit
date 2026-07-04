#pragma once

#include "api/ROCKProviderApi.h"

namespace redux
{
    /*
     * GRAB eligibility for the weapon-part drive runtime, shared by the
     * provider whitelist installed on ROCK and the grip-report filter here.
     * This gates only what a hand grab treats as an AttachOnly drive part —
     * observation is unrestricted: the drive-part cache and the motion
     * learner see EVERY evidence part, so ungrabbable parts still record,
     * group and follow.
     * Grabbable: every reciprocating or hinged action part, plus the feed
     * chain (magazine bodies, magwell/chamber sockets, shells, rounds,
     * laser cells, cosmetic ammo). Latch stays out (no name token or motion
     * source yet). Receiver was tried and removed 2026-07-03 — its name
     * bucket is a catch-all ("frame"/"body"/"pistol"/"weapon") that hijacked
     * receiver support grips into AttachOnly glue.
     *
     * Typed on ROCK's public V1 wire enums; ROCK static_asserts them against
     * its internal classification enums, so these values cannot drift.
     */
    [[nodiscard]] inline constexpr bool weaponPartDriveEligible(
        rock::provider::RockProviderWeaponActionRoleV1 actionRole,
        rock::provider::RockProviderWeaponPartKindV1 partKind)
    {
        using ActionRole = rock::provider::RockProviderWeaponActionRoleV1;
        using PartKind = rock::provider::RockProviderWeaponPartKindV1;
        switch (actionRole) {
        case ActionRole::Bolt:
        case ActionRole::Slide:
        case ActionRole::ChargingHandle:
        case ActionRole::Pump:
        case ActionRole::BreakAction:
        case ActionRole::Cylinder:
        case ActionRole::Lever:
            return true;
        default:
            break;
        }
        switch (partKind) {
        case PartKind::Magazine:
        case PartKind::Magwell:
        case PartKind::Chamber:
        case PartKind::Shell:
        case PartKind::Round:
        case PartKind::LaserCell:
        case PartKind::CosmeticAmmo:
            return true;
        default:
            return false;
        }
    }
}
