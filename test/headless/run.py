#!/usr/bin/env python3
"""Runs one scenario headless and reports what happened.

usage: run.py BINARY SCENARIO [--out DIR] [--save FILE] [--shots N] [--sheet] [--param NAME=VALUE ...]

SCENARIO is a name from scenarios/ or a path to a scenario file. With --shots N
a screenshot is written every N frames; --sheet combines them into sheet.png
(needs ImageMagick), which is the easiest way to write or fix a scenario.
"""

import argparse
import os
import sys

from headless import REPO_DIR, contact_sheet, run_scenario


def parse_param(text):
    name, _, value = text.partition("=")
    try:
        return name, int(value)
    except ValueError:
        return name, value


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("binary")
    parser.add_argument("scenario")
    parser.add_argument("--out", help="output directory (default: build/headless/SCENARIO)")
    parser.add_argument("--save", help="save file to start from")
    parser.add_argument("--shots", type=int, default=0, metavar="N", help="screenshot every N frames")
    parser.add_argument("--sheet", action="store_true", help="combine the screenshots into sheet.png")
    parser.add_argument("--sheet-from", type=int, default=0, metavar="FRAME", help="first frame on the sheet")
    parser.add_argument("--param", action="append", default=[], type=parse_param, metavar="NAME=VALUE")
    parser.add_argument("--audio", action="store_true", help="write the sound to audio.wav")
    args = parser.parse_args()

    name = os.path.splitext(os.path.basename(args.scenario))[0]
    out = args.out or os.path.join(REPO_DIR, "build", "headless", name)
    run = run_scenario(args.binary, args.scenario, out, save=args.save, shot_every=args.shots,
                       params=dict(args.param), audio=args.audio)
    ran = len(run.hashes())
    print(f"{name}: exit code {run.returncode}, {ran}/{run.frames} frames, output in {out}")
    if run.stderr.strip():
        print(run.stderr.strip()[-2000:])
    if args.sheet:
        sheet = contact_sheet(out, first_frame=args.sheet_from)
        print("sheet:", sheet or "no screenshots or ImageMagick not found")
    sys.exit(0 if run.ok else 1)


if __name__ == "__main__":
    main()
