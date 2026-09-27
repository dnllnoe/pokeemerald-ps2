# Continues a save and presses random buttons. Parameters: SEED, FRAMES.
import random

rng = random.Random(SEED)
steps = wait(300) + tap("START") + wait(120) + tap("START") + wait(200) + tap("A") + wait(200)  # main menu
steps += tap("A") + wait(200)                                   # CONTINUE
buttons = [("A", 22), ("B", 8), ("UP", 18), ("DOWN", 14), ("LEFT", 14), ("RIGHT", 14),
           ("START", 3), ("SELECT", 1), ("L", 1), ("R", 1), ("-", 4)]
names = [name for name, _ in buttons]
weights = [weight for _, weight in buttons]
while sum(frames for frames, _ in steps) < FRAMES:
    steps.append((rng.randint(2, 40), rng.choices(names, weights)[0]))
