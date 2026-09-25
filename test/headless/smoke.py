#!/usr/bin/env python3
"""Plays the game headless and checks that it works end to end.

1. new_game: starts a new game and saves. The save must be valid in the GBA
   format and hold the expected state.
2. continue_game: continues that save, plays on and saves again.
3. first_battle: continues that save, fights the first battle and saves with
   the starter in the party.
4. random_play: continues the last save and presses random buttons.
5. With --audit, audit_data.py (needs gdb and a build with debug info).

Every run must exit normally after all of its frames.

usage: smoke.py BINARY [--out DIR] [--random-frames N] [--audit]
"""

import argparse
import os
import subprocess
import sys

import savecheck
from headless import REPO_DIR, TEST_DIR, run_scenario

failures = []


def check(ok, message):
    print(("PASS: " if ok else "FAIL: ") + message)
    if not ok:
        failures.append(message)
    return ok


def check_run(name, run):
    ran = len(run.hashes())
    ok = check(run.ok, f"{name} ran {ran}/{run.frames} frames, exit code {run.returncode}")
    if not ok and run.stderr.strip():
        print(run.stderr.strip()[-2000:])
    return ok


def check_save(name, path, **expected):
    save, problems = savecheck.check(path, **expected)
    if save:
        print("      " + save.summary())
    check(not problems, f"{name} save" + (": " + "; ".join(problems) if problems else " is valid and as expected"))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("binary")
    parser.add_argument("--out", default=os.path.join(REPO_DIR, "build", "headless"))
    parser.add_argument("--random-frames", type=int, default=20000)
    parser.add_argument("--audit", action="store_true", help="also run audit_data.py")
    args = parser.parse_args()

    # Brendan's room is map 1.1, the house's first floor 1.0 and Birch's lab 1.4
    new_game = run_scenario(args.binary, "new_game", os.path.join(args.out, "new_game"))
    if check_run("new_game", new_game):
        check_save("new_game", new_game.save_path, expect_counter=1, expect_name="AAAAAAA",
                   expect_money=3000, expect_map="1.1", expect_party=0)

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

                random_play = run_scenario(args.binary, "random_play", os.path.join(args.out, "random_play"),
                                           save=battle.save_path,
                                           params={"SEED": 1, "FRAMES": args.random_frames})
                check_run("random_play", random_play)

    if args.audit:
        audit = subprocess.run([sys.executable, os.path.join(TEST_DIR, "audit_data.py"), args.binary])
        check(audit.returncode == 0, "map and sound data audit")

    print(f"{len(failures)} failure(s)" if failures else "All checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
