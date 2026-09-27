# Continues the save made by continue_game: meets May, rescues Birch,
# wins the first battle, and saves with the starter in the party.
steps = wait(300) + tap("START") + wait(120) + tap("START") + wait(200) + tap("A") + wait(200)  # main menu
steps += tap("A") + wait(200)                                   # CONTINUE
steps += walk("RIGHT", 4) + walk("DOWN", 3) + walk("RIGHT", 1) + walk("DOWN", 2) + wait(60)
steps += walk("RIGHT", 1) + walk("DOWN", 1) + wait(120)         # out of the house
steps += walk("RIGHT", 9) + walk("UP", 1) + wait(120)           # into May's house
steps += tap("A", 30, hold=4, gap=26) + wait(60)                # May's mom
steps += tap("B", 6, hold=4, gap=26) + wait(60)           # B closes text without talking again
steps += walk("UP", 6) + wait(120)                            # upstairs
steps += walk("RIGHT", 4) + walk("DOWN", 1) + tap("DOWN") + wait(20)  # above May's Poke Ball
steps += tap("A", 40, hold=4, gap=26) + wait(60)                # May comes in
steps += tap("B", 40, hold=4, gap=26) + wait(60)
steps += walk("UP", 1) + walk("LEFT", 4) + walk("UP", 1) + wait(120)   # downstairs
steps += walk("DOWN", 7) + wait(120)                             # out of May's house
steps += walk("LEFT", 3) + walk("UP", 8) + wait(60)             # north exit, Birch needs help
steps += tap("B", 20, hold=4, gap=26) + wait(60)
steps += walk("UP", 3) + wait(60)                                # Route 101, Birch is chased
steps += tap("A", 24, hold=4, gap=26) + wait(60)                # Birch: "In my BAG!"
steps += walk("LEFT", 4) + tap("UP") + wait(10)                 # below Birch's bag
steps += tap("A", 58, hold=4, gap=26)                          # pick a starter and fight
steps += tap("B", 90, hold=4, gap=26)                          # experience, Birch, his lab, NO nickname
steps += tap("A", 30, hold=4, gap=26)                          # YES, go see May
steps += tap("B", 10, hold=4, gap=26) + wait(60)
steps += tap("START") + wait(30) + tap("DOWN", 3) + tap("A") + wait(90)   # SAVE, below POKEMON, BAG and the player
steps += tap("A") + wait(300) + tap("A") + wait(120)                      # YES
steps += wait(120) + tap("A") + wait(300) + tap("B") + wait(120)          # overwrite: YES
