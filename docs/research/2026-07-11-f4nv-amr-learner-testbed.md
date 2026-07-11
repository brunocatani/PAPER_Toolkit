# F4NV-AMR curated learner-motion testbed

Date: 2026-07-11

Project: PAPER_Redux

Weapon: `F4NV-AMR.esp:0x00000F99` (`Anti-Materiel Rifle`)

Sources: preserved `F4NV-AMR.esp_00000F99.json`, preserved `F4NV-AMR.esp_00000F99.capture.jsonl`, current PAPER_Redux learner/sandbox/profile code, and the user's in-game confirmation that the uncurated learner motion is correct

Verification: SHA-256 preservation, JSON/capture inventory, hierarchy/OMOD comparison, exact key-value comparison, generator reproduction, parser round trip, synthetic controller traversal, tests, `custom-fast`, and live testing still pending for the enriched profile

Confidence: high for identity, stored movement values, hierarchy, grouping, transition contract, and marker names; in-game feel and sound descriptor resolution remain pending

## Preserved movement authority

The original files were copied before any modification:

```text
capture SHA-256: 3A97E0B01849E0B4DC87E3F317C08AC57029A4765AE0E582B51D79F71A8FB60D
library SHA-256: 38D45F0A3F33701DC0A7EB92C323668BFF6B8ED24D92F4ED30E12044D8E739BA
```

The active source library contains 23 part records. The curated generator retains the complete `parts` value unchanged and builds its control/driver paths from those exact `learnedPrimary` and `learnedReturn` keys.

The important correction from Ozzy is input semantics: hand translation advances the path; hand rotation does not. Recorded quaternion keys still rotate parts as the learner path advances.

## Rich capture inventory

The capture contains:

- one `weaponSnapshot` with 49 named nodes and eight copied physical evidence entries;
- 90 raw stroke records;
- 252 authored-clip records, including one activated reload clip;
- eight deferred geometry chunks;
- exact weapon identity `F4NV-AMR.esp:0x00000F99`;
- rifle/sniper/ballistic classification.

The activated reload clip is `Animations\44Pistol\WPNReload.hkt`, activity `23`, duration `6.7` seconds. Duration is provenance only and is not serialized as runtime authority.

## Bolt group

Physical grip:

- `HuntingRifleBolt:0`, classified by ROCK as `Bolt` / action role `Bolt`.

Independent top-level drivers:

- `WeaponBolt`, carrying `HuntingRifleBolt:0`;
- `WeaponExtra2`, carrying `HuntingRifleFiringPin:0`;
- `WeaponExtra3`, carrying `HuntingRifleBackBolt:0`.

The opening control path is the exact `HuntingRifleBolt:0.learnedReturn` curve. Driver direction is selected from each node's known-good learned stage:

- opening: `WeaponBolt.learnedReturn`, `WeaponExtra2.learnedReturn`, `WeaponExtra3.learnedPrimary`;
- closing: `WeaponBolt.learnedPrimary`, `WeaponExtra2.learnedPrimary`, `WeaponExtra3.learnedReturn`.

This explicit direction selection avoids treating learner stage labels as semantic truth: recordings may begin while a mechanism is open or closed. Physical endpoints and actual key direction decide the stage.

## Magazine group

Physical grip:

- `BSX`, classified as `Magazine` / reload role `MagazineBody`;
- exact OMOD `F4NV-AMR.esp:0x00006B57`;
- attach point `Fallout4.esm:0x0005D4D7`, editor ID `ap_gun_Mag`, name `Magazine`.

Top-level driver:

- `WeaponMagazine`, carrying the full magazine subtree.

`P-Mag` lies between `WeaponMagazine` and `BSX`. It is connector evidence only. PAPER drives `WeaponMagazine`, not `P-Mag` and not every descendant separately.

The removal control is the exact `BSX.learnedPrimary` path. The insertion control is the exact `BSX.learnedReturn` path. `WeaponMagazine` supplies the matching exact driver stages.

The curated interaction uses removal `0.00 -> 0.20`, then insertion `0.80 -> 1.00`. The raw poses at those two gates are not identical, so the controller preserves the outgoing driver pose at handoff and fades the geometric correction to zero by the seated endpoint.

## Captured reload annotations

| Captured annotation | Curated use |
|---|---|
| `SoundPlay.WPN1AMRstart` | direct sound on bolt grip start |
| `SoundPlay.WPN1AMRboltopen` | direct sound at bolt-open endpoint |
| `SoundPlay.WPN1AMRmagrel` | direct sound at magazine removal path position `0.05` |
| `SoundPlay.WPN1AMRmagout` | direct sound at magazine exchange position `0.20` |
| `SoundPlay.WPN1AMRmagin` | direct sound at insertion position `0.90`, separated from the exchange sound |
| `SoundPlay.WPN1AMRboltclose` | direct sound halfway through bolt closing |
| `SoundPlay.WPN1AMRend` | direct sound at bolt-closed endpoint |
| `CullBone.WeaponMagazineChild1` | inert visibility evidence |
| `UnCullBone.WeaponMagazineChild1` | inert visibility evidence |
| `reloadComplete` | inert gameplay evidence |
| `initiateStart` | inert graph evidence |
| `reloadEnd` | inert graph-exit evidence |

Sound command spelling is normalized to `Soundplay.<descriptor>` in the profile. PAPER strips that prefix and requests the descriptor directly. No captured marker is sent to the animation graph.

## First live-test correction

The 18:44 runtime log proved that bolt sounds were re-arming: `reload_start`, `bolt_open`, `bolt_close`, and `reload_end` all played successfully across repeated cycles. The audible loss came from placement, not a one-shot latch. `bolt_open`/`bolt_close` and `mag_out`/`mag_in` had been assigned to the same transition frames, allowing one descriptor to mask the other.

Return sounds are now spatially separated: bolt-close at closing position `0.50`, end at the closed endpoint, mag-out at the `0.20` exchange, and mag-in at insertion position `0.90`.

### Direct-visibility regression and rollback

The first attempt to apply `CullBone.WeaponMagazineChild1`/`UnCullBone.WeaponMagazineChild1` directly through scene-node cull state regressed the next live test. The profile still bound, both attach-only targets installed, and the magazine grip was recognized at `19:07:19.386`, but the spatial path never advanced far enough to reach its first `0.05` sound. Because the new visibility layer was the only behavior introduced at grip activation, it was removed completely from controller and runtime output. The captured markers remain inert evidence until mesh exchange has a movement-safe implementation.

## Test checklist

1. Equip exact form `F4NV-AMR.esp:0x00000F99` with OMOD `0x00006B57` installed on `BSX`.
2. Do not begin a native reload.
3. Grip the bolt and verify the direction and feel match the preserved learner behavior.
4. Rotate the wrist without translating and confirm the path does not advance.
5. Open/close continuously and confirm all three top-level bolt drivers move once, with no doubled descendant travel.
6. Release and re-grip; confirm the cycle resets cleanly.
7. Grip the magazine and confirm `WeaponMagazine` carries the complete subtree while `P-Mag` is never independently driven.
8. Cross removal positions `0.05` and `0.20`; verify release/out sounds fire once.
9. Continue through insertion position `0.90`; verify the mag-in sound is audible separately from mag-out.
10. Confirm the `0.20 -> 0.80` transition has no visible teleport and insertion reaches the exact seated endpoint.
11. Open and close the bolt for at least two continuous cycles; verify bolt-open, bolt-close, and end each remain separately audible on every cycle.
12. Start and finish a normal native reload separately; confirm PAPER neither holds nor completes it.
