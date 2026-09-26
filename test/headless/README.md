# Headless tests

These scripts play the PC build without a window, using its test mode:

```
pokeemerald64 --test-input input.txt --test-hashes hashes.txt [--save game.sav] [--test-shots DIR --test-shot-every N]
```

The input file holds lines of `<frames> <buttons>`, like `30 A+UP` or `60 -`, and the
game exits when it ends. Each frame runs one audio update and the clock is fixed, so a
run only depends on its input and save file: the same build always produces the same
frames, and two builds that behave the same produce the same hashes.

Everything needs Python 3. The scripts work with any PC build (Linux or Windows).

## Checks

`smoke.py BINARY` plays a new game up to the first save, continues that save and saves
again, fights the first battle and saves with the starter, fights it again with keys
and controller inputs (default and remapped), resets with A+B+START+SELECT and saves
again, and plays with random buttons. Every run has to finish, and the saves
have to be valid GBA-format saves with the expected contents. After the reset, the game
has to give the same frames as after power on. CI runs it on Linux and Windows for every
push and pull request.

Every run also has to give exactly the frames in `expected_frames.txt`, a SHA-256 of each
run's frame hashes. Builds agree on them across compilers and platforms, so a difference
means the game behaves differently. If a change is meant to do that, write the new frames
with `smoke.py BINARY --update-expected` and commit them with the change.

`smoke.py BINARY --audit` also runs `audit_data.py`, which checks every map and song in
the running game through gdb: each pointer, count and struct alignment, the way the C
code reads them. It covers data the scenarios never load. It needs Linux, gdb, and a
build with debug info (`make linux CFLAGS=-g`).

`compare.py BASELINE CANDIDATE` runs the same scenarios with two builds and reports the
first frame whose video or audio differs. Use it to confirm that a change doesn't alter
the game, e.g. by comparing a build of your branch against one of `pc_port`.

`savecheck.py SAVE` checks a save file against the GBA format, independently of the
game code, and prints what's in it.

## Scenarios

Scenarios are in `scenarios/`. Each builds `steps`, a list of `(frames, buttons)`, with
the helpers `tap`, `wait` and `walk` (see `headless.py`):

```python
steps = wait(300) + tap("START")          # skip the intro
steps += walk("UP", 3)                    # 16 frames per tile
steps += tap("A", 10, hold=4, gap=26)     # press A 10 times
```

Besides the GBA buttons, a step can press keys and controller inputs, which go through
the bindings in the settings like when playing: `key:Z`, `pad:a`, `pad:lefty-` (the
names are the settings file's). A scenario that sets `CONFIG` to a settings file in
this directory plays with it (see `scenarios/controls.py`).

They are timed to the frame, so a change to game timing can make one go off track. To
write or fix one, take screenshots and look at them on a contact sheet (needs
ImageMagick):

```
python3 test/headless/run.py ./pokeemerald64 new_game --shots 60 --sheet
```

`--save FILE` starts from a save, and `--param NAME=VALUE` passes values like `SEED`
and `FRAMES` to `random_play`. Output goes to `build/headless/`.
