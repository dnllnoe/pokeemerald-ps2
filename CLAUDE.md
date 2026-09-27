# pokeemerald-native — Claude Code Configuration
# Native PC port of pret's Pokémon Emerald decompilation — C (gnu99), SDL2

## Project
pokeemerald-native builds Pokémon Emerald as a native program for Linux, Windows and macOS, from [pret's decompilation](https://github.com/pret/pokeemerald). The same sources still build the GBA ROM, and that ROM must not change. The PC port was originally made by [NTx86](https://github.com/NTx86/pokeemerald-sdl2pc); this repository continues from it on its own.

- **Branches**: `vanilla` is the default branch: vanilla Emerald on PC. Work based on [pokeemerald-expansion](https://github.com/rh-hideout/pokeemerald-expansion) goes on its own branch.
- **Upstream code**: pret's fixes come from `pret/pokeemerald`, fetched as `pret/master`. `git show pret/master:<path>` shows the original of any shared file, and `git show pret/master:src/m4a_1.s` the sound engine's assembly.

## Behavioral Rules (Always Enforced)
- NEVER question or doubt what the user says they did (installed, ran, tested, merged) — trust them and focus on the code
- Do what has been asked; nothing more, nothing less
- NEVER create files unless necessary; prefer editing existing files
- NEVER proactively create documentation files (*.md) unless explicitly requested
- NEVER save working files, logs or scratch output in the repo root. Build output goes in `build/` (ignored); anything else goes outside the repo
- ALWAYS read a file before editing it, and read a function's body before calling it in a fix
- ALWAYS build and run the headless tests after code changes, and verify both before committing
- NEVER use temporary workarounds, TODOs, "for now" hacks, or deferred fixes — solve the root cause
- Commit and push only when asked. Every change goes on a branch and through a PR into `vanilla`. NEVER force-push a pushed branch
- NEVER distribute, upload or commit a ROM, a save or a game binary. Print hashes only
- NEVER mention or @-mention third parties in PR descriptions or commit messages
- NEVER run `sudo` or install system packages — the user handles installation

## Port Invariants (CRITICAL)
These are what keep the port correct. Every change, and every audit finding, is checked against them.

1. **The GBA ROM doesn't change.** Changes to pret's shared code (`src/`, `include/`, `data/`, `asm/`, `sound/`) are limited to `#ifdef PORTABLE`, `#ifdef UBFIX` and `#ifdef VER_64BIT` branches, `UNUSED`, and casts that don't change codegen. CI's `gba-rom` job builds the ROM with agbcc and compares it with the PR's base.
2. **The GBA build compiles `src/platform/` too.** It builds every `src/**/*.c`, so every file under `src/platform/` is wrapped in `#ifdef PORTABLE`, which leaves it empty for the GBA. Never remove or narrow that guard.
3. **Pointers are 64-bit.**
   - C: never keep a pointer in a `u32`, `s32` or `int`. Script interpreters read pointer operands with `T1_READ_PTR`, not `T1_READ_32`. The build is position-independent with `-Werror=pointer-to-int-cast` and friends.
   - Data: pointers are written with `ptrvalue` and pointer tables aligned with `ptr_align` (`asm/macros/bit_width.inc`), never `.4byte`. Use `.balign`, never `.align`: GNU as for x86 reads `.align N` as N bytes, and for ARM as 2^N.
4. **Saves keep the GBA layout.** A save from the GBA, a 32-bit build or a 64-bit build loads in each of the others. `src/save.c` converts `SaveBlock1` to and from the GBA layout through `sSaveBlock1Saved` (`SaveBlock1ToSaved`, `SaveBlock1FromSaved`), and migrates saves from older 64-bit builds (`IsOld64BitSaveBlock1`). Any pointer added to a saved struct must be converted there.
5. **Frames are deterministic.** The headless test mode hashes video AND audio every frame, and the hashes must be identical between gcc and clang, and between Linux, Windows and macOS (CI checks all three against `test/headless/expected_frames.txt`). So:
   - no libm in game or audio code (`cgb_audio.c` has its own `Sine`)
   - `-ffp-contract=off` for every build
   - no float-to-int conversion of an out-of-range value
   - no wall clock, randomness or uninitialised memory in anything the game reads
6. **The port plays like the GBA.** The sound engine is checked against pret's assembly, and the renderer, BIOS and DMA against GBATEK and mGBA. Undefined behavior in pret's code is fixed under `UBFIX` by reproducing what the GBA actually does (the Battle Factory's `sPokeballGray_Pal[37]` reads `sInterface_Pal[5]`, where it lands on the GBA). Game bugs pret documents (`// BUG:` with a `#ifdef BUGFIX` alternative) stay, since `BUGFIX` isn't defined.

## C Style
- Match the file. pret's files use Allman braces, 4-space indent, `gGlobalName`, `sStaticName`, `PascalCase` functions, `camelCase` locals and `UPPER_SNAKE` constants. The PC files keep their own existing style.
- gnu99. `WERROR=1` builds stay warning-free with gcc and with clang (`COMPILER=clang`).
- PC-only changes in a shared file are as small as possible, guarded, and commented with why.
- Pin layout assumptions with `STATIC_ASSERT` (saved structs, structs that data in `data/` builds).
- Comments explain why. No `TODO`, `FIXME` or `HACK`.

## User-Facing Text (Plain Prose)
Text a player reads must read like plain, human-written prose with no LLM tics: `README.md`, `INSTALL_PC.md`, messages the PC build prints, and the comments the game writes into `pokeemerald.ini`. The same goes for PR descriptions and commit messages.

The game's own text (`data/text`, `_()` strings, `.string` in scripts) is pret's and is never edited. Developer-facing files (`CLAUDE.md`, `.claude/**`, `test/headless/README.md`) and code comments are out of scope.

- NEVER use an em-dash to splice clauses or tack on an appositive. Write two sentences, or join with a plain word (and, with, where, so, because).
- NEVER join two independent clauses with a semicolon. Semicolons in code and between comma-bearing list items are fine.
- NEVER use a spaced hyphen (` - `) as a stand-in dash.
- NEVER use a dramatic "Label: payload" colon for effect. Real field labels and `**Term**: description` lead-ins are fine.
- AVOID rule-of-three triads and "not just X, but Y" flourishes.

## Build & Test

```bash
# Linux (output: ./pokeemerald64). CI's flags: -g for the data audit, WERROR=1
make -j$(nproc) linux CFLAGS=-g WERROR=1

# The same with clang, or with ASan and UBSan
make -j$(nproc) linux COMPILER=clang WERROR=1
make -j$(nproc) linux CFLAGS=-g WERROR=1 SANITIZE=address,undefined

# Windows, cross-built with MinGW-w64 and SDL2's MinGW files in ./SDL2 (see INSTALL_PC.md)
make -j$(nproc) winwsl WERROR=1              # IS64BIT=0 for 32-bit

# macOS: Homebrew make, pkg-config, libpng, sdl2 and x86_64-elf-binutils
gmake -j$(sysctl -n hw.ncpu) macos WERROR=1

# The GBA ROM (agbcc from pret/agbcc; `make modern` uses arm-none-eabi-gcc)
make -j$(nproc)
```

- The build records its flags in `build/<target>/build_flags.txt`, and a change of flags rebuilds everything. After a sanitizer or clang build, the next normal build is a full one.
- Without agbcc installed, GBA-ROM neutrality can only be checked by CI's `gba-rom` job. Never claim a shared-code change is neutral without it.

```bash
# Headless tests: every scenario, the saves, and the frames against expected_frames.txt
python3 test/headless/smoke.py ./pokeemerald64
python3 test/headless/smoke.py ./pokeemerald64 --audit     # also the map and sound data audit (needs gdb and CFLAGS=-g)

# Two builds frame by frame: a change that must not alter the game shows no differences
python3 test/headless/compare.py BASELINE_BINARY ./pokeemerald64

# One scenario, with its sound (audio.wav) or screenshots
python3 test/headless/run.py ./pokeemerald64 rival_battle --audio
python3 test/headless/run.py ./pokeemerald64 first_battle --shots 30 --sheet
```

- `expected_frames.txt` changes only when a change is meant to change frames (`smoke.py --update-expected`), and the commit says why. Updating it to make a failing check pass hides a regression.
- Scenarios live in `test/headless/scenarios/`. New ones define `play(game)` and use the driver's actions (`continue_game`, `walk_to`, `talk_to`, `finish_dialog`, `fight_battle`, `save_game`) rather than hand-timed input. See `test/headless/README.md`.
- CI (`.github/workflows/build.yml`) runs on pushes to `vanilla` and on every PR:
  - `build`: the modern GBA build, Windows 64 and 32-bit, Linux with the smoke tests and the data audit
  - `gba-rom`: the agbcc ROM compared with the PR's base
  - `sanitizers`: ASan and UBSan with the smoke tests
  - `macos`: Apple Silicon, with the smoke tests
  - `windows-test`: the smoke tests on Windows, with the frames compared with Linux's

A PR merges only when all five pass.

### Directory Structure
```
src/                  — pret's game code, shared with the GBA build
src/platform/         — the PC layer, all inside #ifdef PORTABLE:
  sdl2.c              —   SDL2 backend: frame loop, audio queue, input, save and settings files, test mode
  settings.c          —   pokeemerald.ini and the key and controller bindings
  system.c            —   interrupts, soft reset (the gba_ram section), AudioUpdate
  test_state.c        —   the game state the headless driver reads
  gba_easy_draw.c     —   the renderer
  bios.c, dma.c       —   GBA BIOS calls and DMA
  cgb_audio.c         —   the Game Boy sound channels
  win32res/           —   the Windows icon
src/music_player.c    — the m4a sound engine's sequencer, in C (pret's is assembly)
src/sound_mixer.c     — the m4a mixer, in C
src/stub.c            — PC stubs for link, multiboot and other hardware-only code
src/data/             — C data tables
include/              — headers (include/platform/ for the PC layer, include/gba/ for the hardware)
data/                 — event, battle, AI and contest scripts and maps, in GNU assembler
asm/macros/           — assembler macros (bit_width.inc has ptrvalue and ptr_align)
sound/                — voicegroups, songs and samples
graphics/             — images, converted at build time by gbagfx
tools/                — build tools (preproc, mapjson, jsonproc, gbagfx, mid2agb, scaninc, elf2macho, ...)
test/headless/        — the headless tests and their Python driver
.github/workflows/    — CI
.claude/              — the code-audit skill and the pe-*-reviewer agents
```

## Skills
- `code-audit` — the multi-pass audit-and-fix loop. Its partition reviewers are the `pe-*-reviewer` agents in `.claude/agents/review/`: platform, audio, renderer and hardware, saves, shared game code, data and assembler, and build and tests.

## Git
- Branch off `vanilla`, open a PR into `vanilla`, merge when CI is green.
- Commit subjects are a plain imperative sentence saying what changed (no conventional-commit prefixes). The body says why, and how it was verified. Check every claim in a message against the staged diff before committing.
- NEVER force-push a pushed branch. NEVER commit build output, ROMs, saves or `pokeemerald.ini`.

## Security
- Files read from disk are a trust boundary: the save, `pokeemerald.ini`, `gamecontrollerdb.txt` and the test mode's input files. Check sizes, lengths and what a short or malformed file does.
- pret's code trusts its data completely. Anything that ever feeds it data from outside (a network link, an imported save) must validate it first.
- NEVER hardcode or commit secrets or credentials.

## Concurrency: Batch Independent Operations
- Batch INDEPENDENT file reads, edits and shell commands in one message
- Spawn all agents for a task in ONE message, each with full instructions
- "Independent" is the operative word. A read and the edit that depends on it cannot go in the same message: batch the reads, then batch the edits

## Key Pitfalls
- Removing an `#ifdef` in `src/platform/` breaks the GBA build, which only CI builds
- `.4byte` for a pointer assembles fine and is silently truncated; `.align` means different things on x86 and ARM
- A build that succeeds is not proof its output is right: the macOS assembler without `--divide` built 530 wrong data objects without an error
- libm, float contraction and uninitialised reads make frames differ between systems
- The test mode never initialises SDL; nothing on its path may touch video or audio devices
- Soft reset clears only the game's variables; platform state survives it
- Saves must load on the GBA and on every build: never change a saved byte without converting it

## Support
- Repository: https://github.com/fuddlesworth/pokeemerald-native
- Decompilation: https://github.com/pret/pokeemerald
- Original PC port: https://github.com/NTx86/pokeemerald-sdl2pc
