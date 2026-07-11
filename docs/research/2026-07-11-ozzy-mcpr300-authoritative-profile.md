# Ozzy MCPR-300 authoritative reload profile

Date: 2026-07-11
Project: PAPER Redux
Weapon: `MCPR-300 - Watchdog 141`
Identity: `Ozzys_MCPR-300.esp` / local form `0x000020A6`
Runtime form observed: `0x470020A6`
Archetype: `bolt_action_detachable_magazine`
Confidence: high for captured hierarchy, authored reload timing, and grouping; pending first in-game validation for manual graph-event replay and interaction feel

## Durable artifacts

Forensic source:

```text
C:\Users\SENECA\Documents\My Games\Fallout4VR\PAPERRedux_Config\MotionLibrary\Ozzys_MCPR-300.esp_000020A6.capture.jsonl
```

Curated source/deployed profile:

```text
data\MotionLibrary\Ozzys_MCPR-300.esp_000020A6.json
```

Validated deployment SHA-256: `794AAB58530BFDC5EB4B76BA8EF652641C5BC74F68736F1B19DD66F7DF47CFB0`.

The serving profile is intentionally small and does not duplicate clip transforms. The forensic archive remains the immutable source for future refinement.

## Capture health

The archive contains 724 valid JSONL records and no capture-gap records:

| Event type | Count |
|---|---:|
| Weapon snapshot | 1 |
| Geometry chunk | 8 |
| Raw learner stroke | 133 |
| Authored clip | 582 |

Additional facts:

- one capture session;
- ROCK frames `12530..16079`;
- weapon generation `0x702B44D8EE141F3C`;
- 78 discovered nodes;
- only one duplicated node name (`P-Grip`, twice); both are connection
  subnodes, and no driver, follower, or grip identity used by this profile is
  duplicated;
- zero omitted/truncated node-catalog entries;
- 34 body-backed ROCK observation targets;
- 43 additional observation-only scene nodes;
- eight captured geometry clouds, all complete.

The snapshot copied full evidence detail for only 8 of 34 ROCK parts. The observation cache still retained all 34 body/source identities, which is why the authoritative profile can bind its grips, but roles/attach points/geometry are incomplete for the later 26 evidence entries. Lifting that evidence-detail limit may require a ROCK-side API extension and therefore still requires explicit user approval before ROCK is modified.

## Why the old serving map was unsafe

The prior auto-generated file was approximately 8.2 MB with 49 serving part records. It mixed unrelated animation evidence into the same winner-selection system:

- `WPNFireSingleSighted` trigger/recoil strokes;
- `WPNBoltChargeSighted` action cycling;
- `WPNRunForwardReady` locomotion layered over the real reload;
- idle noise;
- the actual `WPNReload` motion.

The activated reload was activity 173. A concurrent `WPNRunForwardReady` layer was activity 174, and raw learner samples stored only the newest active layer. Most genuine reload movement was therefore labeled as running.

Consequences visible in the generated map included:

- legitimate reload candidates rejected as smaller than prior bolt/run paths;
- magazine return paths around 45–55 game units;
- a `WeaponMagazineChild5` learned return exceeding 300 units in the runtime log;
- firing-trigger parts becoming AttachOnly candidates;
- large follower sets truncated at ten and populated by arbitrary ancestor/descendant combinations;
- later recordings overwriting mechanically correct earlier mappings.

The authoritative profile does not consume any of those winner records.

## Authored clip redundancy

The archive has 582 authored-clip records but only 50 unique clip definitions by duration and full track content. Twenty-nine unnamed fallback definitions repeat exactly nineteen times each. This redundancy explains much of the 38 MB archive size but does not invalidate the activated reload record.

The profile uses only the activated `Animations\44Pistol\WPNReload.hkt` record with activity ID 173 and duration `6.583333492279053` seconds.

## Exact hierarchy and grouping

