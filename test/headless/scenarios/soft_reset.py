# Continues the save made by first_battle, walks away from where it was saved
# and resets with A+B+START+SELECT, which loses that. After the reset, the game
# has to start over the same way it did at power on: the same input gives the
# same frames. Then it saves again where the game was saved.
start = wait(300) + tap("START") + wait(120) + tap("START") + wait(200) + tap("A") + wait(200)  # main menu
start += tap("A") + wait(200)                                    # CONTINUE
steps = start + walk("DOWN", 2) + wait(30)                       # away from where the game was saved

START_FRAMES = sum(frames for frames, _ in start)
RESET_FRAME = sum(frames for frames, _ in steps)
steps += [(1, "A+B+START+SELECT")] + start

steps += tap("START") + wait(30) + tap("DOWN", 3) + tap("A") + wait(90)   # SAVE, below POKEMON, BAG and the player
steps += tap("A") + wait(300) + tap("A") + wait(120)                      # YES
steps += wait(120) + tap("A") + wait(300) + tap("B") + wait(120)          # overwrite: YES
