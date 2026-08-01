# PAPER Redux

FO4VR F4SE plugin: physics-interactive weapon-part reloads. Grabbed weapon
parts (bolts, slides, magazines, shells) move along their real authored
animation paths.

**Requires ROCK.** PAPER Redux is a ROCK provider-API consumer: ROCK owns
grabbing, weapon collision evidence, grip capture, and part-drive application;
this plugin owns the reload intelligence on top of it:

- **Clip harvest** — walks the equipped weapon's animation-graph binding sets
  and hooks clip activation to extract baked per-bone motion tracks (bolt
  pull, mag drop) directly from the weapon's own animation clips.
- **Motion learner** — passively records every weapon part's engine-driven
  motion at runtime; learned paths outrank harvested ones, harvested
  activated-clip strokes outrank loaded-clip fallbacks.
- **Part-drive scrub** — while a hand holds an AttachOnly part grip, hand
  displacement scrubs the part along its motion path (followers ride along),
  driven through ROCK's `setWeaponPartDriveTargetsV1`.

## Build

CMake + VS2022 + vcpkg, same structure as ROCK:

```bat
cmake --preset custom-fast && cmake --build build-fast --config Release --target PAPERRedux -- /m:1 /p:CL_MPCount=2
```

`custom-fast` auto-deploys `PAPERRedux.dll`/`.pdb` to the configured mod root
(see `CMakeUserPresets.json`). Tests:

```bat
cmake --preset custom-tests && cmake --build build-tests --config Release && ctest --test-dir build-tests -C Release --output-on-failure
```

## Config

Optional `Data\F4SE\Plugins\PAPERRedux.ini`; `bEnabled` (default `true`) is
the master switch. Missing file keeps defaults.
