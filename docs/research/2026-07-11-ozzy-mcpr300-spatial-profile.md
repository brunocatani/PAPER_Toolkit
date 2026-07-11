# Ozzy MCPR-300 movement-preview findings

Date: 2026-07-11

Project/repo: PAPER_Redux

Weapon: `Ozzys_MCPR-300.esp:0x000020A6` (`MCPR-300 - Watchdog 141`)

Capture: `Ozzys_MCPR-300.esp_000020A6.capture.jsonl`

Captured authored activity: `173`, `Animations\44Pistol\WPNReload.hkt`

Source used: local rich capture, current PAPER_Redux transform conversion and learner path metric, current ROCK provider V1 identities, and local CommonLibF4VR audio/address-library audit

Verification method: capture inventory and pose extraction, generator reproduction, schema round-trip/policy tests, synthetic full-pose traversal, local audio relocation audit, `custom-fast`, and focused in-game movement/audio checks

Confidence: high for identities, hierarchy, OMODs, recorded poses, group geometry, and annotation-to-pose mapping; live grip feel, exact installed assembly binding, alignment feel, and descriptor playback require in-game testing

Affected systems: Ozzy's curated motion-library entry, PAPER's group binding/controller, PAPER-owned ROCK drive sandbox, and optional direct preview audio

## Final outcome

The Ozzy file is a movement-only testbed. It provides explicit physical groups and recorded mechanism paths without becoming a reload.

- The native reload and every animation clip are completely untouched.
- `sourceClip` records where the evidence came from; it is never matched or intercepted.
- The original learner behavior remains available and unchanged for other weapons.
- Bolt and magazine groups cycle independently like learned primary/return paths.
- All driver motion uses captured translation and rotation.
- Ten sound annotations may play directly as preview audio at spatial gates.
- Four visibility annotations and two gameplay annotations remain inert findings.
- Nothing is sent to the animation graph; ammo and reload completion are never changed.
- No runtime timestamp, duration, clip fraction, or stage clock is present.

This intentionally permits more freedom than the source animation. The capture supplies precise geometry and evidence, not a mandatory sequence.

## Capture inventory

The weapon snapshot recorded 78 nodes and 77 observation targets: 34 body-backed identities and 43 observation-only hierarchy/rig identities. The only duplicate node name was `P-Grip`; none of the selected grips, drivers, or connector evidence is ambiguous.

Activity `173` supplied 13 weapon tracks. Six moved materially:

| Track | Recorded role | Maximum motion |
|---|---|---:|
| `WeaponBolt` | combined bolt rotation and translation | ~9.12 translation, ~1.115 rad rotation |
| `WeaponExtra1` | linear bolt slide | ~9.12 translation |
| `WeaponExtra2` | bolt-handle unlock/lock | ~1.115 rad rotation, nearly zero translation |
| `WeaponMagazine` | physical magazine path | ~28.59 translation, ~1.921 rad rotation |
| `WeaponOptics2` | replacement magazine/ammunition visual | ~32.57 translation, ~1.921 rad rotation |
| `WeaponExtra3` | magazine latch | ~0.592 rad rotation |

`WeaponMagazineChild1..5`, `WeaponOptics1`, and `WeaponTrigger` were effectively stationary tracks. They remain hierarchy/visibility descendants rather than independent motion drivers.

## Connector rule

`P-*` names are attachment/connect-point subnodes, not complete NIF parts.

- `P-Bolt` is the parent connector for three driver branches. It is not the handle, slide, bolt body, or a collider.
- `P-Mag` is a connector inside the `WeaponMagazine` descendant hierarchy. It carries physical magazine meshes but is not itself the magazine.

The profile stores both only as `connectorEvidence`. They can never be offered as grips or driven as mechanism nodes.

## Bolt group

Group id: `bolt_action`

Physical grips:

| Source | Role | OMOD |
|---|---|---|
| `MCPR_3000_SEModelMesh_022:0` | bolt handle | `Ozzys_MCPR-300.esp:0x00001F48`, `[Receiver] Standard` |
| `MCPR_3000_SEModelMesh_007:0` | bolt slide | `Ozzys_MCPR-300.esp:0x00001F48`, `[Receiver] Standard` |
| `MCPR_3000_SEModelMesh_008:0` | bolt body | base/unpaired in the capture |

