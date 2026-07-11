# Authoritative reload profiles (motion-library format 2)

Date: 2026-07-11
Project: PAPER Redux
Status: implemented; Ozzy MCPR-300 is the first in-game testbed
Confidence: high for the format/runtime contract; in-game interaction and graph-event replay still require the first Ozzy test

## Purpose

Format 2 adds an optional `authoritativeReload` profile to a curated per-weapon motion-library file. Its presence changes the file from a collection of competing learned/authored paths into the behavioral authority for that weapon.

The profile supersedes `sMotionPathMode` for its weapon only. Other weapons continue using the existing Hybrid, Authored, Learned, or ClipScrub behavior.

The profile does not contain reduced or reconstructed transform curves. It captures and freezes the verified live reload clip in Havok user-controlled mode, lets the engine pose the complete weapon rig, and supplies PAPER with:

- exact ROCK collider identities that may manipulate each mechanism;
- explicit groups of independently animated driver nodes;
- explicit scene-graph followers that must inherit instead of receiving duplicate drives;
- contiguous physical interaction stages;
- exact captured animation-event payloads and their authored times;
- the boundary where clip control returns to the engine.

`P-*` names identify connection subnodes. A NIF part may contain one, but the
connector is not the mesh, collider, whole part, or a proxy for the whole
part. It may remain in the inherited hierarchy inventory when it carries
descendants; the format rejects it as a physical grip source, and grouping
must be derived from the complete driver/mesh hierarchy around it.

This separation is deliberate. The live clip remains the transform authority, while the curated file owns interpretation and interaction policy.

## Source authority and verification

Implementation authority:

1. Current PAPER Redux source on `develop`.
2. The append-only per-weapon capture archive.
3. Current ROCK V1 evidence/grip/target APIs already mirrored in PAPER.
4. Existing locally verified `hkbClipGenerator` user-controlled-time interface documented in `docs/research/2026-07-05-clip-scrub-hkbClipGenerator-verified-offsets.md`.

No web source, HIGGS source, Ghidra operation, FO4 Mods extraction, or ROCK modification was used for this implementation.

## Disk resolution

The writable/user library remains:

```text
Documents\My Games\Fallout4VR\PAPERRedux_Config\MotionLibrary
```

PAPER now also checks the read-only bundled location when no user file exists:

```text
Data\F4SE\Plugins\PAPERReduxMotionLibrary
```

Resolution is deterministic:

1. Collision-safe user filename.
2. Legacy sanitized user filename.
3. Bundled profile.

An existing user file is never silently bypassed when malformed. It is the explicit override and must either parse or fail closed.

The `custom-fast` post-build deployment copies bundled profiles beside the DLL under `F4SE\Plugins\PAPERReduxMotionLibrary`.

## Format contract

An authoritative file must have:

```json
{
  "format": 2,
  "curated": true,
  "parts": [],
  "authoritativeReload": {
    "profileVersion": 1,
    "archetype": "...",
    "clip": {},
    "groups": [],
    "stages": [],
    "events": []
  }
}
```

`curated: true` is mandatory. This prevents learner persistence and re-record wipes from replacing the profile.

### Clip identity

The `clip` object defines:

- `nameContains`: case-insensitive activated-clip filter;
- `durationSeconds`: expected full and cropped duration;
- `durationToleranceSeconds`: accepted difference, capped at one second;
- `releaseSeconds`: final curated interaction/event boundary.

The runtime rejects and releases a captured clip when its name, duration, crop start, or cropped duration does not match. A wrong clone therefore continues as a native reload instead of being controlled by the wrong profile.

### Groups

Each group has:

- a stable `id` and semantic `role`;
- exact `grips`, keyed by ROCK source name and optional plugin/local OMOD identity;
- independent animation `drivers`;
- `inheritedFollowers` for every descendant that must ride a driver through scene-graph inheritance.

The runtime validates the complete driver/follower inventory against the current weapon generation. It also resolves each grip to a body ID and enforces its OMOD identity. A missing node, changed workbench component, missing OMOD, or unbound group rejects the profile for that generation.

Only a group's declared grip bodies are installed as ROCK AttachOnly targets. INI class allowlists and learner path existence do not participate while an authoritative profile is bound.

### Stages

Profile-version 1 stages must:

