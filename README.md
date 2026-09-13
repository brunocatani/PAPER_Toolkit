# PAPER Toolkit

> **Personal experimental development tool. Not recommended for use.**
>
> This is my own workspace for experimenting with weapon motion and animation in Fallout 4 VR. It needs major changes, may break at any time, and may not be useful to anyone else.

The code explores motion capture, weapon-part interaction through ROCK, and an in-game animation inspection panel. Features and workflows are unfinished and subject to change. **Avoid using this in a normal playthrough.**

For the reload animation mod, see [PAPER on Nexus Mods](https://www.nexusmods.com/fallout4/mods/108883) or [PAPER on GitHub](https://github.com/brunocatani/PAPER).

## Development notes

- Plugin and build target: `PAPER_Toolkit` (`PAPER_Toolkit.dll`).
- Requires ROCK; the inspection panel also uses PAPER and Prisma UI for Fallout 4 VR.
- Builds depend on the surrounding development workspace. See [CMakeLists.txt](CMakeLists.txt) and the [preset template](CMakeUserPresets.json.template) for the required libraries and SDK paths.
- The [configuration reference](data/config/PAPER_Toolkit.ini) documents the current options. The active file is under `Documents/My Games/Fallout4VR/Mods_Config/PAPER_Toolkit/PAPER_Toolkit.ini`.
