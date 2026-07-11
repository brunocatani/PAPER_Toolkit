# Curated learner movement-preview profiles (motion-library format 2)

Date: 2026-07-11

Project: PAPER_Redux

Source authority: current PAPER_Redux learner/runtime source, locally recorded per-weapon motion libraries, matching rich captures, and live in-game movement reports

Verification method: preserved-file hashing, library/capture inspection, generated-profile reproduction, engine-free parse/controller tests, `custom-fast`, and in-game movement/audio testing

Confidence: high for the file/controller contract and preservation of learner paths; each new weapon still requires live grip, grouping, transition, and sound validation

Affected systems: motion-library parsing, curated group binding, movement-preview projection, the PAPER-owned ROCK drive sandbox, and optional direct preview audio

## Purpose and boundary

`spatialReload` is the historical field name for a movement-only profile. It is not a reload implementation. It has no authority over native reload clips, animation time, ammo, visibility, graph events, or completion.

The valid runtime contract is now `learnerMovementPreview`. A curated profile may reorganize exact learner-recorded paths into explicit physical groups, stages, and sound gates, but it may not reconstruct movement from authored clip tracks or let controller rotation drive progress.

Weapons without a valid curated profile retain the normal learner and configured Hybrid, Authored, Learned, or ClipScrub behavior. A rejected profile fails closed and installs no curated drives.

## Why learner movement is the authority

The first Ozzy MCPR-300 test converted captured animation tracks into a separate full-hand-pose controller. Live testing falsified that approach: parts moved in incorrect directions and wrist rotation advanced mechanisms in place. The generated data was numerically self-consistent, but it did not preserve the already-correct learner interaction space.

F4NV-AMR provided a known-good control. Its current motion-library JSON moved in the exact directions and manner required. That file and capture were preserved byte-for-byte before enrichment. The new profile copies its existing `parts` values and uses those exact learned keys for both control geometry and driver tracks.

The durable invariant is:

```text
known-good learner path -> curated grouping/staging -> runtime drive
```

Authored clip tracks remain evidence for names, hierarchy, markers, and comparison. They are not silently promoted into a new movement basis.

## Coordinate contract

Every valid profile declares:

```json
"coordinateConventions": {
  "controlPaths": "recorded-learner-delta-pose",
  "driverTracks": "weapon-root-local",
  "quaternionOrder": "w,x,y,z",
  "pathDistance": "recorded-learner-pose-arc",
  "handProjection": "weapon-root-local-translation"
}
```

A pose is `[tx, ty, tz, qw, qx, qy, qz]`. Recorded path distance retains the learner metric:

```text
translation distance + quaternion angle in radians * 3 game units
```

The metric preserves the recorded curve and rotation sampling. Hand projection deliberately uses translation only, exactly like the proven learner scrub: current part position plus weapon-local hand displacement is projected onto nearby path segments. Stored quaternion keys still drive the mechanism's captured rotation. Wrist rotation alone cannot advance the path.

Projection is fixed-capacity, allocation-free, window-limited to nearby segments, and clamped to two path units per frame.

## Explicit groups and hierarchy

Each group separates:

- `grips`: concrete physical ROCK bodies, selected by source name and optional load-order-independent OMOD;
- `drivers`: independent top-level rig nodes PAPER drives;
- `inheritedFollowers`: descendants carried by those drivers through the scene graph;
- `connectorEvidence`: `P-*` nodes used only to verify hierarchy.

`P-*` always means a connector point. It is never the whole NIF part, a physical grip, or an independent driver. A physical mesh may be below a connector; that does not turn the connector into the mesh.

Driving top-level rig nodes avoids the doubled movement caused by independently driving both a parent and its descendant. Binding fails closed on missing/ambiguous identities, OMOD mismatch, incorrect ancestry, cross-group driver reuse, or a connector used as a grip/driver.

## Spatial cycles

Each group owns a closed stage cycle. There is no global reload sequence and no reload-state dependency.

- Bolt stages exchange at their maximum/open and minimum/closed physical endpoints, with bounded endpoint tolerance.
- The magazine removal stage changes at normalized path position `0.20`.
- The magazine insertion stage begins at normalized path position `0.80` and proceeds to its seated endpoint.
- At a `0.20 -> 0.80` handoff, each incoming driver is aligned to the exact outgoing pose. The correction fades geometrically to zero by the next gate, preventing a teleport while converging to the recorded incoming endpoint.
- Releasing ends drives and resets the selected group to its first/outgoing stage.

These are positions along recorded paths, not animation-time fractions.

## Mapped findings and sound

`mappedEvents` separates authority:

- `sound`: may request direct, edge-latched preview audio at a spatial/state gate;
- `visibility`: retained evidence only;
- `gameplay`: retained evidence only.

No mapped event is sent through `NotifyAnimationGraphImpl`. Visibility commands, `reloadComplete`, `reloadEnd`, and similar gameplay/graph markers remain inert.

For sound only, PAPER removes the captured `Soundplay.` command prefix, resolves the descriptor through `BSAudioManager::GetSoundHandleByName`, and starts it directly with zero-millisecond `FadeInPlay`. Failures are logged once per mapped event. This audio path cannot alter reload state.

## Fail-closed validation

The profile is rejected as a unit for:

- runtime mode other than `learnerMovementPreview`;
- authority other than `curatedRecordedMovement`;
- missing/incorrect coordinate conventions, especially translation-only hand projection;
- malformed poses, inconsistent arcs, or incomplete driver tracks;
- temporal/clip/session authority fields;
- invalid groups, cycles, fractions, OMODs, hierarchy, or event positions;
- `P-*` used as a grip or driver.

The old Ozzy `movementPreview` signature is intentionally rejected. This prevents the known-bad full-pose profile from remaining silently active.

## Generation and preservation

F4NV-AMR is regenerated only from the preserved known-good library plus its matching rich capture:

```powershell
python tools\generate_f4nv_amr_spatial_profile.py `<known-good.json>` `<capture.jsonl>` data\MotionLibrary\F4NV-AMR.esp_00000F99.json
```

The generator validates weapon identity and captured annotation inventory, retains all 23 original part records, and adds the curated profile. It does not use annotation time to drive movement.

Before any active-file replacement, preserve the original JSON and capture and record SHA-256 hashes. Runtime files are never the only copy of a known-good mapping again.

## Validation boundary

Policy tests prove schema rejection, exact key retention, translation-only input, group cycles, 20/80 magazine handoff continuity, sound-only output, and inert visibility/gameplay mappings. Builds prove compilation and deployment, not interaction feel.

Live validation must still confirm physical body binding, directions, grouping, endpoint feel, 20/80 exchange feel, and descriptor resolution. The correct failure mode is a rejected profile, no preview drive, a rate-limited sound failure, or restored sandbox ownership—never clip interception or gameplay mutation.
