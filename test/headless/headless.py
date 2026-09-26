"""Runs the game's headless test mode (--test-input) from Python.

A scenario is a Python file that builds `steps`, a list of (frames, buttons)
tuples: hold `buttons` ("A", "A+UP", "-" for none, ...) for `frames` frames.
The helpers below are available in scenario files without importing them.
"""

import glob
import os
import shutil
import subprocess

TEST_DIR = os.path.dirname(os.path.abspath(__file__))
SCENARIO_DIR = os.path.join(TEST_DIR, "scenarios")
REPO_DIR = os.path.dirname(os.path.dirname(TEST_DIR))


def tap(button, times=1, hold=2, gap=20):
    """Press a button `times` times, holding it `hold` frames with `gap` frames between presses."""
    steps = []
    for _ in range(times):
        steps += [(hold, button), (gap, "-")]
    return steps


def wait(frames):
    return [(frames, "-")]


def walk(direction, tiles):
    """Walk `tiles` tiles, at 16 frames per tile."""
    return [(16 * tiles, direction), (4, "-")]


def load_scenario(scenario, **params):
    """Returns the steps of a scenario, given by name (see scenarios/) or path.
    `params` are made available to the scenario as variables."""
    return scenario_variables(scenario, **params)["steps"]


def scenario_variables(scenario, **params):
    """Returns all of the variables a scenario sets, like load_scenario. A
    scenario can set CONFIG to a settings file in this directory to play with."""
    path = scenario if scenario.endswith(".py") else os.path.join(SCENARIO_DIR, scenario + ".py")
    namespace = {"tap": tap, "wait": wait, "walk": walk, "load_scenario": load_scenario, "CONFIG": None, **params}
    with open(path) as f:
        exec(compile(f.read(), path, "exec"), namespace)
    return namespace


class Run:
    def __init__(self, out_dir, frames, returncode, stderr):
        self.out_dir = out_dir
        self.frames = frames          # frames in the input
        self.returncode = returncode
        self.stderr = stderr
        self.save_path = os.path.join(out_dir, "game.sav")
        self.hashes_path = os.path.join(out_dir, "hashes.txt")

    def hashes(self):
        """(video hash, audio hash) of every frame that ran."""
        if not os.path.exists(self.hashes_path):
            return []
        with open(self.hashes_path) as f:
            return [tuple(line.split()[1:3]) for line in f]

    @property
    def ok(self):
        """The game exited normally after running every frame of the input."""
        return self.returncode == 0 and len(self.hashes()) == self.frames


def run_scenario(binary, scenario, out_dir, save=None, shot_every=0, params=None, timeout=None):
    """Runs a scenario and returns a Run. `save` is a save file to start from
    (it's copied, not modified). With `shot_every`, a PPM screenshot is written
    every that many frames, plus one of the last frame."""
    variables = scenario_variables(scenario, **(params or {}))
    steps = variables["steps"]
    os.makedirs(out_dir, exist_ok=True)
    for old in glob.glob(os.path.join(out_dir, "frame_*.ppm")):
        os.remove(old)

    input_path = os.path.join(out_dir, "input.txt")
    with open(input_path, "w") as f:
        for frames, buttons in steps:
            f.write(f"{frames} {buttons}\n")

    run = Run(out_dir, sum(frames for frames, _ in steps), None, "")
    if os.path.exists(run.save_path):
        os.remove(run.save_path)
    if save:
        shutil.copyfile(save, run.save_path)

    cmd = [os.path.abspath(binary), "--save", run.save_path, "--test-input", input_path,
           "--test-hashes", run.hashes_path]
    if variables["CONFIG"]:
        cmd += ["--config", os.path.join(TEST_DIR, variables["CONFIG"])]
    if shot_every:
        cmd += ["--test-shots", out_dir, "--test-shot-every", str(shot_every)]
    proc = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, timeout=timeout)
    run.returncode = proc.returncode
    run.stderr = proc.stderr
    return run


def contact_sheet(out_dir, first_frame=0, columns=6, max_shots=48):
    """Combines the screenshots of a run into sheet.png with ImageMagick, if it's installed."""
    magick = shutil.which("magick") or shutil.which("montage")
    shots = sorted(glob.glob(os.path.join(out_dir, "frame_*.ppm")))
    shots = [s for s in shots if int(s[-10:-4]) >= first_frame][:max_shots]
    if not magick or not shots:
        return None
    args = [magick, "montage"] if magick.endswith("magick") else [magick]
    for shot in shots:
        args += ["-label", str(int(shot[-10:-4])), shot]
    sheet = os.path.join(out_dir, "sheet.png")
    args += ["-tile", f"{columns}x", "-geometry", "240x160+2+2", "-pointsize", "12",
             "-background", "#333", "-fill", "white", sheet]
    subprocess.run(args, check=True)
    return sheet