Independent drivers:

- `WeaponExtra2`, carrying `WeaponExtra2Helper` and the handle mesh;
- `WeaponExtra1`, carrying `WeaponExtra1Helper` and the slide mesh;
- `WeaponBolt`, carrying `WeaponBoltHelper` and the main bolt mesh.

`WeaponBolt` provides the combined physical control shape. Grabbing any curated bolt mesh moves all three drivers; `P-Bolt` is never treated as the part.

### Bolt geometry and cycling

The captured outgoing/open path is 12.465 physical units. It begins with the rotation-only unlock and then the approximately 9.08-unit rearward pull. Full quaternion projection makes the unlock reachable by wrist rotation.

At the maximum/open endpoint, a 0.35-unit `endpointTolerance` snaps only the residual error to the exact captured pose and switches the bolt group to its return/close stage. The captured close path is 12.463 physical units. At its minimum/closed endpoint, the same tolerance latches the exact closed pose and switches the group back to outgoing/open.

This cycle is local to the bolt. It neither waits for the magazine nor indicates a reload state.

## Magazine group

Group id: `magazine_exchange`

Physical grips:

- `MCPR_3000_SEModelMesh_021:0`;
- `GM6_Magazine:13`;
- `XMag_SEModelMesh_083:0`.

All three are base/unpaired identities in this capture. The misleadingly named `WeaponOptics2` is not a grip; it is a separately animated replacement/ammunition branch.

Independent drivers:

- `WeaponMagazine`: physical magazine hierarchy and magazine mesh descendants;
- `WeaponOptics2`: replacement/ammunition visual branch;
- `WeaponExtra3`: magazine latch and its mesh.

### Captured geometry

The full captured removal arc is 45.160 physical units. The full captured insertion arc is 48.353 physical units and includes the latch returning to rest after the magazine itself seats. The complete curves remain in the profile as enriched evidence and driver geometry.

The trajectory is a loop rather than a straight line. Removal and insertion therefore remain separate paths even though the curated preview uses only selected spatial intervals.

### Curated exchange cycle

The outgoing/remove stage starts seated at outward fraction `0`. It transitions after normalized `0.20` outward travel rather than requiring maximum captured excursion.

At that exchange point, the group changes to the insertion stage at normalized entry `0.80`. The user then follows the final insertion interval to seated/outward `0`. Seating switches the group back to the outgoing stage at its seated entry.

This `0.20 -> 0.80 -> seated` cycle is deliberately freer than a one-to-one animation replay. It gives a practical exchange point while retaining the captured mechanism motion on both sides.

### Alignment correction

The recorded absolute driver poses at removal `0.20` and insertion `0.80` are not guaranteed to coincide. On transition, each driver receives a rigid alignment correction that initially preserves the exact outgoing pose. The correction fades to identity as spatial progress approaches the insertion endpoint, so the mechanism does not teleport and still reaches the exact captured seated pose.

The fade is geometric. There is no elapsed-time blend.

### Latch preservation

`WeaponExtra3` changes while `WeaponMagazine` has little or no movement. A uniform driver grid would erase that linkage peak, so the profile retains explicit `(pathDistance, pose)` driver keys.

Removal evidence includes:

| Path distance | Captured state |
|---:|---|
| 0.000 | latch rest |
| 0.006 | latch opening |
| 1.479 | maximum captured latch rotation |
| 1.967 | latch returning |
| 1.990 | latch rest |

The insertion track reopens the latch near seating and returns it to rest at 48.353. Preview state is sampled from magazine path position, never elapsed animation time.

## Group-local transition summary

| Group stage | Full captured arc | Active transition | Next group stage |
|---|---:|---|---|
| bolt outgoing/open | 12.465 | maximum endpoint within 0.35 | bolt return/close at open entry |
| bolt return/close | 12.463 | minimum endpoint within 0.35 | bolt outgoing/open at seated entry |
| magazine outgoing/remove | 45.160 | normalized outward 0.20 | magazine insertion at entry 0.80 |
| magazine insertion | 48.353 | seated/outward 0 within 0.75 endpoint tolerance | magazine outgoing/remove at seated entry |

