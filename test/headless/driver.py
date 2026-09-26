"""Plays the game from Python, deciding what to press from what's going on.

The game runs its test mode with its input coming from here a line at a time,
and after each line it writes its state: the map and position, whether a
script or a battle is running, the music and so on (see WriteTestState in
src/platform/test_state.c). Everything pressed is written to input.txt, so a
run can be replayed like any other scenario, and gives the same frames.

A scenario that defines play(game) is played this way, with the actions below:

    def play(game):
        continue_game(game)
        walk_to(game, 11, 0)          # on the current map, around people
        leave_map(game, "UP")
        finish_dialog(game)
"""

import json
import os
import shutil
import subprocess

import maps


class GameError(Exception):
    pass


class State:
    def __init__(self, data):
        self.__dict__.update(data)
        self.pos = tuple(self.pos)
        self.objects = [tuple(o) for o in self.objects]

    @property
    def map_name(self):
        return maps.map_name(*self.map) if self.map[0] >= 0 else None

    @property
    def free(self):
        """In the overworld with nothing going on, so the player can move."""
        return (self.overworld and not self.locked and not self.script and not self.battle
                and not self.fade and not self.moving)

    def __repr__(self):
        return (f"<frame {self.frame}: {self.map_name} {self.pos}, "
                f"{'battle' if self.battle else 'free' if self.free else 'busy'}, bgm {self.bgm}>")


class Game:
    def __init__(self, binary, out_dir, save=None, config=None, shot_every=0, audio=False, max_frames=200000):
        os.makedirs(out_dir, exist_ok=True)
        self.out_dir = out_dir
        self.save_path = os.path.join(out_dir, "game.sav")
        self.hashes_path = os.path.join(out_dir, "hashes.txt")
        self.max_frames = max_frames
        if os.path.exists(self.save_path):
            os.remove(self.save_path)
        if save:
            shutil.copyfile(save, self.save_path)

        cmd = [os.path.abspath(binary), "--save", self.save_path, "--test-input", "-", "--test-state", "-",
               "--test-hashes", self.hashes_path]
        if config:
            cmd += ["--config", config]
        if shot_every:
            cmd += ["--test-shots", out_dir, "--test-shot-every", str(shot_every)]
        if audio:
            cmd += ["--test-audio", os.path.join(out_dir, "audio.wav")]
        self.stderr = open(os.path.join(out_dir, "stderr.txt"), "w")
        # Every state read, for looking into what happened
        self.states = open(os.path.join(out_dir, "states.jsonl"), "w")
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr,
                                     text=True, bufsize=1)
        self.steps = []
        self.frames = 0
        self.state = self._read_state()

    def _read_state(self):
        for line in self.proc.stdout:
            if line.startswith("{"):
                self.states.write(line)
                return State(json.loads(line))
        raise GameError(f"the game exited (exit code {self.proc.wait()})")

    def press(self, buttons="-", frames=1):
        """Holds buttons ("A", "UP+B", "-" for none) for some frames."""
        if self.frames + frames > self.max_frames:
            raise GameError(f"over {self.max_frames} frames")
        self.proc.stdin.write(f"{frames} {buttons}\n")
        self.proc.stdin.flush()
        self.steps.append((frames, buttons))
        self.frames += frames
        self.state = self._read_state()
        return self.state

    def wait(self, frames):
        return self.press("-", frames)

    def tap(self, button, times=1, hold=2, gap=20):
        for _ in range(times):
            self.press(button, hold)
            self.press("-", gap)
        return self.state

    def finish(self):
        """Ends the game and writes input.txt. Returns the exit code."""
        with open(os.path.join(self.out_dir, "input.txt"), "w") as f:
            for frames, buttons in self.steps:
                f.write(f"{frames} {buttons}\n")
        self.proc.stdin.close()
        returncode = self.proc.wait()
        self.proc.stdout.close()
        self.stderr.close()
        self.states.close()
        return returncode


