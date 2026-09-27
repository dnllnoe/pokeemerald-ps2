# Continues the save made by first_battle and follows the story: heals at
# Oldale's Pokémon Center, battles the rival on Route 103, gets the Pokédex in
# Birch's lab and the Running Shoes from Mom, and saves. Wild Pokémon in the
# grass on the way get fought too.
from driver import continue_game, finish_dialog, leave_map, save_game, talk_to, walk_to


def heal_in_oldale(game):
    """From anywhere in Oldale Town, to the Pokémon Center's nurse and back out."""
    walk_to(game, 6, 16)                # the Pokémon Center's door
    walk_to(game, 7, 4)
    talk_to(game, "UP")                 # the nurse heals the party
    walk_to(game, 7, 7)
    leave_map(game, "DOWN")             # Oldale Town


def play(game):
    continue_game(game)                 # in Birch's lab
    walk_to(game, 6, 11)
    leave_map(game, "DOWN")             # Littleroot Town
    walk_to(game, 11, 0)
    leave_map(game, "UP")               # Route 101
    walk_to(game, 11, 0)
    leave_map(game, "UP")               # Oldale Town
    heal_in_oldale(game)
    walk_to(game, 10, 0)
    leave_map(game, "UP")               # Route 103
    walk_to(game, 10, 4)                # below the rival
    talk_to(game, "UP")                 # battle, and she goes back to the lab

    walk_to(game, 10, 21)
    leave_map(game, "DOWN")             # Oldale Town, where she stops us once more
    heal_in_oldale(game)
    walk_to(game, 11, 19)
    leave_map(game, "DOWN")             # Route 101
    walk_to(game, 11, 19)
    leave_map(game, "DOWN")             # Littleroot Town
    walk_to(game, 7, 16)                # Birch's lab: the Pokédex
    finish_dialog(game)
    walk_to(game, 6, 11)
    leave_map(game, "DOWN")             # Littleroot Town
    walk_to(game, 11, 5)                # Mom stops us on the way
    finish_dialog(game)
    save_game(game, 4)                  # below POKéDEX, POKéMON, BAG and the player