The two groups define independent cycles and there is no required bolt-before-magazine order. Runtime cycle state exists only for the continuous pinned grip; release resets the selected group to its outgoing stage.

## Enriched annotation findings

The authored annotations were aligned offline to captured full poses. Their original annotation time helped locate the evidence but is not serialized as runtime authority.

### Direct-audio preview candidates

Only these ten sound annotations may become executable preview output. PAPER strips `Soundplay.` and resolves the remainder directly through `BSAudioManager`; it never forwards the annotation to the animation graph.

| Finding | Recorded group/stage context | Captured physical gate | Full-path distance |
|---|---|---|---:|
| `Soundplay.WPNMCPR-300_XMags_Empty_Raise` | bolt outgoing/open | stage entry | — |
| `Soundplay.WPNMCPR-300_XMags_Empty_Boltback` | bolt outgoing/open | path position | 0.522 |
| `Soundplay.WPNMCPR-300_XMags_Empty_Grab` | bolt outgoing/open | open endpoint | — |
| `Soundplay.WPNMCPR-300_XMags_Empty_Maggrab` | magazine outgoing/remove | grip start | — |
| `Soundplay.WPNMCPR-300_XMags_Empty_Magout` | magazine outgoing/remove | path position | 3.304 |
| `Soundplay.WPNMCPR-300_XMags_Empty_Rattle` | captured insertion evidence | path position | 19.850 |
| `Soundplay.WPNMCPR-300_XMags_Empty_Magin` | magazine insertion | path position | 39.873 |
| `Soundplay.WPNMCPR-300_XMags_Empty_Magslap` | magazine insertion | seated endpoint | — |
| `Soundplay.WPNMCPR-300_XMags_Empty_Boltclose` | bolt return/close | path position | 1.270 |
| `Soundplay.WPNMCPR-300_XMags_Empty_End` | bolt return/close | closed endpoint | — |

Sound gates are one-shot within a group cycle and use crossing/hysteresis state so hand oscillation cannot spam them. A captured sound outside the curated active interval remains mapped evidence; the preview does not invent time or force-fire it.

### Inert findings

These six annotations remain named and pose-aligned, but they are never dispatched or applied:

| Finding | Kind | Captured context | Full-path distance |
|---|---|---|---:|
| `UnCullBone.WeaponMagazineChild1` | visibility | bolt-open stage entry | — |
| `Cullbone.WeaponOptics2` | visibility | bolt-open stage entry | — |
| `Uncullbone.WeaponOptics2` | visibility | insertion path | 14.786 |
| `Cullbone.WeaponMagazineChild1` | visibility | insertion path | 17.318 |
| `ReloadComplete` | gameplay | captured closed-bolt endpoint | — |
| `reloadEnd` | gameplay | captured closed-bolt endpoint | — |

They are research for a future reload design only. The controller cannot emit them, and the runtime must never call `NotifyAnimationGraphImpl` for them.

## Runtime behavior

1. The curated file loads and exact grip, OMOD, driver, follower, and connector evidence bind to the equipped weapon generation.
2. Without starting a reload, PAPER exposes the concrete bolt and magazine grips through its existing ROCK owner token.
3. A valid hand/trigger acquisition selects that grip's group-local current stage.
4. Full hand translation and rotation are projected onto the recorded path interval.
5. The group's named drivers are emitted through the existing PAPER drive sandbox.
6. At a curated spatial gate, the group switches to its linked return/next stage and applies a temporary geometric alignment correction when needed.
7. Eligible sound findings may play directly once; visibility and gameplay findings remain inert.
8. Release ends the preview lease and resets the selected group to its outgoing stage; weapon/profile replacement also resets ownership safely.

At no step does PAPER inspect or modify a live clip, notify the animation graph, change visibility, commit ammo, or complete a reload. ROCK source and provider API version remain unchanged.

