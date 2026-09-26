# Plays first_battle with the keyboard and a controller instead of the GBA
# buttons, which has to give the same frames. Each press takes the next of the
# inputs for its button, so all of them get used. Parameter: REMAPPED, to use
# the bindings in controls.ini instead of the default ones.
if REMAPPED:
    CONFIG = "controls.ini"
    inputs = {"A": ["key:K", "pad:y"], "B": ["key:J", "pad:rightshoulder"], "START": ["key:Q", "pad:back"],
              "UP": ["key:W", "pad:lefty-", "pad:dpup"], "DOWN": ["key:S", "pad:righty+"],
              "LEFT": ["key:A", "pad:lefttrigger"], "RIGHT": ["key:D", "pad:dpright"]}
else:
    inputs = {"A": ["key:Z", "pad:a"], "B": ["key:X", "pad:b", "pad:x"], "START": ["key:Return", "pad:start"],
              "UP": ["key:Up", "pad:dpup", "pad:lefty-"], "DOWN": ["key:Down", "pad:dpdown", "pad:lefty+"],
              "LEFT": ["key:Left", "pad:dpleft", "pad:leftx-"], "RIGHT": ["key:Right", "pad:dpright", "pad:leftx+"]}

presses = 0
steps = []
for frames, buttons in load_scenario("first_battle"):
    if buttons != "-":
        buttons = "+".join(inputs[button][presses % len(inputs[button])] for button in buttons.split("+"))
        presses += 1
    steps.append((frames, buttons))
