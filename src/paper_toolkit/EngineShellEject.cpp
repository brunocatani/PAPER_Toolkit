#include "paper_toolkit/EngineShellEject.h"

namespace paper_toolkit
{
    namespace
    {
        /*
         * FO4VR module offsets, verified 2026-07-05 with two agreeing sources
         * per hop (VR address library symbol names + Ghidra raw disassembly
         * of the FO4VR binary):
         *
         *  - 0x330C80  TESObjectWEAP::EjectShellCasing(TESObjectREFR&,
         *    BGSObjectInstanceT<TESObjectWEAP>&, BGSEquipIndex) — addr-lib ID
         *    1578899. Disassembly landmarks: shooter formtype 0x41 (Actor)
         *    check, static init of the "P-Casing" node-name string
         *    (s_P-Casing global), non-empty casing-model requirement on the
         *    weapon instance, node-near-shooter distance gate, randomized
         *    eject velocity, debris spawn into the shooter's parent cell.
         *    Every missing piece fails closed inside the engine.
         *
         *  - 0xE50DA0  Actor::GetCurrentWeapon(BGSObjectInstance* out,
         *    BGSEquipIndex) — addr-lib ID 1277201; thin wrapper over
         *    Actor::GetCurrentItem<TESObjectWEAP,43> (ID 1074514; the WEAP
         *    formtype 0x2B gate is visible in the disassembly). Constructs
         *    the out instance IN PLACE (BGSObjectInstance ctor, ID 1095748)
         *    and grants the caller one owned ref on the instance data; a
         *    missing/non-weapon item yields a null-form instance.
         *
         * The call shape below clones EjectShellCasingHandler::executeHandler
         * — the engine's own anim-event path for shell ejection — so this
         * inherits exactly the behavior of a fired shot's casing.
         */
        constexpr std::uintptr_t kGetCurrentWeaponOffset = 0xE50DA0;
        constexpr std::uintptr_t kEjectShellCasingOffset = 0x330C80;

        // Weapons live at BGSEquipIndex 0; the engine handler resolves the
        // same index when the anim event carries no payload.
        constexpr std::uint32_t kWeaponEquipIndex = 0;

        /*
         * Raw mirror of RE::BGSObjectInstance {TESForm*, TBO_InstanceData*}.
         * The commonlib type's constructor calls the engine ctor — but
         * GetCurrentWeapon constructs its out param itself, so it must
         * receive plain zeroed storage, not an already-constructed object.
         */
        struct WeaponInstanceRaw
        {
            RE::TESForm* object{ nullptr };
            RE::TBO_InstanceData* instanceData{ nullptr };
        };
        static_assert(sizeof(WeaponInstanceRaw) == 0x10);

        using GetCurrentWeaponFn = WeaponInstanceRaw* (*)(RE::Actor*, WeaponInstanceRaw*, std::uint32_t);
        using EjectShellCasingFn = void* (*)(RE::TESObjectREFR*, WeaponInstanceRaw*, std::uint32_t);
    }

    ShellEjectResult ejectShellCasingForEquippedWeapon(RE::Actor& shooter, const std::uint32_t expectedWeaponFormId)
    {
        const auto base = REL::Module::get().base();
        const auto getCurrentWeapon = reinterpret_cast<GetCurrentWeaponFn>(base + kGetCurrentWeaponOffset);
        const auto ejectShellCasing = reinterpret_cast<EjectShellCasingFn>(base + kEjectShellCasingOffset);

        WeaponInstanceRaw instance{};
        getCurrentWeapon(&shooter, &instance, kWeaponEquipIndex);

        auto result = ShellEjectResult::NoWeapon;
        if (instance.object) {
            if (instance.object->formID == expectedWeaponFormId) {
                ejectShellCasing(&shooter, &instance, kWeaponEquipIndex);
                result = ShellEjectResult::Ejected;
            } else {
                result = ShellEjectResult::WeaponMismatch;
            }
        }
        // Release the owned instance-data ref GetCurrentWeapon granted. The
        // deleting destructor is virtual, so `delete` dispatches into the
        // engine's own deallocation — the same dec + destroy-on-zero the
        // engine handler performs after its eject call.
        if (instance.instanceData && instance.instanceData->DecRef() == 0) {
            delete instance.instanceData;
        }
        return result;
    }
}