## Validation evidence and remaining live check

Completed evidence for the underlying data and integration surface:

- local capture inventory, hierarchy/OMOD identity, moving-track extraction, full-pose geometry, and annotation spelling were inspected;
- pure-rotation projection was exercised by the earlier spatial policy test;
- local CommonLibF4VR and the VR address library confirm that the audio manager/handle calls needed for direct preview sound resolve at function boundaries.

Completed on the finalized movement-preview candidate:

- regenerate the profile and verify parse plus serialize/parse round trip;
- recursively prove temporal authority fields and the removed timed `authoritativeReload` object are rejected;
- prove `P-*` misuse, invalid OMOD identity, malformed cycles, and event-pose mismatch fail closed;
- synthetically traverse both independent bolt endpoints and the magazine `0.20 -> 0.80 -> seated` cycle;
- prove alignment starts continuous and converges to the exact captured endpoint by geometric progress;
- prove only sound-kind findings reach executable output and visibility/gameplay findings never do;
- policy suite passed `1/1`;
- `custom-fast` built and auto-deployed the FO4VR DLL/PDB and bundled profile;
- generator reproduction was byte-identical to the repository profile;
- repository, bundled deploy, and Documents override profiles had matching SHA-256 hashes.

An earlier timed/intercepting candidate built successfully, but that build is not validation of this finalized contract.

The remaining validation is the in-game movement-and-sound checklist below. It is intentionally not an ammo/reload-completion test.

## In-game movement-and-sound checklist

Do not start a reload for this test. The checklist validates preview movement and optional sounds only; ammo count and reload completion must not change.

1. Equip exact form `Ozzys_MCPR-300.esp:0x000020A6` with the captured receiver assembly.
2. Confirm the movement-preview profile binds with two explicit groups and six drivers while the weapon remains otherwise idle.
3. Grip/rotate/pull any curated bolt mesh. Confirm `WeaponExtra2`, `WeaponExtra1`, and `WeaponBolt` remain synchronized through rotation and translation.
4. Without releasing, reach the open maximum within tolerance and reverse the hand. Confirm the bolt group switches to its close path.
5. Close/lock to the minimum endpoint while maintaining the grip. Confirm the bolt group cycles back to open.
6. Stop partway, release, and re-grip. Confirm the engine restores its baseline and the new preview grip starts from the outgoing path without snapping.
7. Grip a curated magazine mesh and move outward. Confirm latch/linkage motion and the Maggrab/Magout sounds occur once at their physical gates.
8. Cross normalized outward `0.20`. Confirm the group changes to insertion at entry `0.80` without a visible driver teleport.
9. Move to seated/outward `0`. Confirm the alignment correction converges to the exact captured seated pose and eligible Magin/Magslap audio occurs once.
10. Re-grip the seated magazine. Confirm it is back on the outgoing/remove path.
11. Manipulate bolt and magazine in the opposite order. Confirm group selection is independent and every new grip starts from its outgoing path.
12. Confirm preview sounds do not repeat while hovering across a tolerance and do not trigger visibility changes.
13. Confirm no native reload starts, no clip freezes or changes speed, no ammo count changes, and no reload-complete behavior occurs.
14. Start a normal native reload only after finishing the preview test. Confirm it behaves as it did before this profile, independently of the movement-preview controller.

## Remaining risks

- The exact installed NIF/OMOD combination may fail the strict binding even though capture identity is internally consistent.
- The `0.20 -> 0.80` correction may be mathematically continuous but still feel unnatural; tune spatial gates or captured paths, never a clock.
- The physical grip body and visual driver can have local-frame error that only live hand testing exposes.
- Some captured sound descriptors may fail to resolve in the installed plugin. Failure must be silent except for a rate-limited diagnostic and must not fall back to graph events.
- Native reload audio can overlap preview audio if the user deliberately manipulates the preview while a normal engine reload is running. This is an audio/test-use conflict, not permission to intercept the reload.
- Visibility and gameplay evidence is deliberately incomplete for a future reload system. It must remain inert until a separately designed, sufficiently informed reload architecture exists.