`P-*` nodes are connection subnodes. A larger NIF part may contain one, but
the connector is not the mesh, collider, whole part, or a proxy for the whole
part. Connectors remain useful hierarchy evidence and can carry descendant
meshes; grouping still comes from the complete animated driver/descendant
structure, and only concrete mesh/collider identities can be gripped.

### Bolt action

Static connection subnode inside the bolt assembly (not the whole part):

```text
P-Bolt
```

Independent animated drivers:

```text
WeaponExtra2  -> WeaponExtra2Helper -> MCPR_3000_SEModelMesh_022:0
WeaponExtra1  -> WeaponExtra1Helper -> MCPR_3000_SEModelMesh_007:0
WeaponBolt    -> WeaponBoltHelper   -> MCPR_3000_SEModelMesh_008:0
```

Interpretation:

- `WeaponExtra2`: handle unlock/lock rotation;
- `WeaponExtra1`: linear bolt slide;
- `WeaponBolt`: combined main-bolt rotation and translation.

These are one coordinated mechanism but not one rigid transform. The live clip drives all three at the same stage time. Their helpers/meshes inherit and must not be driven separately.

Exact physical grip identities:

| Source | OMOD | Role |
|---|---|---|
| `MCPR_3000_SEModelMesh_022:0` | `[Receiver] Standard`, `Ozzys_MCPR-300.esp:0x1F48` | Bolt handle |
| `MCPR_3000_SEModelMesh_007:0` | `[Receiver] Standard`, `Ozzys_MCPR-300.esp:0x1F48` | Bolt slide |
| `MCPR_3000_SEModelMesh_008:0` | Base/unpaired | Main bolt body |

### Magazine exchange

Independent animated drivers:

```text
WeaponMagazine  = animation driver for the physical magazine hierarchy
WeaponOptics2   = replacement/ammunition visual (name is semantically misleading)
WeaponExtra3    = magazine latch
```

`WeaponMagazine` owns these inherited descendants:

```text
WeaponMagazineHelper
  WeaponMagazineChild2 -> WeaponMagazineChild2:150
  WeaponMagazineChild3 -> WeaponMagazineChild3:154
  WeaponMagazineChild4 -> WeaponMagazineChild4:158
  WeaponMagazineChild5 -> WeaponMagazineChild5:162
  WeaponMagazineChild6 -> WeaponMagazineChild6:166
  WeaponMagazineChild7 -> WeaponMagazineChild7:170
P-Mag (connection subnode inside the magazine NIF, not the whole part)
  WeaponMagazineChild1
    WeaponMagazineChild1Helper
      MCPR_3000_SEModelMesh_021:0
  GM6_Magazine:13
  XMag_SEModelMesh_083:0
```

`WeaponOptics2` owns:

```text
WeaponOptics2Helper
  ammo_unspent_338nm_LOD0_SEModelMesh_001:0
  ammo_unspent_338nm_LOD0_SEModelMesh:0
  WeaponOptics2Helper:114
  WeaponOptics2Helper:117
  WeaponOptics2Helper:125
  WeaponOptics2Helper:126
  WeaponOptics2Helper:128
  WeaponOptics2Helper:129
```

`WeaponExtra3` owns:

```text
WeaponExtra3Helper -> MCPR_3000_SEModelMesh_003:0
```

The profile deliberately exposes only the physical magazine meshes as grip candidates. Ammunition visuals and the latch move with the group but cannot independently seize clip-time authority.

| Grip source | OMOD | Role |
|---|---|---|
| `MCPR_3000_SEModelMesh_021:0` | Base/unpaired | Magazine body |
| `GM6_Magazine:13` | Base/unpaired | Magazine variant |
| `XMag_SEModelMesh_083:0` | Base/unpaired | Magazine variant |

## Motion findings from the activated reload

