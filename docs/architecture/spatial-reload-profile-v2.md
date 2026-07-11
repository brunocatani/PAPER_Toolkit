# Spatial movement-preview profiles (motion-library format 2)

Date: 2026-07-11

Project: PAPER_Redux

Source authority: current PAPER_Redux source, local ROCK provider V1 contract, and per-weapon rich captures

Verification method: capture/profile inspection, engine-free policy tests, parse/round-trip tests, local CommonLibF4VR relocation audit, `custom-fast`, and focused in-game movement/audio checks

Confidence: high for the file/controller contract and captured pose data; movement feel and direct sound-name resolution require in-game testing

Affected systems: motion-library parsing, curated group binding, the PAPER-owned ROCK drive sandbox, movement-preview state, and optional preview audio

## Purpose and boundary

`spatialReload` is a curated movement-preview surface. Despite the historical field name, it is not a reload implementation and has no authority over the native reload, animation graph, ammo, visibility, or gameplay.

For a weapon with a valid curated profile, PAPER can expose explicit physical grips and let the user move their recorded mechanisms. The native clip is never selected, matched, intercepted, frozen, sampled, advanced, or released. `sourceClip` records only which captured clip supplied the offline evidence. Starting a native reload remains a completely separate engine action.

Weapons without a curated movement-preview profile retain the original learner and configured Hybrid, Authored, Learned, or ClipScrub behavior. This profile does not create a replacement or fallback path for them.

The executable preview contract is spatial:

- a hand is projected onto a recorded translation-plus-rotation path;
- each physical group owns its own primary/return-style cycle;
- named rig drivers receive captured weapon-root-local poses;
- sound annotations may be previewed directly through the audio manager at spatial gates;
- visibility and gameplay annotations are retained only as inert research findings;
- no animation timestamp, clip duration, normalized clip fraction, or stage clock exists.

Spatial fractions are allowed. They describe positions along a recorded geometric path, never animation time.

## Coordinate contract

Every profile declares these conventions:

```json
"coordinateConventions": {
  "controlPaths": "grip-relative-pose",
  "driverTracks": "weapon-root-local",
  "quaternionOrder": "w,x,y,z",
  "pathDistance": "translation-plus-rotation-at-3-game-unit-radius"
}
```

A pose is `[tx, ty, tz, qw, qx, qy, qz]`. Path distance uses the learner's physical metric:

```text
translation distance + quaternion angle in radians * 3 game units
```

The rotation term is essential. A wrist can traverse a bolt-handle unlock even when the handle has almost no translation.

## Explicit groups and node identity

Each interaction group separates three kinds of identity:

- `grips`: concrete ROCK physical bodies, selected by source name and optional load-order-independent OMOD identity;
- `drivers`: independent animated rig nodes that PAPER drives;
- `connectorEvidence`: `P-*` hierarchy nodes used only to confirm the expected captured assembly.

`P-*` always means connection point. It is never a complete NIF part, grip, collider, mesh, or motion driver. A physical NIF part may contain a `P-*` connector, and that connector may parent descendants, but the connector is only one subnode of the assembly.

Each driver also lists `inheritedFollowers`. Those descendants move through scene-graph inheritance and are not driven independently. Binding fails closed on missing or ambiguous names, OMOD mismatches, connectors used as grips/drivers, physical nodes used as rig drivers, or conflicting driver ancestry.

## Group-local cycles

There is no global four-step reload sequence. During one continuous grip, the selected group cycles through the stages explicitly linked by `nextStage`.

Gripping the bolt affects only the bolt cycle. Gripping the magazine affects only the magazine cycle. Releasing ends the preview drive, lets the engine restore its baseline, and resets that group to its outgoing stage for the next grip. This matches the learner's grip-session lifetime and avoids a raw-target jump on re-grip.

Each stage declares:

- a full `controlPath` of grip-relative translation and rotation poses;
- `entryFraction` and `transitionFraction`, both geometric positions on that path;
- a linked `nextStage` and its explicit entry fraction;
- conceptual outward fractions for diagnostics (`0` seated/rest, `1` maximum outward);
- an `endpointTolerance` used only when the transition is a physical endpoint;
- a distance-keyed driver track for every driver in the group.

Interior transition gates are exact spatial crossings. `endpointTolerance` cannot make an interior gate fire early.

### Bolt cycle

The bolt outgoing stage runs to its maximum/open physical endpoint. Entering the maximum endpoint tolerance latches the exact captured open pose and switches that group to its return stage. The return stage runs to the minimum/closed endpoint; entering its tolerance latches the exact captured closed pose and switches the group back to outgoing.

This max/min switching is independent of reload state. It only changes which recorded bolt path is offered on the next manipulation.

### Magazine exchange cycle

A full maximum-excursion removal is not the desired preview exchange boundary. The magazine outgoing stage instead transitions after normalized `0.20` outward travel. At that physical exchange point the group switches to the insertion stage at entry fraction `0.80`.

The insertion stage then travels to its seated endpoint, conceptual outward fraction `0`. Once seated, the magazine group switches back to the outgoing stage at its seated/outward-`0` entry for the next interaction.

