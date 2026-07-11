#!/usr/bin/env python3
"""Generate Ozzy MCPR-300's curated physical movement-preview profile.

The capture's authored marker locations are consulted only offline to ask
"which recorded pose was the weapon in when this annotation was authored?".
The emitted runtime file contains no clocks, durations, clip fractions, or
temporal stage windows. Normalized values are spatial path positions only;
every moving driver, gate, and mapped finding resolves to physical path
distance and weapon-local pose.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any, Iterable


KEY_COUNT = 24
ROTATION_RADIUS = 3.0
ACTIVITY_ID = "173"
SOURCE_CLIP = r"Animations\44Pistol\WPNReload.hkt"
C = 0.900823
S = 0.434185
BASIS = (
    (0.0, 1.0, 0.0),
    (C, 0.0, S),
    (S, 0.0, -C),
)


def q_normalize(q: Iterable[float]) -> tuple[float, float, float, float]:
    q = tuple(float(v) for v in q)
    length = math.sqrt(sum(v * v for v in q))
    if length <= 1.0e-12:
        return (1.0, 0.0, 0.0, 0.0)
    return tuple(v / length for v in q)  # type: ignore[return-value]


def q_conjugate(q: Iterable[float]) -> tuple[float, float, float, float]:
    w, x, y, z = q_normalize(q)
    return (w, -x, -y, -z)


def q_multiply(a: Iterable[float], b: Iterable[float]) -> tuple[float, float, float, float]:
    aw, ax, ay, az = q_normalize(a)
    bw, bx, by, bz = q_normalize(b)
    return q_normalize(
        (
            aw * bw - ax * bx - ay * by - az * bz,
            aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
        )
    )


def q_nlerp(a: Iterable[float], b: Iterable[float], t: float) -> tuple[float, float, float, float]:
    a = q_normalize(a)
    b = q_normalize(b)
    if sum(x * y for x, y in zip(a, b)) < 0.0:
        b = tuple(-v for v in b)
    return q_normalize(tuple(x + (y - x) * t for x, y in zip(a, b)))


def q_angle(a: Iterable[float], b: Iterable[float]) -> float:
    dot = abs(sum(x * y for x, y in zip(q_normalize(a), q_normalize(b))))
    return 2.0 * math.acos(min(1.0, max(-1.0, dot)))


def mat_transpose(m: tuple[tuple[float, ...], ...]) -> tuple[tuple[float, ...], ...]:
    return tuple(tuple(m[column][row] for column in range(3)) for row in range(3))


def mat_multiply(
    a: tuple[tuple[float, ...], ...], b: tuple[tuple[float, ...], ...]
) -> tuple[tuple[float, ...], ...]:
    return tuple(
        tuple(sum(a[row][k] * b[k][column] for k in range(3)) for column in range(3))
        for row in range(3)
    )


def q_to_matrix(q: Iterable[float]) -> tuple[tuple[float, ...], ...]:
    w, x, y, z = q_normalize(q)
    return (
        (1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - w * z), 2.0 * (x * z + w * y)),
        (2.0 * (x * y + w * z), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - w * x)),
        (2.0 * (x * z - w * y), 2.0 * (y * z + w * x), 1.0 - 2.0 * (x * x + y * y)),
    )


def matrix_to_q(m: tuple[tuple[float, ...], ...]) -> tuple[float, float, float, float]:
    trace = m[0][0] + m[1][1] + m[2][2]
    if trace > 0.0:
        s = math.sqrt(trace + 1.0) * 2.0
        q = (0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s)
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0
        q = ((m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s)
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0
        q = ((m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s)
    else:
        s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0
        q = ((m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s)
    return q_normalize(q)


def pose_distance(a: list[float], b: list[float]) -> float:
    return math.dist(a[:3], b[:3]) + q_angle(a[3:7], b[3:7]) * ROTATION_RADIUS


def pose_lerp(a: list[float], b: list[float], t: float) -> list[float]:
    return [a[i] + (b[i] - a[i]) * t for i in range(3)] + list(q_nlerp(a[3:7], b[3:7], t))


def clean_pose(pose: Iterable[float]) -> list[float]:
    values = list(pose)
    values[3:7] = q_normalize(values[3:7])
    return [round(0.0 if abs(v) < 5.0e-10 else float(v), 9) for v in values]


def rig_delta_to_scene(delta: Iterable[float]) -> tuple[float, float, float]:
    dx, dy, dz = delta
    return (dy, C * dx + S * dz, S * dx - C * dz)


def scene_rotation_delta(first: list[float], current: list[float]) -> tuple[float, float, float, float]:
    rig_delta = mat_multiply(q_to_matrix(current[3:7]), mat_transpose(q_to_matrix(first[3:7])))
    scene_delta = mat_multiply(mat_multiply(BASIS, rig_delta), mat_transpose(BASIS))
    return matrix_to_q(scene_delta)


def convert_track(raw: list[list[float]], rest: list[float]) -> list[list[float]]:
    first = raw[0]
    converted: list[list[float]] = []
    for sample in raw:
        translation_delta = rig_delta_to_scene(sample[i] - first[i] for i in range(3))
        rotation_delta = scene_rotation_delta(first, sample)
        converted.append(
            [rest[i] + translation_delta[i] for i in range(3)]
            + list(q_multiply(rotation_delta, rest[3:7]))
        )
    return converted


def sample_pose(samples: list[list[float]], source_position: float) -> list[float]:
    source_position = min(float(len(samples) - 1), max(0.0, source_position))
    lower = int(math.floor(source_position))
    if lower + 1 >= len(samples):
        return list(samples[-1])
    return pose_lerp(samples[lower], samples[lower + 1], source_position - lower)


def cumulative_arc(samples: list[list[float]], start: int, end: int) -> list[float]:
    out = [0.0]
    for index in range(start + 1, end + 1):
        out.append(out[-1] + pose_distance(samples[index - 1], samples[index]))
    return out


def source_at_arc(cumulative: list[float], start: int, target: float) -> float:
    if target <= 0.0:
        return float(start)
    if target >= cumulative[-1]:
        return float(start + len(cumulative) - 1)
    for index in range(1, len(cumulative)):
        if target <= cumulative[index]:
            span = cumulative[index] - cumulative[index - 1]
            t = 1.0 if span <= 1.0e-9 else (target - cumulative[index - 1]) / span
            return float(start + index - 1) + t
    return float(start + len(cumulative) - 1)


def normalize_control_pose(pose: list[float], start_pose: list[float]) -> list[float]:
    return [pose[i] - start_pose[i] for i in range(3)] + list(
        q_multiply(pose[3:7], q_conjugate(start_pose[3:7]))
    )


def resample_control(
    samples: list[list[float]], start: int, end: int
) -> tuple[list[list[float]], float, list[float]]:
    cumulative = cumulative_arc(samples, start, end)
    raw_total = cumulative[-1]
    if raw_total <= 0.0:
        raise ValueError("control path has no physical travel")
    start_pose = samples[start]
    keys: list[list[float]] = []
    for key in range(KEY_COUNT):
        source = source_at_arc(cumulative, start, raw_total * key / (KEY_COUNT - 1))
        keys.append(normalize_control_pose(sample_pose(samples, source), start_pose))
    measured = sum(pose_distance(a, b) for a, b in zip(keys, keys[1:]))
    return keys, measured, cumulative


def monotonic_driver_distances(cumulative: list[float], declared_total: float) -> list[float]:
    """Spread zero-control-motion runs into adjacent physical intervals.

    This preserves latch/linkage poses without inventing a clock. A latch
    opening while the magazine has not yet translated is assigned the first
    small segment of the subsequent magazine path; an end latch closure is
    assigned the last physical seating segment.
    """

    raw_total = cumulative[-1]
    if raw_total <= 0.0:
        raise ValueError("cannot key a driver against a zero-length path")
    distances = [value / raw_total * declared_total for value in cumulative]
    count = len(distances)
    index = 0
    while index < count:
        run_end = index
        while run_end + 1 < count and abs(distances[run_end + 1] - distances[index]) <= 1.0e-8:
            run_end += 1
        if run_end > index:
            left = distances[index - 1] if index > 0 else distances[index]
            right = distances[run_end + 1] if run_end + 1 < count else distances[run_end]
            if index == 0 and run_end + 1 < count:
                left = 0.0
                for offset, item in enumerate(range(index, run_end + 1)):
                    distances[item] = left + (right - left) * offset / (run_end - index + 1)
            elif run_end == count - 1 and index > 0:
                right = declared_total
                for offset, item in enumerate(range(index, run_end + 1), 1):
                    distances[item] = left + (right - left) * offset / (run_end - index + 1)
            else:
                for offset, item in enumerate(range(index, run_end + 1), 1):
                    distances[item] = left + (right - left) * offset / (run_end - index + 2)
        index = run_end + 1

    distances[0] = 0.0
    distances[-1] = declared_total
    minimum_step = min(0.0005, declared_total / (count * 100.0))
    for index in range(1, count):
        distances[index] = max(distances[index], distances[index - 1] + minimum_step)
    if distances[-1] > declared_total:
        scale = declared_total / distances[-1]
        distances = [value * scale for value in distances]
    distances[0] = 0.0
    distances[-1] = declared_total
    return distances


def path_pose(keys: list[list[float]], arc_length: float, path_distance: float) -> list[float]:
    position = min(arc_length, max(0.0, path_distance)) / arc_length * (len(keys) - 1)
    lower = int(math.floor(position))
    if lower + 1 >= len(keys):
        return list(keys[-1])
    return pose_lerp(keys[lower], keys[lower + 1], position - lower)


def read_capture(path: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    snapshot: dict[str, Any] | None = None
    authored: dict[str, Any] | None = None
    with path.open("r", encoding="utf-8") as stream:
        for line in stream:
            event = json.loads(line)
            if snapshot is None and event.get("event") == "weaponSnapshot":
                snapshot = event
            if (
                event.get("event") == "authoredClip"
                and str(event.get("activityId")) == ACTIVITY_ID
                and event.get("animationName") == SOURCE_CLIP
            ):
                authored = event
            if snapshot is not None and authored is not None:
                break
    if snapshot is None or authored is None:
        raise ValueError("capture lacks the required snapshot or activated activity 173")
    return snapshot, authored


def build_profile(snapshot: dict[str, Any], authored: dict[str, Any]) -> dict[str, Any]:
    nodes = {node["name"]: node for node in snapshot["nodes"] if node["name"] not in {"P-Grip"}}
    raw_tracks = {track["bone"]: track["samples"] for track in authored["weaponTracks"]}
    moving_drivers = (
        "WeaponBolt",
        "WeaponExtra1",
        "WeaponExtra2",
        "WeaponExtra3",
        "WeaponMagazine",
        "WeaponOptics2",
    )
    converted: dict[str, list[list[float]]] = {}
    for name in moving_drivers:
        node = nodes.get(name)
        if node is None or not node.get("weaponLocalPoseValid") or name not in raw_tracks:
            raise ValueError(f"missing rest pose or authored track for {name}")
        converted[name] = convert_track(raw_tracks[name], node["weaponLocalPose"])

    bolt = converted["WeaponBolt"]
    bolt_delta = [pose_distance(bolt[0], sample) for sample in bolt]
    bolt_peak = max(bolt_delta)
    open_plateau = [i for i, value in enumerate(bolt_delta) if value >= bolt_peak - 0.06]
    bolt_open_end = max(open_plateau)
    bolt_close_end = next(
        i for i in range(bolt_open_end + 1, len(bolt)) if bolt_delta[i] <= 0.02
    )

    magazine = converted["WeaponMagazine"]
    magazine_delta = [pose_distance(magazine[0], sample) for sample in magazine]
    magazine_peak = max(range(len(magazine)), key=lambda i: magazine_delta[i])
    first_mag_motion = next(i for i in range(magazine_peak + 1) if magazine_delta[i] > 0.05)
    magazine_remove_start = max(0, first_mag_motion - 2)
    magazine_insert_end = next(
        i for i in range(magazine_peak + 1, len(magazine)) if magazine_delta[i] <= 0.02
    )
    # Include the latch's return-to-rest poses after the magazine itself seats.
    extra3 = converted["WeaponExtra3"]
    latch_rest = extra3[0]
    while (
        magazine_insert_end + 1 < len(extra3)
        and pose_distance(extra3[magazine_insert_end], latch_rest) > 0.02
    ):
        magazine_insert_end += 1
    while (
        magazine_insert_end + 1 < len(extra3)
        and pose_distance(extra3[magazine_insert_end], latch_rest) > 0.02
    ):
        magazine_insert_end += 1
    # The recorded latch closes at sample 41 even though magazine rest occurs at 39.
    magazine_insert_end = max(magazine_insert_end, 41)

    stage_specs = [
        ("bolt_open", "bolt_action", "WeaponBolt", 0, bolt_open_end, 0.0, 1.0,
         "bolt_close", 0.0, 0.0, 1.0, 0.35,
         ("WeaponExtra2", "WeaponExtra1", "WeaponBolt"),
         "Outgoing bolt movement: unlock and pull to the recorded rear endpoint; direction changes only inside endpoint tolerance."),
        ("magazine_remove", "magazine_exchange", "WeaponMagazine", magazine_remove_start, magazine_peak, 0.0, 0.20,
         "magazine_insert", 0.80, 0.0, 0.20, 0.75,
         ("WeaponMagazine", "WeaponOptics2", "WeaponExtra3"),
         "Outgoing magazine preview uses the first 20% of the full captured extraction path, then hands off to insertion at its 80% entry."),
        ("magazine_insert", "magazine_exchange", "WeaponMagazine", magazine_peak, magazine_insert_end, 0.80, 1.0,
         "magazine_remove", 0.0, 0.20, 0.0, 0.75,
         ("WeaponMagazine", "WeaponOptics2", "WeaponExtra3"),
         "Incoming magazine preview starts at 80% of the full captured insertion path and returns to the physical seated endpoint, then resets outgoing."),
        ("bolt_close", "bolt_action", "WeaponBolt", bolt_open_end, bolt_close_end, 0.0, 1.0,
         "bolt_open", 0.0, 1.0, 0.0, 0.35,
         ("WeaponExtra2", "WeaponExtra1", "WeaponBolt"),
         "Incoming bolt movement: push and lock at the recorded seated endpoint, then reset to the outgoing direction."),
    ]

    stages: list[dict[str, Any]] = []
    stage_data: dict[str, dict[str, Any]] = {}
    for (
        stage_id, group_id, control_name, start, end,
        entry_fraction, transition_fraction, next_stage,
        next_entry_fraction, outward_entry, outward_transition,
        endpoint_tolerance, drivers, notes,
    ) in stage_specs:
        control_keys, declared_arc, cumulative = resample_control(
            converted[control_name], start, end
        )
        distances = monotonic_driver_distances(cumulative, declared_arc)
        tracks = []
        for driver in drivers:
            keys = []
            for offset, source_index in enumerate(range(start, end + 1)):
                keys.append(
                    {
                        "pathDistance": round(distances[offset], 9),
                        "pose": clean_pose(converted[driver][source_index]),
                    }
                )
            tracks.append({"node": driver, "scale": 1.0, "keys": keys})
        stage = {
            "id": stage_id,
            "group": group_id,
            "entryFraction": entry_fraction,
            "transitionFraction": transition_fraction,
            "nextStage": next_stage,
            "nextStageEntryFraction": next_entry_fraction,
            "outwardFractionAtEntry": outward_entry,
            "outwardFractionAtTransition": outward_transition,
            "endpointTolerance": endpoint_tolerance,
            "controlPath": {
                "arcLength": round(declared_arc, 9),
                "keys": [clean_pose(key) for key in control_keys],
            },
            "driverTracks": tracks,
            "notes": notes,
        }
        stages.append(stage)
        stage_data[stage_id] = {
            "start": start,
            "end": end,
            "rawTotal": cumulative[-1],
            "cumulative": cumulative,
            "declaredArc": declared_arc,
            "controlKeys": control_keys,
        }

    observations = {item["sourceName"]: item for item in snapshot["observationTargets"]}

    def grip(source: str, role: str) -> dict[str, Any]:
        item = observations[source]
        encoded: dict[str, Any] = {"source": source, "role": role}
        ref = item.get("omod", {}).get("ref")
        if ref:
            encoded["omod"] = {"plugin": ref["plugin"], "id": ref["id"]}
            encoded["omodName"] = item["omod"].get("name", "")
        return encoded

    groups = [
        {
            "id": "bolt_action",
            "role": "unlock_pull_push_lock",
            "notes": "P-Bolt is connector evidence only. Three independent rig drivers are synchronized by one physical bolt path; their helper/mesh descendants inherit and are never independently driven.",
            "connectorEvidence": ["P-Bolt"],
            "grips": [
                grip("MCPR_3000_SEModelMesh_022:0", "bolt_handle"),
                grip("MCPR_3000_SEModelMesh_007:0", "bolt_slide"),
                grip("MCPR_3000_SEModelMesh_008:0", "bolt_body"),
            ],
            "drivers": [
                {"node": "WeaponExtra2", "role": "handle_unlock_lock_rotation", "inheritedFollowers": ["WeaponExtra2Helper", "MCPR_3000_SEModelMesh_022:0"]},
                {"node": "WeaponExtra1", "role": "bolt_linear_slide", "inheritedFollowers": ["WeaponExtra1Helper", "MCPR_3000_SEModelMesh_007:0"]},
                {"node": "WeaponBolt", "role": "main_bolt_combined_rotation_translation", "inheritedFollowers": ["WeaponBoltHelper", "MCPR_3000_SEModelMesh_008:0"]},
            ],
        },
        {
            "id": "magazine_exchange",
            "role": "latch_extract_visual_swap_insert_seat",
            "notes": "P-Mag is a connector subnode inside the physical magazine hierarchy, never the part or grip. WeaponMagazine, WeaponOptics2, and WeaponExtra3 retain independent position-keyed tracks.",
            "connectorEvidence": ["P-Mag"],
            "grips": [
                grip("MCPR_3000_SEModelMesh_021:0", "magazine_body"),
                grip("GM6_Magazine:13", "magazine_body_variant"),
                grip("XMag_SEModelMesh_083:0", "magazine_body_variant"),
            ],
            "drivers": [
                {"node": "WeaponMagazine", "role": "physical_magazine_animation_driver", "inheritedFollowers": ["WeaponMagazineHelper", "WeaponMagazineChild2", "WeaponMagazineChild2:150", "WeaponMagazineChild3", "WeaponMagazineChild3:154", "WeaponMagazineChild4", "WeaponMagazineChild4:158", "WeaponMagazineChild5", "WeaponMagazineChild5:162", "WeaponMagazineChild6", "WeaponMagazineChild6:166", "WeaponMagazineChild7", "WeaponMagazineChild7:170", "WeaponMagazineChild1", "WeaponMagazineChild1Helper", "MCPR_3000_SEModelMesh_021:0", "GM6_Magazine:13", "XMag_SEModelMesh_083:0"]},
                {"node": "WeaponOptics2", "role": "replacement_magazine_ammunition_visual", "inheritedFollowers": ["WeaponOptics2Helper", "ammo_unspent_338nm_LOD0_SEModelMesh_001:0", "ammo_unspent_338nm_LOD0_SEModelMesh:0", "WeaponOptics2Helper:114", "WeaponOptics2Helper:117", "WeaponOptics2Helper:125", "WeaponOptics2Helper:126", "WeaponOptics2Helper:128", "WeaponOptics2Helper:129"]},
                {"node": "WeaponExtra3", "role": "magazine_latch", "inheritedFollowers": ["WeaponExtra3Helper", "MCPR_3000_SEModelMesh_003:0"]},
            ],
        },
    ]

    annotations = {item["text"]: item for item in authored["annotations"]}

    def mapped_event(
        event_id: str,
        stage: str,
        trigger: str,
        kind: str,
        payload: str,
        notes: str = "",
    ) -> dict[str, Any]:
        encoded = {
            "id": event_id,
            "stage": stage,
            "trigger": trigger,
            "kind": kind,
            "sourceEvent": payload,
        }
        if trigger == "pathPosition":
            source_marker = annotations[payload]
            source_position = (
                float(source_marker["timeSeconds"])
                / float(authored["durationSeconds"])
                * (len(raw_tracks["WeaponBolt"]) - 1)
            )
            info = stage_data[stage]
            source_position = min(float(info["end"]), max(float(info["start"]), source_position))
            integer = int(math.floor(source_position))
            local = source_position - integer
            offset = integer - info["start"]
            raw_arc = info["cumulative"][offset]
            if local > 0.0 and integer < info["end"]:
                raw_arc += local * (
                    info["cumulative"][offset + 1] - info["cumulative"][offset]
                )
            path_distance = raw_arc / info["rawTotal"] * info["declaredArc"]
            encoded["pathFraction"] = round(
                path_distance / info["declaredArc"], 9
            )
            encoded["targetPose"] = clean_pose(
                path_pose(info["controlKeys"], info["declaredArc"], path_distance)
            )
        if notes:
            encoded["notes"] = notes
        return encoded

    mapped_events = [
        mapped_event("raise", "bolt_open", "stageEnter", "sound", "Soundplay.WPNMCPR-300_XMags_Empty_Raise"),
        mapped_event("show_loaded_magazine", "bolt_open", "stageEnter", "visibility", "UnCullBone.WeaponMagazineChild1"),
        mapped_event("hide_replacement_visual", "bolt_open", "stageEnter", "visibility", "Cullbone.WeaponOptics2"),
        mapped_event("bolt_back", "bolt_open", "pathPosition", "sound", "Soundplay.WPNMCPR-300_XMags_Empty_Boltback"),
        mapped_event("weapon_grab", "bolt_open", "stageComplete", "sound", "Soundplay.WPNMCPR-300_XMags_Empty_Grab"),
        mapped_event("magazine_grab", "magazine_remove", "gripStart", "sound", "Soundplay.WPNMCPR-300_XMags_Empty_Maggrab"),
        mapped_event("magazine_out", "magazine_remove", "pathPosition", "sound", "Soundplay.WPNMCPR-300_XMags_Empty_Magout"),
        mapped_event("show_replacement_visual", "magazine_insert", "pathPosition", "visibility", "Uncullbone.WeaponOptics2"),
        mapped_event("hide_removed_magazine", "magazine_insert", "pathPosition", "visibility", "Cullbone.WeaponMagazineChild1"),
        mapped_event("magazine_rattle", "magazine_insert", "pathPosition", "sound", "Soundplay.WPNMCPR-300_XMags_Empty_Rattle"),
        mapped_event("magazine_in", "magazine_insert", "pathPosition", "sound", "Soundplay.WPNMCPR-300_XMags_Empty_Magin"),
        mapped_event("magazine_slap", "magazine_insert", "stageComplete", "sound", "Soundplay.WPNMCPR-300_XMags_Empty_Magslap"),
        mapped_event("bolt_close", "bolt_close", "pathPosition", "sound", "Soundplay.WPNMCPR-300_XMags_Empty_Boltclose"),
        mapped_event("reload_complete", "bolt_close", "stageComplete", "gameplay", "ReloadComplete", "Captured ammo-commit annotation retained as inert metadata; movementPreview never dispatches it."),
        mapped_event("reload_end_contact", "bolt_close", "stageComplete", "sound", "Soundplay.WPNMCPR-300_XMags_Empty_End"),
        mapped_event("reload_end", "bolt_close", "stageComplete", "gameplay", "reloadEnd", "Captured graph-exit annotation retained as inert metadata; movementPreview never dispatches it."),
    ]

    profile = {
        "profileVersion": 1,
        "runtimeMode": "movementPreview",
        "authority": "curatedSpatialMovement",
        "coordinateConventions": {
            "controlPaths": "grip-relative-pose",
            "driverTracks": "weapon-root-local",
            "quaternionOrder": "w,x,y,z",
            "pathDistance": "translation-plus-rotation-at-3-game-unit-radius",
        },
        "archetype": "bolt_action_detachable_magazine",
        "sourceCapture": "Ozzys_MCPR-300.esp_000020A6.capture.jsonl",
        "sourceActivityId": ACTIVITY_ID,
        "sourceClip": SOURCE_CLIP,
        "notes": "Curated movement-only preview. Physical grips cycle each explicit interaction group independently; source clip and all mapped annotations are provenance, visibility/gameplay remain inert, and only mapped sounds may be played directly.",
        "groups": groups,
        "stages": stages,
        "mappedEvents": mapped_events,
    }
    return {
        "format": 2,
        "weapon": {"plugin": "Ozzys_MCPR-300.esp", "id": "0x000020A6"},
        "weaponName": snapshot["weapon"]["name"],
        "curated": True,
        "parts": [],
        "spatialReload": profile,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    snapshot, authored = read_capture(args.capture)
    document = build_profile(snapshot, authored)
    encoded = json.dumps(document, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