| Driver | Maximum excursion | Authored moving intervals | Interpretation |
|---|---:|---|---|
| `WeaponExtra2` | 3.34 | `0.42–0.63`, `5.43–5.64` | Handle unlock/lock rotation |
| `WeaponExtra1` | 9.12 | `0.63–1.04`, `5.22–5.43` | Bolt rear/forward translation |
| `WeaponBolt` | 9.71 | `0.42–1.04`, `5.22–5.64` | Combined bolt rotation/translation |
| `WeaponExtra3` | 1.78 | `1.78–2.19`, `3.97–4.28` | Magazine latch |
| `WeaponMagazine` | 29.15 | `1.88–2.09`, `2.19–4.08` | Physical magazine exchange |
| `WeaponOptics2` | 33.08 | `1.88–2.09`, `2.19–4.08` | Replacement/ammunition visual |

`WeaponMagazineChild1..5`, `WeaponOptics1`, and `WeaponTrigger` have no meaningful authored local motion in this clip. Magazine descendants move through inheritance; the child/optics pair also participates in visibility staging.

Every animated driver returns to its initial clip pose by the end.

## Curated stages

The profile uses four contiguous interaction windows so no sound or visibility interval is skipped:

| Stage | Group | Window | Required physical result |
|---|---|---:|---|
| `bolt_open` | `bolt_action` | `0.000–1.041667` | Raise/preload, unlock handle, pull bolt fully rearward |
| `magazine_remove` | `magazine_exchange` | `1.041667–2.416667` | Operate latch, acquire magazine, extract to Magout |
| `magazine_insert` | `magazine_exchange` | `2.416667–4.541667` | Visual swap, insert, release latch, seat through Magslap |
| `bolt_close` | `bolt_action` | `4.541667–5.750000` | Push forward, lock handle, commit reload, play end contact |

The hand must reach a stage's end pose and release its current grip before PAPER activates the next stage's collider group.

Static/dwell portions remain inside the adjacent stage. The existing clip pursuit controller uses its pull-gated crawl until the selected mechanism starts responding; this preserves authored event spacing instead of jumping over quiet portions.

## Exact event table

| Time | Kind | Stored payload | Meaning |
|---:|---|---|---|
| 0.166667 | Sound | `Soundplay.WPNMCPR-300_XMags_Empty_Raise` | Raise/preload |
| 0.208333 | Visibility | `UnCullBone.WeaponMagazineChild1` | Show loaded magazine visual |
| 0.250000 | Visibility | `Cullbone.WeaponOptics2` | Hide replacement/ammunition visual |
| 0.458333 | Sound | `Soundplay.WPNMCPR-300_XMags_Empty_Boltback` | Bolt unlock/back contact |
| 1.375000 | Sound | `Soundplay.WPNMCPR-300_XMags_Empty_Grab` | Weapon-hand contact |
| 1.875000 | Sound | `Soundplay.WPNMCPR-300_XMags_Empty_Maggrab` | Magazine acquisition |
| 2.416667 | Sound | `Soundplay.WPNMCPR-300_XMags_Empty_Magout` | Magazine clears weapon |
| 3.250000 | Visibility | `Uncullbone.WeaponOptics2` | Reveal replacement/ammunition visual |
| 3.291667 | Visibility | `Cullbone.WeaponMagazineChild1` | Hide removed magazine visual |
| 3.333333 | Sound | `Soundplay.WPNMCPR-300_XMags_Empty_Rattle` | Replacement handling |
| 3.666667 | Sound | `Soundplay.WPNMCPR-300_XMags_Empty_Magin` | Magazine enters well |
| 4.541667 | Sound | `Soundplay.WPNMCPR-300_XMags_Empty_Magslap` | Magazine seated |
| 5.291667 | Sound | `Soundplay.WPNMCPR-300_XMags_Empty_Boltclose` | Bolt closing/locking |
| 5.708333 | Gameplay | `reloadComplete` | Captured event-ID-56 ammo-commit boundary |
| 5.750000 | Sound | `Soundplay.WPNMCPR-300_XMags_Empty_End` | End contact |