This is deliberately not a one-to-one replay of the captured animation. The recorded paths supply geometry and linkage poses; curated transition fractions define the freer interaction.

## Hand projection and retained pose

At grip start the controller records the full weapon-local hand and part poses and anchors the current stage at the group's retained position. Each frame it applies the hand's rigid translation-and-rotation delta to the part, then projects the desired pose onto nearby path segments.

Projection remains bounded and allocation-free:

- three neighboring segments on either side;
- nine fixed pose samples per segment;
- at most two physical arc units of progress per frame;
- full pose distance, including quaternion rotation;
- no file I/O, global scans, heap growth, or graph calls in the frame path.

Releasing stops the preview lease and resets the selected group to its outgoing/rest stage. A transition during a continuous grip selects the linked stage immediately; it does not wait for or signal an animation clip.

## Driver alignment at a curated handoff

Curated gates such as magazine `0.20 -> 0.80` can connect two recorded tracks whose absolute driver poses do not coincide. Teleporting to the new track would be visible, while permanently offsetting it would lose the captured endpoint.

At a handoff, PAPER computes the rigid correction from the incoming track's entry pose to the exact outgoing driver pose. That correction starts fully applied and fades to identity across the new stage's active interval. The handoff is continuous, and the driver converges back to the exact captured track pose by its next transition.

The correction is group/driver state, not a time blend. Its weight is derived only from geometric progress between spatial gates.

## Driver lifetime and ownership

The existing `WeaponPartDriveSandbox` remains the sole ROCK consumer and drive owner. Preview driver output enters that owner as fixed-capacity source-name drives, preserving target registration, two-frame leases, provider reset handling, cleanup, and untrusted-observation marking.

Only the group selected by the pinned physical grip emits driver poses. A same-grip stage transition preserves the outgoing pose and continues under the existing ROCK lease. Releasing stops preview output; the sandbox lease then restores engine ownership and the next grip starts from that group's outgoing baseline. Weapon/profile replacement follows the same deterministic cleanup path.

## Mapped findings and preview sound

`mappedEvents` preserves the capture's annotation-to-pose alignment. It is intentionally split by authority:

- `sound`: may be emitted as one-shot preview audio at a spatial/state gate;
- `visibility`: inert metadata only;
- `gameplay`: inert metadata only.

No mapped event is sent to `NotifyAnimationGraphImpl`. In particular, `ReloadComplete`, `reloadEnd`, `Cullbone.*`, and `UnCullBone.*` are never dispatched. The movement controller exposes only eligible sound indices.

Captured sound payloads have the form `Soundplay.<descriptor>`. Preview audio strips the `Soundplay.` command prefix and passes only the descriptor name to `BSAudioManager::GetSoundHandleByName`, then starts the returned `BSSoundHandle` with zero-millisecond `FadeInPlay`. Playback is main-thread, edge-latched, and rate-limited on failure. It cannot change reload or graph state.

The Ozzy capture retains ten sound annotations and six inert visibility/gameplay annotations. A sound outside a stage's curated active interval remains useful evidence but is not synthesized through time or fired merely because it existed in the source clip.

## Fail-closed validation

The profile is rejected as a unit when any executable invariant fails, including:

- wrong format/profile version or runtime mode other than `movementPreview`;
- malformed/non-unit/NaN poses or inconsistent path arcs;
- any temporal authority field such as `timeSeconds`, `durationSeconds`, clip fraction, or stage clock;
- missing or duplicate groups, stages, grips, connectors, drivers, tracks, or mapped-event identities;
- `P-*` used as a grip or driver;
- invalid OMOD identity or exact runtime hierarchy binding failure;
- driver keys that do not cover the full physical path in strict order;
- invalid group-local cycle links, fractions, outward mapping, or endpoint tolerance;
- a mapped path pose that disagrees with its declared geometric position.

An invalid profile is not partially executed. Its failure does not alter the learner implementation used by other weapons.

## Generation and provenance

The Ozzy fixture is regenerated from the local capture with:

```powershell
python tools\generate_ozzy_spatial_profile.py `<capture.jsonl>` --output data\MotionLibrary\Ozzys_MCPR-300.esp_000020A6.json
```

The generator may use source annotation timestamps offline to locate the corresponding recorded poses. It serializes geometric positions and full poses, not those timestamps. `sourceCapture`, `sourceActivityId`, and `sourceClip` remain provenance only.

## Risks and validation boundary

Policy tests validate paths, cycles, handoff continuity, event filtering, and absence of temporal/graph authority. On 2026-07-11 the finalized candidate passed the policy suite (`1/1`), regenerated the Ozzy JSON byte-for-byte from the capture, and completed the auto-deploying `custom-fast` FO4VR build. The repository, bundled deploy, and Documents override JSON copies had the same SHA-256.

Those checks cannot prove interaction feel, exact body binding in the live NIF, or whether every captured sound descriptor resolves in the installed plugin. Those still require the movement-and-sound test in game. Build success from the earlier timed/intercepting candidate is not treated as evidence for this contract.

The correct failure mode is a rejected profile, a silent/rate-limited sound failure, or restored sandbox ownership. It must never be clip interception, graph notification, ammo mutation, or a hidden fallback reload implementation.
