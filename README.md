# Pokémon Emerald

This is a PC port of Pokémon Emerald, based on the [pret decompilation](https://github.com/pret/pokeemerald).
The PC port was originally made by [NTx86](https://github.com/NTx86/pokeemerald-sdl2pc).


Supported operating systems: Windows, Linux and macOS


To set up the repository, see [INSTALL_PC.md](INSTALL_PC.md).

## Playing

| GBA | Keyboard | Controller |
|---|---|---|
| A | Z | A |
| B | X | B or X |
| Start | Enter | Start |
| Select | \ or Backspace | Back |
| L, R | A, S | LB, RB |
| D-pad | Arrow keys | D-pad or left stick |
| Fast-forward (hold) | Space | Right trigger |

Ctrl+R resets the game, like A+B+Start+Select, Ctrl+P pauses and F11 switches to
fullscreen.

The controls, the window size and fullscreen can be changed in `pokeemerald.ini`, which
the game writes with the defaults when it first starts. It's next to the save file, in
`%APPDATA%\pokeemerald` on Windows, `~/.local/share/pokeemerald` on Linux and
`~/Library/Application Support/pokeemerald` on macOS. If the
folder the game starts in has a `pokeemerald.sav`, where earlier versions saved, both
files are there instead. The game prints where they are when it starts, and
`--save FILE` and `--config FILE` use other files.

SDL knows most controllers. For others, put a `gamecontrollerdb.txt` from
[SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) in the same folder.