- start at `0.0`;
- be strictly ordered;
- be contiguous within one millisecond;
- have positive duration;
- reference an existing group;
- end exactly at `releaseSeconds`.

Contiguity is a fidelity invariant. Jumping over a gap would collapse authored sound spacing and visibility changes into one frame.

The current stage's clip fraction is a hard sandbox window. A hand cannot scrub into another mechanism's stage. At the stage end, time is held until the manipulating grip is released; only then does the next group's whitelist become active.

### Events

Events are ordered, one-shot-per-session animation-graph payloads with one of three semantic kinds:

- `sound`;
- `visibility`;
- `gameplay`.

All kinds currently use the same safe engine path: `IAnimationGraphManagerHolder::NotifyAnimationGraphImpl` on the player. The kind exists for diagnostics and future policy, not for separate ad-hoc playback implementations.

Events fire only when the engine-observed clip time crosses their stored position in the forward direction. Scrubbing backward does not spam or replay already-fired contact sounds.

## Runtime sequence

1. Equip loads and parses the weapon file.
2. The complete current ROCK evidence/node cache is built.
3. Exact profile groups are generation-bound and validated.
4. PAPER arms the profile's clip filter regardless of `sMotionPathMode`.
5. The matching activated clip is captured and frozen at its seed fraction.
6. The pure controller validates clone identity/timing and starts stage zero.
7. Only the stage group's exact grip colliders become AttachOnly targets.
8. The existing pursuit controller maps physical hand displacement to clip time, clamped to the stage window.
9. Timeline events replay once at engine-observed crossings.
10. Reaching a stage end holds time until grip release.
11. The next stage/group becomes active.
12. At the final release boundary, PAPER requests graph-thread restoration of the clip's native mode; the engine finishes the remaining tail.

## Safety and failure behavior

- No engine pointer is retained by the format/controller.
- No file I/O or allocation occurs in the per-frame controller.
- Clip writes remain confined to the existing graph-thread hook.
- The main thread communicates desired fractions through the existing atomic channel.
- Stage/event state is reset on clip-session turnover, weapon change, shutdown, or profile removal.
- Profile duration/crop mismatch releases native behavior.
- Profile assembly mismatch prevents profile arming and installs no targets.
- Profile presence is authoritative even when binding fails: the old
  heuristic/INI modes are not used as a hidden fallback for that weapon.
- Profile binding inventories the full named weapon subtree even if the
  mapper-only `bFullSubtreeObservation` toggle is off; a curated runtime
  contract does not silently depend on that recording preference.
- Every declared name must resolve uniquely (grips also require their exact
  OMOD). Missing or ambiguous identities reject the profile; PAPER never
  guesses which duplicate node was intended.
- Provider target leases and ownership remain unchanged.
- ROCK API version remains 1 and ROCK source is untouched.

## Validation coverage

The policy test loads the actual bundled Ozzy JSON and verifies:

- format-2 parsing;
- curated/authoritative requirements;
- OMOD identity round-trip;
- group, stage, event, driver, and follower preservation;
- four contiguous stage transitions;
- stage-end grip-release gating;
- all fifteen events emitted exactly once;
- final native release;
- duration mismatch fails closed;
- authoritative data is rejected unless `curated` is true.
- partial, empty, or malformed OMOD identities fail closed.
- `P-*` connector subnodes are rejected as physical grips.

The full plugin is also built through the auto-deploying `custom-fast` preset. Runtime feel, event acceptance, sound playback, culling, and actual ammo commit remain in-game validation points.

## Adding another weapon

For each new weapon:

1. Preserve the fresh `.capture.jsonl` archive.
2. Select the activated reload clip, never an unnamed/fallback definition.
3. Confirm its exact duration, crop, markers, and moving tracks.
4. Build hierarchy-root groups; never group by motion timing alone.
5. Treat each `P-*` entry as a connector subnode, never as the whole NIF part.
6. Separate independent drivers from inherited descendants.
7. Choose only meaningful physical grip colliders and preserve their OMOD identities.
8. Make stages contiguous from zero through the release boundary.
9. Store exact annotation payloads, deduplicated against generic graph triggers.
10. Add the profile under `data/MotionLibrary`.
11. Extend the per-weapon research document and policy fixtures where the new archetype adds a new invariant.
