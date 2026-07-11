#!/usr/bin/env python3
"""Enrich the known-good F4NV-AMR learner library without changing its paths.

The input library remains the movement authority.  This generator copies its
``parts`` array verbatim as Python values and builds a curated movement-preview
profile from those exact 24-key learned paths.  It never derives movement from
clip time.  The rich capture supplies identity, hierarchy, OMOD, annotation,
and provenance evidence only.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any, Iterable


WEAPON_PLUGIN = "F4NV-AMR.esp"
WEAPON_ID = "0x00000F99"
MAG_OMOD = {"plugin": WEAPON_PLUGIN, "id": "0x00006B57"}
RELOAD_CLIP = r"Animations\44Pistol\WPNReload.hkt"


def load_capture(path: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    snapshot: dict[str, Any] | None = None
    reload_clip: dict[str, Any] | None = None
    with path.open("r", encoding="utf-8") as stream:
        for line in stream:
            record = json.loads(line)
            if record.get("event") == "weaponSnapshot" and snapshot is None:
                snapshot = record
            elif (
                record.get("event") == "authoredClip"
                and record.get("activatedClip") is True
                and record.get("animationName") == RELOAD_CLIP
            ):
                reload_clip = record
    if snapshot is None:
        raise ValueError("capture has no weaponSnapshot")
    if reload_clip is None:
        raise ValueError("capture has no activated F4NV-AMR reload clip")
    return snapshot, reload_clip


def part_index(library: dict[str, Any]) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for part in library.get("parts", []):
        source = part.get("source")
        if not isinstance(source, str) or not source or source in result:
            raise ValueError(f"invalid or duplicate part source: {source!r}")
        result[source] = part
    return result


def normalized_quat(values: Iterable[float]) -> list[float]:
    q = [float(value) for value in values]
    length = math.sqrt(sum(value * value for value in q))
    if length <= 1.0e-12:
        return [1.0, 0.0, 0.0, 0.0]
    return [value / length for value in q]


def conjugate(q: list[float]) -> list[float]:
    n = normalized_quat(q)
    return [n[0], -n[1], -n[2], -n[3]]


def multiply(a: list[float], b: list[float]) -> list[float]:
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return normalized_quat(
        [
            aw * bw - ax * bx - ay * by - az * bz,
            aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
        ]
    )


def quat_angle(a: list[float], b: list[float]) -> float:
    dot = abs(sum(x * y for x, y in zip(a, b)))
    return 2.0 * math.acos(min(1.0, dot))


def pose_distance(a: list[float], b: list[float]) -> float:
    translation = math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(3)))
    return translation + 3.0 * quat_angle(a[3:7], b[3:7])


def delta_control_path(stage: dict[str, Any]) -> dict[str, Any]:
    keys = stage.get("keys")
    if not isinstance(keys, list) or len(keys) != 24:
        raise ValueError("learner stage must contain exactly 24 keys")
    anchor = keys[0]
    anchor_inverse = conjugate([float(value) for value in anchor[3:7]])
    relative: list[list[float]] = []
    for key in keys:
        rotation = multiply([float(value) for value in key[3:7]], anchor_inverse)
        relative.append(
            [
                float(key[0]) - float(anchor[0]),
                float(key[1]) - float(anchor[1]),
                float(key[2]) - float(anchor[2]),
                *rotation,
            ]
        )
    arc = sum(pose_distance(relative[i - 1], relative[i]) for i in range(1, 24))
    return {"arcLength": arc, "keys": relative}


def stage(library_parts: dict[str, dict[str, Any]], source: str, name: str) -> dict[str, Any]:
    try:
        value = library_parts[source][name]
    except KeyError as error:
        raise ValueError(f"missing {source}.{name}") from error
    if len(value.get("keys", [])) != 24:
        raise ValueError(f"malformed {source}.{name}")
    return value


def driver_track(
    library_parts: dict[str, dict[str, Any]],
    node: str,
    source: str,
    stage_name: str,
    control_arc: float,
    follower_name: str | None = None,
) -> dict[str, Any]:
    source_stage = stage(library_parts, source, stage_name)
    scale = 1.0
    if follower_name is None:
        keys = source_stage["keys"]
    else:
        follower = next(
            (
                candidate
                for candidate in source_stage.get("followers", [])
                if candidate.get("bone") == follower_name
            ),
            None,
        )
        if follower is None:
            raise ValueError(
                f"missing follower {follower_name} in {source}.{stage_name}"
            )
        keys = follower["keys"]
        scale = float(follower.get("restScale", 1.0))
    return {
        "node": node,
        "scale": scale,
        "keys": [
            {
                "pathDistance": control_arc * index / 23.0,
                "pose": key,
            }
            for index, key in enumerate(keys)
        ],
    }


def pose_at_fraction(control_path: dict[str, Any], fraction: float) -> list[float]:
    keys = control_path["keys"]
    position = max(0.0, min(1.0, fraction)) * 23.0
    index = min(23, int(position))
    if index == 23:
        return keys[23]
    t = position - index
    a = keys[index]
    b = keys[index + 1]
    translation = [a[i] + (b[i] - a[i]) * t for i in range(3)]
    qa = normalized_quat(a[3:7])
    qb = normalized_quat(b[3:7])
    sign = -1.0 if sum(x * y for x, y in zip(qa, qb)) < 0.0 else 1.0
    rotation = normalized_quat([qa[i] + (sign * qb[i] - qa[i]) * t for i in range(4)])
    return [*translation, *rotation]


def make_stage(
    *,
    stage_id: str,
    group_id: str,
    control_source: str,
    control_stage: str,
    driver_sources: list[tuple[str, str, str, str | None]],
    library_parts: dict[str, dict[str, Any]],
    entry: float,
    transition: float,
    next_stage: str,
    next_entry: float,
    outward_entry: float,
    outward_transition: float,
    tolerance: float,
    notes: str,
) -> dict[str, Any]:
    control = delta_control_path(stage(library_parts, control_source, control_stage))
    arc = float(control["arcLength"])
    return {
        "id": stage_id,
        "group": group_id,
        "entryFraction": entry,
        "transitionFraction": transition,
        "nextStage": next_stage,
        "nextStageEntryFraction": next_entry,
        "outwardFractionAtEntry": outward_entry,
        "outwardFractionAtTransition": outward_transition,
        "endpointTolerance": tolerance,
        "controlPath": control,
        "driverTracks": [
            driver_track(
                library_parts,
                node,
                source,
                source_stage,
                arc,
                follower_name,
            )
            for node, source, source_stage, follower_name in driver_sources
        ],
        "notes": notes,
    }


def mapped_event(
    event_id: str,
    stage_id: str,
    trigger: str,
    kind: str,
    source_event: str,
    notes: str,
    stages: dict[str, dict[str, Any]],
    path_fraction: float | None = None,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "id": event_id,
        "stage": stage_id,
        "trigger": trigger,
        "kind": kind,
        "sourceEvent": source_event,
        "notes": notes,
    }
    if path_fraction is not None:
        result["pathFraction"] = path_fraction
        result["targetPose"] = pose_at_fraction(
            stages[stage_id]["controlPath"], path_fraction
        )
    return result


def verify_capture_identity(snapshot: dict[str, Any], reload_clip: dict[str, Any]) -> None:
    weapon = snapshot.get("weapon", {})
    ref = weapon.get("ref", {})
    if ref.get("plugin") != WEAPON_PLUGIN or ref.get("id") != WEAPON_ID:
        raise ValueError(f"unexpected captured weapon identity: {ref!r}")
    annotations = {entry.get("text") for entry in reload_clip.get("annotations", [])}
    required = {
        "SoundPlay.WPN1AMRstart",
        "SoundPlay.WPN1AMRboltopen",
        "SoundPlay.WPN1AMRmagrel",
        "SoundPlay.WPN1AMRmagout",
        "SoundPlay.WPN1AMRmagin",
        "SoundPlay.WPN1AMRboltclose",
        "SoundPlay.WPN1AMRend",
        "CullBone.WeaponMagazineChild1",
        "UnCullBone.WeaponMagazineChild1",
        "reloadComplete",
        "initiateStart",
    }
    missing = required - annotations
    if missing:
        raise ValueError(f"reload capture is missing annotations: {sorted(missing)}")


def build(library: dict[str, Any], snapshot: dict[str, Any], reload_clip: dict[str, Any]) -> dict[str, Any]:
    if library.get("weapon") != {"id": WEAPON_ID, "plugin": WEAPON_PLUGIN} and library.get(
        "weapon"
    ) != {"plugin": WEAPON_PLUGIN, "id": WEAPON_ID}:
        raise ValueError("input library is not F4NV-AMR 00000F99")
    verify_capture_identity(snapshot, reload_clip)
    parts = part_index(library)

    stages_list = [
        make_stage(
            stage_id="bolt_open",
            group_id="bolt_group",
            control_source="HuntingRifleBolt:0",
            control_stage="learnedReturn",
            driver_sources=[
                ("WeaponBolt", "HuntingRifleBolt:0", "learnedReturn", "WeaponBolt"),
                ("WeaponExtra2", "HuntingRifleBolt:0", "learnedReturn", "WeaponExtra2"),
                ("WeaponExtra3", "HuntingRifleBolt:0", "learnedReturn", "WeaponExtra3"),
            ],
            library_parts=parts,
            entry=0.0,
            transition=1.0,
            next_stage="bolt_close",
            next_entry=0.0,
            outward_entry=0.0,
            outward_transition=1.0,
            tolerance=0.35,
            notes="Exact learner-recorded opening paths. Transition occurs at the physical maximum endpoint.",
        ),
        make_stage(
            stage_id="bolt_close",
            group_id="bolt_group",
            control_source="HuntingRifleBolt:0",
            control_stage="learnedPrimary",
            driver_sources=[
                ("WeaponBolt", "HuntingRifleBolt:0", "learnedPrimary", "WeaponBolt"),
                ("WeaponExtra2", "WeaponExtra2", "learnedPrimary", None),
                ("WeaponExtra3", "WeaponExtra3", "learnedReturn", None),
            ],
            library_parts=parts,
            entry=0.0,
            transition=1.0,
            next_stage="bolt_open",
            next_entry=0.0,
            outward_entry=1.0,
            outward_transition=0.0,
            tolerance=0.35,
            notes="Exact learner-recorded closing paths. Transition occurs at the physical minimum endpoint.",
        ),
        make_stage(
            stage_id="magazine_remove",
            group_id="magazine_group",
            control_source="BSX",
            control_stage="learnedPrimary",
            driver_sources=[
                ("WeaponMagazine", "BSX", "learnedPrimary", "WeaponMagazine")
            ],
            library_parts=parts,
            entry=0.0,
            transition=0.20,
            next_stage="magazine_insert",
            next_entry=0.80,
            outward_entry=0.0,
            outward_transition=0.20,
            tolerance=0.75,
            notes="Exact learner-recorded removal path, active only through the requested 20 percent exchange gate.",
        ),
        make_stage(
            stage_id="magazine_insert",
            group_id="magazine_group",
            control_source="BSX",
            control_stage="learnedReturn",
            driver_sources=[
                ("WeaponMagazine", "BSX", "learnedReturn", "WeaponMagazine")
            ],
            library_parts=parts,
            entry=0.80,
            transition=1.0,
            next_stage="magazine_remove",
            next_entry=0.0,
            outward_entry=0.20,
            outward_transition=0.0,
            tolerance=0.75,
            notes="Exact learner-recorded insertion path from 80 percent to seated; the geometric correction prevents a handoff jump.",
        ),
    ]
    stages = {value["id"]: value for value in stages_list}

    profile = {
        "profileVersion": 1,
        "runtimeMode": "learnerMovementPreview",
        "authority": "curatedRecordedMovement",
        "coordinateConventions": {
            "controlPaths": "recorded-learner-delta-pose",
            "driverTracks": "weapon-root-local",
            "quaternionOrder": "w,x,y,z",
            "pathDistance": "recorded-learner-pose-arc",
            "handProjection": "weapon-root-local-translation",
        },
        "archetype": "anti-materiel-rifle-bolt-action-detachable-magazine",
        "sourceCapture": "F4NV-AMR.esp_00000F99.capture.jsonl",
        "sourceActivityId": str(reload_clip["activityId"]),
        "sourceClip": RELOAD_CLIP,
        "notes": "Movement-only testbed built from the exact known-good learner paths. It never owns a reload clip, ammo, visibility, or gameplay.",
        "groups": [
            {
                "id": "bolt_group",
                "role": "bolt_action",
                "notes": "The physical bolt mesh selects three independent top-level rig drivers. Descendant meshes ride those drivers; no connector is driven.",
                "connectorEvidence": [],
                "grips": [
                    {
                        "source": "HuntingRifleBolt:0",
                        "role": "physical_bolt_grip",
                    }
                ],
                "drivers": [
                    {
                        "node": "WeaponBolt",
                        "role": "main_bolt",
                        "inheritedFollowers": ["HuntingRifleBolt:0"],
                    },
                    {
                        "node": "WeaponExtra2",
                        "role": "firing_pin_linkage",
                        "inheritedFollowers": ["HuntingRifleFiringPin:0"],
                    },
                    {
                        "node": "WeaponExtra3",
                        "role": "rear_bolt_linkage",
                        "inheritedFollowers": ["HuntingRifleBackBolt:0"],
                    },
                ],
            },
            {
                "id": "magazine_group",
                "role": "detachable_magazine",
                "notes": "BSX is the physical OMOD-backed grip. WeaponMagazine is the sole top-level driver and carries the complete magazine subtree through P-Mag.",
                "connectorEvidence": ["P-Mag"],
                "grips": [
                    {
                        "source": "BSX",
                        "omod": MAG_OMOD,
                        "omodName": "F4NV-AMR magazine assembly",
                        "role": "physical_magazine_grip",
                    }
                ],
                "drivers": [
                    {
                        "node": "WeaponMagazine",
                        "role": "magazine_root",
                        "inheritedFollowers": [
                            "WeaponMagazineChild1",
                            "Object01",
                            "WeaponMagazineChild3",
                            "Object02",
                            "WeaponMagazineChild4",
                            "308MagSmalBullets:0",
                            "WeaponMagazineChild5",
                            "WeaponMagazineChild2",
                            "BSX",
                        ],
                    }
                ],
            },
        ],
        "stages": stages_list,
        "mappedEvents": [
            mapped_event("reload_start", "bolt_open", "gripStart", "sound", "Soundplay.WPN1AMRstart", "Direct preview audio on a physical bolt grip; no graph event is sent.", stages),
            mapped_event("bolt_open", "bolt_open", "stageComplete", "sound", "Soundplay.WPN1AMRboltopen", "Plays at the spatial open endpoint.", stages),
            mapped_event("mag_release", "magazine_remove", "pathPosition", "sound", "Soundplay.WPN1AMRmagrel", "Direct preview audio at a spatial position shortly after removal begins.", stages, 0.05),
            mapped_event("mag_out", "magazine_remove", "stageComplete", "sound", "Soundplay.WPN1AMRmagout", "Plays at the curated 20 percent exchange gate.", stages),
            mapped_event("mag_in", "magazine_insert", "pathPosition", "sound", "Soundplay.WPN1AMRmagin", "Plays at insertion position 0.90, spatially separated from the mag-out exchange sound.", stages, 0.90),
            mapped_event("bolt_close", "bolt_close", "pathPosition", "sound", "Soundplay.WPN1AMRboltclose", "Plays halfway along the recorded closing path, spatially separated from bolt-open and end.", stages, 0.50),
            mapped_event("reload_end", "bolt_close", "stageComplete", "sound", "Soundplay.WPN1AMRend", "Direct preview audio at the spatial closed endpoint.", stages),
            mapped_event("magazine_child_cull", "magazine_remove", "stageEnter", "visibility", "CullBone.WeaponMagazineChild1", "Captured visibility evidence only; never dispatched or applied.", stages),
            mapped_event("magazine_child_uncull", "magazine_insert", "stageEnter", "visibility", "UnCullBone.WeaponMagazineChild1", "Captured visibility evidence only; never dispatched or applied.", stages),
            mapped_event("reload_complete", "bolt_close", "stageComplete", "gameplay", "reloadComplete", "Captured gameplay evidence only; never dispatched.", stages),
            mapped_event("initiate_start", "bolt_close", "stageComplete", "gameplay", "initiateStart", "Captured graph evidence only; never dispatched.", stages),
            mapped_event("reload_graph_end", "bolt_close", "stageComplete", "gameplay", "reloadEnd", "Captured graph-exit evidence only; never dispatched.", stages),
        ],
    }

    output = dict(library)
    output["curated"] = True
    output["spatialReload"] = profile
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("library", type=Path, help="preserved known-good motion-library JSON")
    parser.add_argument("capture", type=Path, help="matching rich capture JSONL")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    with args.library.open("r", encoding="utf-8") as stream:
        library = json.load(stream)
    snapshot, reload_clip = load_capture(args.capture)
    enriched = build(library, snapshot, reload_clip)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(enriched, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