def wait_until(game, condition, step=2, limit=3000):
    """Waits until condition(state) is true."""
    start = game.frames
    while not condition(game.state):
        if game.frames - start > limit:
            raise GameError(f"waited {limit} frames, now {game.state}")
        game.wait(step)
    return game.state


def continue_game(game):
    """From power on: skips the intro and continues the save."""
    game.wait(300)
    game.tap("START")
    game.wait(120)
    game.tap("START")
    game.wait(200)
    game.tap("A")
    game.wait(200)
    game.tap("A")                   # CONTINUE
    return wait_until(game, lambda s: s.free)


def finish_dialog(game, button="A", limit=400):
    """Presses button until the player can move: through text, cutscenes, and
    battles that come up (see fight_battle)."""
    for _ in range(limit):
        if game.state.free:
            return game.state
        if game.state.battle:
            fight_battle(game)
        else:
            game.tap(button, hold=2, gap=10)
    raise GameError(f"still busy after {limit} presses: {game.state}")


def fight_battle(game, limit=600):
    """Fights with the first move until the battle ends. A goes through
    everything else, like the text and learning a move."""
    for _ in range(limit):
        if not game.state.battle:
            return wait_until(game, lambda s: not s.fade and s.overworld)
        game.tap("A", hold=2, gap=10)
    raise GameError(f"the battle didn't end: {game.state}")


def walk_to(game, x, y, grass_cost=20, on_battle=fight_battle):
    """Walks to (x, y) on the current map, around people and through as little
    grass as it can. Battles that come up go to on_battle, and scripts that
    start are finished with finish_dialog. Stops if something takes the player
    to another map."""
    goal = (x, y)
    start_map = game.state.map
    blocked = set()
    tried = 0
    while True:
        state = game.state
        if state.map != start_map:
            return wait_until(game, lambda s: s.free or s.locked or s.script or s.battle)
        if state.battle:
            on_battle(game)
            continue
        if state.locked or state.script:
            finish_dialog(game)
            continue
        if not state.overworld or state.fade:
            game.wait(2)
            continue
        if state.pos == goal:
            if not state.moving:
                return state
            game.wait(1)
            continue

        path = maps.load(state.map_name).path(state.pos, goal, blocked | set(state.objects), grass_cost)
        if path is None:
            raise GameError(f"no way to {goal} from {state}")
        dx, dy = maps.DIRECTIONS[path[0]]
        next_tile = (state.pos[0] + dx, state.pos[1] + dy)
        game.press(path[0], 2)
        if game.state.pos != state.pos:
            tried = 0
            blocked.clear()
        else:
            # Something the map doesn't know about is in the way
            tried += 2
            if tried > 40:
                blocked.add(next_tile)
                tried = 0


FACING = {"DOWN": 1, "UP": 2, "LEFT": 3, "RIGHT": 4}


def talk_to(game, direction):
    """Turns to face direction and talks to whoever is there, until the player
    can move again."""
    for _ in range(10):
        if game.state.facing == FACING[direction]:
            break
        game.press(direction, 1)
        game.wait(8)
    game.tap("A")
    return finish_dialog(game)


def leave_map(game, direction, limit=300):
    """Walks off the map, or through a door, in a direction until the map changes."""
    start = game.state.map
    for _ in range(limit):
        if game.state.map != start:
            return wait_until(game, lambda s: s.free or s.locked or s.script or s.battle)
        game.press(direction, 2)
    raise GameError(f"didn't leave {game.state.map_name} going {direction}: {game.state}")


def save_game(game, menu_position):
    """Saves from the start menu, where SAVE is menu_position items down."""
    game.tap("START")
    game.wait(30)
    game.tap("DOWN", menu_position)
    game.tap("A")
    game.wait(90)
    game.tap("A")                   # YES
    game.wait(300)
    game.tap("A")
    game.wait(240)
    game.tap("A")                   # overwrite: YES
    game.wait(300)
    game.tap("B")
    return wait_until(game, lambda s: s.free)
