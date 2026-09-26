"""The game's maps, read from data/maps and data/layouts, for finding the way.

Each tile is one of:
  "#"  blocked
  "g"  tall grass, where wild Pokémon appear
  "~"  water, which needs Surf
  "<" ">" "^" "v"  a ledge, which can only be jumped down in that direction
  "."  anything else that can be walked on
"""

import heapq
import json
import os
import re
import struct

from headless import REPO_DIR

DIRECTIONS = {"UP": (0, -1), "DOWN": (0, 1), "LEFT": (-1, 0), "RIGHT": (1, 0)}
LEDGES = {"MB_JUMP_EAST": ">", "MB_JUMP_WEST": "<", "MB_JUMP_NORTH": "^", "MB_JUMP_SOUTH": "v"}
LEDGE_DIRECTIONS = {">": "RIGHT", "<": "LEFT", "^": "UP", "v": "DOWN"}


def _behavior_names():
    """Metatile behavior values to names, from their enum."""
    names = []
    with open(os.path.join(REPO_DIR, "include", "constants", "metatile_behaviors.h")) as f:
        for line in f:
            match = re.match(r"\s*(MB_\w+)", line)
            if match:
                names.append(match.group(1))
    return names


_BEHAVIORS = _behavior_names()
_LAYOUTS = {layout["id"]: layout
            for layout in json.load(open(os.path.join(REPO_DIR, "data", "layouts", "layouts.json")))["layouts"]
            if "id" in layout}
_GROUPS = json.load(open(os.path.join(REPO_DIR, "data", "maps", "map_groups.json")))
# Tileset names to their metatile attribute files, through the Tileset structs
_ATTRIBUTE_FILES = {}
with open(os.path.join(REPO_DIR, "src", "data", "tilesets", "metatiles.h")) as f:
    for line in f:
        match = re.search(r'(gMetatileAttributes_\w+)\[\] = INCBIN_U16\("([^"]+)"\)', line)
        if match:
            _ATTRIBUTE_FILES[match.group(1)] = match.group(2)
_ATTRIBUTES = {}
with open(os.path.join(REPO_DIR, "src", "data", "tilesets", "headers.h")) as f:
    for match in re.finditer(r"struct Tileset (gTileset_\w+) =\s*\{(.*?)\};", f.read(), re.S):
        attributes = re.search(r"\.metatileAttributes = (\w+)", match.group(2))
        if attributes:
            _ATTRIBUTES[match.group(1)] = _ATTRIBUTE_FILES[attributes.group(1)]


def map_name(group, num):
    """The name of a map, like "LittlerootTown", from its group and number."""
    return _GROUPS[_GROUPS["group_order"][group]][num]


def _tile_kind(behavior):
    name = _BEHAVIORS[behavior] if behavior < len(_BEHAVIORS) else ""
    if name in ("MB_TALL_GRASS", "MB_LONG_GRASS"):
        return "g"
    if name in LEDGES:
        return LEDGES[name]
    if re.search(r"DEEP_WATER|POND_WATER|OCEAN_WATER|WATERFALL|CURRENT", name):
        return "~"
    return "."


class Map:
    def __init__(self, name):
        self.name = name
        info = json.load(open(os.path.join(REPO_DIR, "data", "maps", name, "map.json")))
        layout = _LAYOUTS[info["layout"]]
        self.width, self.height = layout["width"], layout["height"]
        behaviors = []
        for tileset in (layout["primary_tileset"], layout["secondary_tileset"]):
            with open(os.path.join(REPO_DIR, _ATTRIBUTES[tileset]), "rb") as f:
                data = f.read()
            values = [struct.unpack_from("<H", data, i)[0] & 0xFF for i in range(0, len(data), 2)]
            # The primary tileset has the first 512 metatiles
            behaviors += values + [0] * (512 - len(values)) if not behaviors else values
        with open(os.path.join(REPO_DIR, layout["blockdata_filepath"]), "rb") as f:
            blocks = f.read()
        self.tiles = []
        for y in range(self.height):
            row = ""
            for x in range(self.width):
                block = struct.unpack_from("<H", blocks, (y * self.width + x) * 2)[0]
                metatile = block & 0x3FF
                row += "#" if block & 0xC00 else _tile_kind(behaviors[metatile] if metatile < len(behaviors) else 0)
            self.tiles.append(row)
        self.warps = {(warp["x"], warp["y"]): warp["dest_map"] for warp in info.get("warp_events", [])}

    def tile(self, x, y):
        if 0 <= x < self.width and 0 <= y < self.height:
            return self.tiles[y][x]
        return "#"

    def path(self, start, goal, blocked=(), grass_cost=20):
        """The shortest way from start to goal as a list of directions, with each
        tile of grass counting as grass_cost tiles. Warps and the blocked tiles are
        avoided, apart from the goal: a door counts as blocked on the map, but
        walking into it goes through. Returns None if there's no way."""
        blocked = set(blocked) - {goal}
        best = {start: 0}
        queue = [(0, start, [])]
        while queue:
            cost, (x, y), directions = heapq.heappop(queue)
            if (x, y) == goal:
                return directions
            if cost > best.get((x, y), cost):
                continue
            for direction, (dx, dy) in DIRECTIONS.items():
                nx, ny = x + dx, y + dy
                kind = self.tile(nx, ny)
                if kind in LEDGE_DIRECTIONS:
                    # A ledge can only be jumped down, over to the tile past it
                    if LEDGE_DIRECTIONS[kind] != direction:
                        continue
                    nx, ny = nx + dx, ny + dy
                    kind = self.tile(nx, ny)
                if (nx, ny) in self.warps:
                    if (nx, ny) != goal:
                        continue
                elif kind in "#~" or (nx, ny) in blocked:
                    continue
                step = cost + 1 + (grass_cost if kind == "g" else 0) + (0 if directions[-1:] == [direction] else 1)
                if step < best.get((nx, ny), float("inf")):
                    best[(nx, ny)] = step
                    heapq.heappush(queue, (step, (nx, ny), directions + [direction]))
        return None


_maps = {}


def load(name):
    if name not in _maps:
        _maps[name] = Map(name)
    return _maps[name]
