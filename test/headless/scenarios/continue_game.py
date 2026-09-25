# Continues the save made by new_game, goes downstairs for the TV event, and
# saves again (overwriting the existing file).
steps = wait(300) + tap("START") + wait(120) + tap("START") + wait(200) + tap("A") + wait(200)  # main menu
steps += tap("A") + wait(200)                                   # CONTINUE
steps += walk("RIGHT", 2) + walk("UP", 1) + wait(120)          # downstairs
steps += tap("A", 70, hold=4, gap=26)                          # Mom and the TV
steps += walk("DOWN", 4) + walk("LEFT", 2) + walk("DOWN", 4) + wait(120)
steps += tap("START") + wait(30) + tap("DOWN") + tap("DOWN") + tap("A") + wait(90)   # SAVE
steps += tap("A") + wait(300) + tap("A") + wait(120)                                  # YES
steps += wait(120) + tap("A") + wait(300) + tap("A") + wait(120)                   # overwrite: YES
