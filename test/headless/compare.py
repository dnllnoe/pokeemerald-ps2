#!/usr/bin/env python3
"""Compares two builds frame by frame.

Runs the same scenarios with both builds and reports the first frame whose
video or audio differs. Changes that shouldn't affect the game (refactors,
build changes, platform fixes) should show no differences at all. Scenarios
after new_game start from the save the BASELINE build made in the previous
one, so loading older saves gets tested too.

usage: compare.py BASELINE CANDIDATE [--out DIR] [--random-seeds 1,2,3] [--random-frames N]
"""

import argparse
import os
import sys

from headless import REPO_DIR, run_scenario


def first_difference(a, b, index):
    for frame, (x, y) in enumerate(zip(a, b)):
        if x[index] != y[index]:
            return frame
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("--out", default=os.path.join(REPO_DIR, "build", "headless-compare"))
    parser.add_argument("--random-seeds", default="1", help="comma-separated seeds for random_play")
    parser.add_argument("--random-frames", type=int, default=20000)
    args = parser.parse_args()

    # (name, scenario, parameters, name of the run whose baseline save to start from)
    runs = [("new_game", "new_game", {}, None), ("continue_game", "continue_game", {}, "new_game"),
            ("first_battle", "first_battle", {}, "continue_game")]
    for seed in args.random_seeds.split(","):
        runs.append((f"random_play_{seed}", "random_play", {"SEED": int(seed), "FRAMES": args.random_frames},
                     "first_battle"))

    differences = 0
    baseline_saves = {}
    for name, scenario, params, save_from in runs:
        results = {}
        for label, binary in (("baseline", args.baseline), ("candidate", args.candidate)):
            out = os.path.join(args.out, label, name)
            results[label] = run_scenario(binary, scenario, out, save=baseline_saves.get(save_from), params=params)
        baseline_saves[name] = results["baseline"].save_path

        a, b = results["baseline"].hashes(), results["candidate"].hashes()
        video, audio = first_difference(a, b, 0), first_difference(a, b, 1)
        status = []
        for label, run in results.items():
            if not run.ok:
                status.append(f"{label} failed (exit code {run.returncode}, {len(run.hashes())}/{run.frames} frames)")
        if len(a) != len(b):
            status.append(f"{len(a)} vs {len(b)} frames")
        if video is not None:
            status.append(f"video differs from frame {video}")
        if audio is not None:
            status.append(f"audio differs from frame {audio}")
        differences += bool(status)
        print(f"{name}: " + ("; ".join(status) if status else f"identical ({len(a)} frames)"))

    sys.exit(1 if differences else 0)


if __name__ == "__main__":
    main()
