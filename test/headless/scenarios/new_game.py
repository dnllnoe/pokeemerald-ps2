# Starts a new game, goes through the intro, sets the clock, opens the bag
# and the trainer card, and saves in the player's room (Brendan's house 2F).
steps = wait(300) + tap("START") + wait(120) + tap("START") + wait(90) + tap("A") + wait(120)
steps += tap("A", 175, hold=4, gap=26)                 # Birch's speech, naming (AAAAAAA)
steps += wait(700)                                      # truck ride, the door opens
steps += walk("RIGHT", 4)                               # leave the truck
steps += tap("A", 90, hold=4, gap=26)                  # Mom
steps += wait(60)
steps += walk("RIGHT", 2) + wait(30) + walk("UP", 6) + wait(30)
steps += walk("LEFT", 2) + walk("UP", 3) + wait(120)    # upstairs
steps += walk("LEFT", 2) + [(4, "UP")] + wait(20) + tap("A", 6, hold=4, gap=30)
steps += [(90, "UP")] + wait(20) + tap("A", 8, hold=4, gap=30) + wait(60)   # clock screen
steps += [(120, "RIGHT")] + wait(20) + tap("A") + wait(40) + tap("UP") + tap("A") + wait(120)  # set it, YES
steps += tap("A", 40, hold=4, gap=26) + wait(60)       # Mom comes upstairs
steps += tap("START") + wait(30) + tap("A") + wait(90) + tap("B") + wait(90)   # bag, close it
steps += tap("DOWN") + tap("A") + wait(120) + tap("A") + wait(120)          # trainer card, flip it
steps += tap("B", 3, gap=120)                                              # back to the field
steps += tap("START") + wait(30) + tap("DOWN") + tap("A") + wait(90)       # SAVE
steps += tap("A") + wait(300) + tap("A") + wait(60)                        # YES
steps += wait(120) + tap("A") + wait(60)
