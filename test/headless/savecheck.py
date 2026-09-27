#!/usr/bin/env python3
"""Checks that a save file is in the GBA format and reads a few values from it.

This is independent of the game code: it uses the Gen 3 sector format and the
GBA offsets documented in include/global.h, so a save that passes can be used
by the GBA game and by 32-bit and 64-bit PC builds.

usage: savecheck.py SAVE [--expect-map G.N] [--expect-money N] [--expect-counter N]
                         [--expect-name NAME] [--expect-party N]
"""

import argparse
import struct
import sys

SECTOR_SIZE = 0x1000
SECTOR_DATA_SIZE = 3968
SECTOR_SIGNATURE = 0x08012025
SECTORS_PER_SLOT = 14
SAVEBLOCK2_SIZE = 0xF2C
SAVEBLOCK1_SIZE = 0x3D88
POKEMON_STORAGE_SIZE = 0x83D0


def chunk_sizes():
    sizes = [SAVEBLOCK2_SIZE]
    for total, count in ((SAVEBLOCK1_SIZE, 4), (POKEMON_STORAGE_SIZE, 9)):
        sizes += [max(0, min(total - i * SECTOR_DATA_SIZE, SECTOR_DATA_SIZE)) for i in range(count)]
    return sizes


def checksum(data, size):
    total = sum(struct.unpack_from(f"<{size // 4}I", data)) & 0xFFFFFFFF
    return ((total >> 16) + (total & 0xFFFF)) & 0xFFFF


def decode_text(data):
    """Decodes Gen 3 text (letters, digits and spaces only)."""
    out = []
    for byte in data:
        if byte == 0xFF:
            break
        if 0xBB <= byte <= 0xD4:
            out.append(chr(ord("A") + byte - 0xBB))
        elif 0xD5 <= byte <= 0xEE:
            out.append(chr(ord("a") + byte - 0xD5))
        elif 0xA1 <= byte <= 0xAA:
            out.append(chr(ord("0") + byte - 0xA1))
        elif byte == 0x00:
            out.append(" ")
        else:
            out.append("?")
    return "".join(out)


class SaveError(Exception):
    pass


class Save:
    """The newest valid save slot of a save file."""

    def __init__(self, path):
        with open(path, "rb") as f:
            data = f.read()
        sizes = chunk_sizes()
        best = None
        for slot in range(2):
            sectors, counter = {}, None
            for i in range(SECTORS_PER_SLOT):
                sector = data[(slot * SECTORS_PER_SLOT + i) * SECTOR_SIZE:][:SECTOR_SIZE]
                if len(sector) < SECTOR_SIZE:
                    break
                sector_id, chk, signature, counter = struct.unpack_from("<HHII", sector, 0xFF4)
                if signature != SECTOR_SIGNATURE or sector_id >= SECTORS_PER_SLOT:
                    break
                if chk != checksum(sector, sizes[sector_id]):
                    break
                sectors[sector_id] = sector[:SECTOR_DATA_SIZE]
            if len(sectors) == SECTORS_PER_SLOT and (best is None or counter > best[0]):
                best = (counter, slot, sectors)
        if best is None:
            raise SaveError(f"{path}: no save slot is valid with the GBA sector sizes")

        self.counter, self.slot, sectors = best
        self.saveblock2 = sectors[0][:sizes[0]]
        self.saveblock1 = b"".join(sectors[i][:sizes[i]] for i in range(1, 5))

        sb1, sb2 = self.saveblock1, self.saveblock2
        key = struct.unpack_from("<I", sb2, 0xAC)[0]
        self.name = decode_text(sb2[0x00:0x08])
        self.trainer_id = struct.unpack_from("<I", sb2, 0x0A)[0] & 0xFFFF
        self.play_time = struct.unpack_from("<HBB", sb2, 0x0E)
        self.position = struct.unpack_from("<hh", sb1, 0x00)
        self.map = struct.unpack_from("<bb", sb1, 0x04)
        self.party_count = sb1[0x234]
        self.money = struct.unpack_from("<I", sb1, 0x490)[0] ^ key
        # The in-game clock is the RTC minus this offset, in days, hours, minutes
        # and seconds. Setting the clock takes the day from the RTC, which counts
        # from 2000-01-01 as day 1.
        self.clock_offset = struct.unpack_from("<hbbb", sb2, 0x98)

    def summary(self):
        hours, minutes, seconds = self.play_time
        return (f"slot {self.slot + 1}, save counter {self.counter}: {self.name} (TID {self.trainer_id}), "
                f"map {self.map[0]}.{self.map[1]} at {self.position}, money {self.money}, "
                f"party {self.party_count}, time {hours}:{minutes:02}:{seconds:02}")


def check(path, expect_map=None, expect_money=None, expect_counter=None, expect_name=None, expect_party=None,
          expect_position=None, expect_clock_days=None):
    """Returns (save, list of problems)."""
    try:
        save = Save(path)
    except (OSError, SaveError) as e:
        return None, [str(e)]
    problems = []
    if expect_map is not None and "%d.%d" % save.map != expect_map:
        problems.append(f"map is {save.map[0]}.{save.map[1]}, expected {expect_map}")
    if expect_money is not None and save.money != expect_money:
        problems.append(f"money is {save.money}, expected {expect_money}")
    if expect_counter is not None and save.counter != expect_counter:
        problems.append(f"save counter is {save.counter}, expected {expect_counter}")
    if expect_name is not None and save.name != expect_name:
        problems.append(f"name is {save.name!r}, expected {expect_name!r}")
    if expect_party is not None and save.party_count != expect_party:
        problems.append(f"party has {save.party_count}, expected {expect_party}")
    if expect_position is not None and save.position != expect_position:
        problems.append(f"position is {save.position}, expected {expect_position}")
    if expect_clock_days is not None and save.clock_offset[0] != expect_clock_days:
        problems.append(f"the clock was set on day {save.clock_offset[0]} of the RTC, expected {expect_clock_days}")
    return save, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("save")
    parser.add_argument("--expect-map", help="map group and number, like 1.1")
    parser.add_argument("--expect-money", type=int)
    parser.add_argument("--expect-counter", type=int)
    parser.add_argument("--expect-name")
    parser.add_argument("--expect-party", type=int)
    args = parser.parse_args()

    save, problems = check(args.save, args.expect_map, args.expect_money, args.expect_counter,
                           args.expect_name, args.expect_party)
    if save:
        print(save.summary())
    for problem in problems:
        print("FAIL:", problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