The profile therefore replays ten concrete sound payloads, four visibility payloads, and one gameplay commit payload.

The generic trigger names (`SoundPlay`, `CullBone`, `UncullBone`) are not replayed separately; their annotation payloads above carry the necessary concrete sound/node data. This avoids duplicate events.

## Implemented runtime behavior

- The profile binds only when every declared driver, follower, grip source, and OMOD matches the current weapon generation.
- Exact driver/follower binding forces a one-time full named-subtree inventory for this curated weapon, independently of the mapper observation toggle.
- Binding requires each declared identity to resolve exactly once. Ozzy's only duplicate name is the unused `P-Grip`, so every profile identity is unambiguous; a later weapon with duplicate declared names will fail closed until its profile format carries a path-qualified identity.
- A successful bind logs that the profile supersedes `sMotionPathMode`.
- The matching `WPNReload` clone is captured even if the INI is set to Learned/Hybrid/Authored.
- Full and cropped clip duration must both match `6.583333` within `0.05` seconds, with no meaningful crop start.
- The current stage clamps the pursuit controller's fraction.
- Only that stage's group receives ROCK AttachOnly targets.
- Events replay once when engine-observed time crosses them.
- Backward scrubbing does not replay a fired event.
- The final release occurs at `5.75` seconds; the engine completes the remaining native tail.
- Rich capture remains available, but learner/harvest winners cannot serve while this authoritative profile is present. If exact binding fails, PAPER installs no targets and leaves the native reload untouched instead of falling back to an old mode.

## Validation completed

- Actual bundled JSON parsed by policy test.
- Serialize/parse round-trip preserved groups, OMODs, stages, events, drivers, and followers.
- Four stage ends held until grip release.
- All fifteen events emitted exactly once.
- Duration mismatch released fail-closed.
- Authoritative data without `curated: true` was rejected.
- Partial/empty OMOD identities were rejected.
- `P-*` connector subnodes were rejected as physical grip sources.
- Full `custom-fast` FO4VR build succeeded.
- DLL/PDB and bundled profile auto-deployed.
- Source, bundled deployment, and live Documents profile SHA-256 matched after deployment.

Active local test configuration at deployment was `bEnabled=true`, `bMotionLibrary=true`, `sMotionPathMode=learned`, `bRequireTriggerUnlock=false`, `bClipScrubSweepTest=false`, and `bResetLearnedPaths=false`. No INI edit was required: the profile deliberately overrides `learned` for Ozzy, and the current trigger setting means the listed parts can be grabbed directly during their stage.

## First in-game test checklist

1. Equip the Ozzy MCPR-300 and start a normal reload.
2. Confirm the log says `Authoritative reload profile bound` and then `Authoritative reload session ... started`.
3. Confirm only bolt pieces accept trigger-unlocked AttachOnly manipulation during `bolt_open`.
4. Pull to the rear stop, release, and confirm the log advances to `magazine_remove`.
5. Confirm magazine meshes—not ammunition/helper colliders—control the magazine stages.
6. Remove and release at Magout; then insert and release at Magslap.
7. Confirm the bolt becomes eligible again only for `bolt_close`.
8. Confirm each sound occurs once near its physical contact.
9. Confirm the two cull/uncull swaps do not leave duplicate or missing magazine visuals.
10. Confirm `reloadComplete` updates ammunition and native completion does not double-commit.
11. Confirm cancel/weapon-switch restores the clip and targets cleanly.
12. Save `PAPERRedux.log` if any stage stalls; the stage/window/event lines identify the precise failure boundary.

## Known remaining risk

The captured event payloads are locally authoritative, and PAPER now dispatches them through the player's standard animation-graph notification path. This code path builds and is lifecycle-safe, but its handling of payload-bearing `Soundplay.*` and `CullBone.*` events during a user-controlled clip has not yet been observed in game. The first Ozzy run determines whether all payload events are accepted directly or whether a later, separately verified engine dispatch path is required.
