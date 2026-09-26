#!/usr/bin/env python3
"""Plays the game headless and checks that it works end to end.

1. new_game: starts a new game and saves. The save must be valid in the GBA
   format and hold the expected state.
2. continue_game: continues that save, plays on and saves again.
3. first_battle: continues that save, fights the first battle and saves with
   the starter in the party.
4. soft_reset: continues that save, walks away and resets with
   A+B+START+SELECT. After the reset, the same input has to give the same
   frames as after power on, and saving has to put the player back where the
   game was saved.
5. random_play: continues the first_battle save and presses random buttons.
6. With --audit, audit_data.py (needs gdb and a build with debug info).

Every run must exit normally after all of its frames, and give the frames in
expected_frames.txt. When a change is meant to change them, --update-expected
writes the new ones.

usage: smoke.py BINARY [--out DIR] [--random-frames N] [--audit] [--update-expected]
"""

import argparse
import hashlib
import os
import subprocess
import sys

import savecheck
from headless import REPO_DIR, TEST_DIR, run_scenario, scenario_variables

EXPECTED_FRAMES_PATH = os.path.join(TEST_DIR, "expected_frames.txt")
EXPECTED_FRAMES_HEADER = """\
# A SHA-256 of the video and audio hashes of every frame of each smoke.py run.
# Every build, on every platform, has to give exactly these frames. When a
# change is meant to change them, update this file and say why in the commit:
#   python3 test/headless/smoke.py BINARY --update-expected
"""
DEFAULT_RANDOM_FRAMES = 20000

failures = []
expected_frames = {}  # run name: (frames, digest)
new_frames = {}


def check(ok, message):
    print(("PASS: " if ok else "FAIL: ") + message)
    if not ok:
        failures.append(message)
    return ok


def check_run(name, run, check_frames=True):
    ran = len(run.hashes())
    ok = check(run.ok, f"{name} ran {ran}/{run.frames} frames, exit code {run.returncode}")
    if not ok and run.stderr.strip():
        print(run.stderr.strip()[-2000:])
    if ok and check_frames:
        text = "".join(f"{video} {audio}\n" for video, audio in run.hashes())
        new_frames[name] = (run.frames, hashlib.sha256(text.encode()).hexdigest())
        if expected_frames is not None:
            same = expected_frames.get(name) == new_frames[name]
            check(same, f"{name} gives the frames in expected_frames.txt" if same else
                  f"{name} doesn't give the frames in expected_frames.txt (compare.py finds the first different "
                  "frame; if the change is meant to change them, use --update-expected)")
    return ok


def load_expected_frames():
    with open(EXPECTED_FRAMES_PATH) as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                name, frames, digest = line.split()
                expected_frames[name] = (int(frames), digest)


def write_expected_frames():
    with open(EXPECTED_FRAMES_PATH, "w", newline="\n") as f:
        f.write(EXPECTED_FRAMES_HEADER)
        for name, (frames, digest) in new_frames.items():
            f.write(f"{name} {frames} {digest}\n")
    print(f"Wrote {EXPECTED_FRAMES_PATH}")


def check_save(name, path, **expected):
    save, problems = savecheck.check(path, **expected)
    if save:
        print("      " + save.summary())
    check(not problems, f"{name} save" + (": " + "; ".join(problems) if problems else " is valid and as expected"))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("binary")
    parser.add_argument("--out", default=os.path.join(REPO_DIR, "build", "headless"))
    parser.add_argument("--random-frames", type=int, default=DEFAULT_RANDOM_FRAMES)
    parser.add_argument("--audit", action="store_true", help="also run audit_data.py")
    parser.add_argument("--update-expected", action="store_true",
                        help="write the frames of this build to expected_frames.txt instead of checking them")
    args = parser.parse_args()

    global expected_frames
    if args.update_expected:
        expected_frames = None
    else:
        load_expected_frames()

    # Brendan's room is map 1.1, the house's first floor 1.0 and Birch's lab 1.4.
    # The clock gets set in new_game, on the test clock's date (2026-01-01),
    # which is day 9498 of the RTC.
    new_game = run_scenario(args.binary, "new_game", os.path.join(args.out, "new_game"))
    if check_run("new_game", new_game):
        check_save("new_game", new_game.save_path, expect_counter=1, expect_name="AAAAAAA",
                   expect_money=3000, expect_map="1.1", expect_party=0, expect_clock_days=9498)

        continued = run_scenario(args.binary, "continue_game", os.path.join(args.out, "continue_game"),
                                 save=new_game.save_path)
        if check_run("continue_game", continued):
            check_save("continue_game", continued.save_path, expect_counter=2, expect_name="AAAAAAA",
                       expect_money=3000, expect_map="1.0")

            battle = run_scenario(args.binary, "first_battle", os.path.join(args.out, "first_battle"),
                                  save=continued.save_path)
            if check_run("first_battle", battle):
                check_save("first_battle", battle.save_path, expect_counter=3, expect_name="AAAAAAA",
                           expect_money=3000, expect_map="1.4", expect_party=1)

                reset = run_scenario(args.binary, "soft_reset", os.path.join(args.out, "soft_reset"),
                                     save=battle.save_path)
                if check_run("soft_reset", reset):
                    variables = scenario_variables("soft_reset")
                    hashes, count = reset.hashes(), variables["START_FRAMES"]
                    after_reset = hashes[variables["RESET_FRAME"] + 1:][:count]
                    check(after_reset == hashes[:count],
                          f"soft_reset starts over like at power on ({count} frames with the same input)")
                    # Where first_battle saved
                    check_save("soft_reset", reset.save_path, expect_counter=4, expect_map="1.4",
                               expect_party=1, expect_position=(6, 5))

                random_play = run_scenario(args.binary, "random_play", os.path.join(args.out, "random_play"),
                                           save=battle.save_path,
                                           params={"SEED": 1, "FRAMES": args.random_frames})
                check_run("random_play", random_play, check_frames=args.random_frames == DEFAULT_RANDOM_FRAMES)

    if args.audit:
        audit = subprocess.run([sys.executable, os.path.join(TEST_DIR, "audit_data.py"), args.binary])
        check(audit.returncode == 0, "map and sound data audit")

    if args.update_expected:
        if failures:
            print(f"Not updating {EXPECTED_FRAMES_PATH}, since checks failed")
        else:
            write_expected_frames()
    print(f"{len(failures)} failure(s)" if failures else "All checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
