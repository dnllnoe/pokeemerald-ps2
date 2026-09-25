#!/usr/bin/env python3
"""Checks all map and sound data of a running game against how the C code reads it.

Walks every map (layout, tilesets, events, warps, map scripts, connections) and
every song (tracks, voicegroups, samples) through gdb, and reports pointers that
don't point into the executable, counts or values out of range, and structs that
aren't aligned. It covers data the scenarios never load, which is how
mismatches between the assembled data and the C structs show up.

Needs Linux, gdb and a build with debug info (make linux CFLAGS=-g).

usage: audit_data.py BINARY
"""

import os
import re
import struct
import subprocess
import sys
import tempfile

try:
    import gdb
except ImportError:
    gdb = None


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    binary = os.path.abspath(sys.argv[1])
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    with tempfile.TemporaryDirectory() as tmp:
        input_path = os.path.join(tmp, "input.txt")
        with open(input_path, "w") as f:
            f.write("1 -\n")
        cmd = ["gdb", "-batch", "-nx", "-ex", "set pagination off", "-ex", "set debuginfod enabled off",
               "-ex", "break AgbMain", "-ex", "run",
               "-x", os.path.abspath(__file__),
               "--args", binary, "--save", os.path.join(tmp, "game.sav"), "--test-input", input_path]
        proc = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, AUDIT_REPO=repo))

    report = [line for line in proc.stdout.splitlines() if line.startswith("AUDIT")]
    print("\n".join(report))
    result = [line for line in report if line.startswith("AUDIT problems:")]
    if not result:
        print("The audit didn't run (is gdb installed and does the binary have debug info?)")
        print(proc.stderr.strip()[-2000:])
        sys.exit(1)
    sys.exit(0 if result[0] == "AUDIT problems: 0" else 1)


def audit():
    repo = os.environ["AUDIT_REPO"]
    inferior = gdb.selected_inferior()
    exe = os.path.realpath(gdb.current_progspace().filename)
    ranges = []
    with open(f"/proc/{inferior.pid}/maps") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 6 and os.path.realpath(parts[5]) == exe:
                lo, hi = (int(x, 16) for x in parts[0].split("-"))
                ranges.append((lo, hi))

    problems, stats, aligned = [], {}, []  # aligned: (address, alignment, what)

    def count(what):
        stats[what] = stats.get(what, 0) + 1

    def check_ptr(value, what, null_ok=True):
        value = int(value)
        if value == 0:
            if not null_ok:
                problems.append(f"{what}: NULL")
            return False
        if not any(lo <= value < hi for lo, hi in ranges):
            problems.append(f"{what}: pointer {value:#x} outside the executable")
            return False
        return True

    def mem(address, size):
        return bytes(inferior.read_memory(address, size))

    def u8(address):
        return mem(address, 1)[0]

    def u16(address):
        return struct.unpack("<H", mem(address, 2))[0]

    def ptr(address):
        return struct.unpack("<Q", mem(address, 8))[0]

    def ev(expr):
        return gdb.parse_and_eval(expr)

    def address_of(symbol):
        return int(ev(f"(unsigned long)&{symbol}"))

    # ---- maps, listed in the generated data/maps/groups.inc ----
    groups, current = [], None
    with open(f"{repo}/data/maps/groups.inc") as f:
        for line in f:
            if line.startswith("gMapGroups::"):
                current = None
            elif m := re.match(r"(gMapGroup_\w+)::", line):
                current = []
                groups.append((m.group(1), current))
            elif (m := re.match(r"\s*ptrvalue\s+(\w+)", line)) and current is not None:
                current.append(m.group(1))
    valid_maps = {(g, n) for g, (_, names) in enumerate(groups) for n in range(len(names))}

    table = address_of("gMapGroups")
    for g, (label, names) in enumerate(groups):
        group = ptr(table + 8 * g)
        if group != address_of(label):
            problems.append(f"gMapGroups[{g}] isn't {label}")
        for n, name in enumerate(names):
            if ptr(group + 8 * n) != address_of(name):
                problems.append(f"{label}[{n}] isn't {name}")

    seen_layouts, seen_tilesets = set(), set()
    for g, (label, names) in enumerate(groups):
        for n, name in enumerate(names):
            count("maps")
            where = f"map {name} ({g}.{n})"
            header = ev(f"*(const struct MapHeader *)&{name}")
            aligned.append((address_of(name), 8, f"{where} header"))

            layout = header["mapLayout"]
            if check_ptr(layout, f"{where}.mapLayout", null_ok=False) and int(layout) not in seen_layouts:
                seen_layouts.add(int(layout))
                count("layouts")
                aligned.append((int(layout), 8, f"{where} layout"))
                L = layout.dereference()
                if not (1 <= int(L["width"]) <= 512 and 1 <= int(L["height"]) <= 512):
                    problems.append(f"{where} layout size {int(L['width'])}x{int(L['height'])}")
                check_ptr(L["border"], f"{where}.layout.border", False)
                check_ptr(L["map"], f"{where}.layout.map", False)
                for field in ("primaryTileset", "secondaryTileset"):
                    tileset = L[field]
                    if check_ptr(tileset, f"{where}.layout.{field}", False) and int(tileset) not in seen_tilesets:
                        seen_tilesets.add(int(tileset))
                        count("tilesets")
                        T = tileset.dereference()
                        for part in ("tiles", "palettes", "metatiles", "metatileAttributes"):
                            check_ptr(T[part], f"{where}.{field}.{part}", False)
                        check_ptr(T["callback"], f"{where}.{field}.callback")

            events = header["events"]
            if check_ptr(events, f"{where}.events"):
                E = events.dereference()
                aligned.append((int(events), 8, f"{where}.events"))
                for array in ("objectEvents", "warps", "coordEvents", "bgEvents"):
                    if int(E[array]):
                        aligned.append((int(E[array]), 8, f"{where}.{array}"))
                objects, warps, coords, bgs = (int(E[k]) for k in
                                               ("objectEventCount", "warpCount", "coordEventCount", "bgEventCount"))
                if objects > 64:
                    problems.append(f"{where} objectEventCount {objects}")
                if objects:
                    check_ptr(E["objectEvents"], f"{where}.objectEvents", False)
                for i in range(min(objects, 64)):
                    count("object events")
                    obj = E["objectEvents"][i]
                    check_ptr(obj["script"], f"{where}.objectEvents[{i}].script")
                    if int(obj["kind"]) != 0:
                        problems.append(f"{where}.objectEvents[{i}].kind {int(obj['kind'])}")
                if warps:
                    check_ptr(E["warps"], f"{where}.warps", False)
                for i in range(warps):
                    count("warps")
                    dest = (int(E["warps"][i]["mapGroup"]), int(E["warps"][i]["mapNum"]))
                    if dest not in valid_maps and dest[1] != 127:  # MAP_DYNAMIC
                        problems.append(f"{where}.warps[{i}] goes to {dest}")
                if coords:
                    check_ptr(E["coordEvents"], f"{where}.coordEvents", False)
                for i in range(coords):
                    count("coord events")
                    check_ptr(E["coordEvents"][i]["script"], f"{where}.coordEvents[{i}].script")
                if bgs:
                    check_ptr(E["bgEvents"], f"{where}.bgEvents", False)
                for i in range(bgs):
                    count("bg events")
                    bg = E["bgEvents"][i]
                    kind = int(bg["kind"])
                    if kind == 7:  # BG_EVENT_HIDDEN_ITEM
                        item = int(bg["bgUnion"]["hiddenItem"]["item"])
                        if not 0 < item < 400:
                            problems.append(f"{where}.bgEvents[{i}] hidden item {item}")
                    elif kind <= 4:
                        check_ptr(bg["bgUnion"]["script"], f"{where}.bgEvents[{i}].script")
                    elif kind != 8:  # BG_EVENT_SECRET_BASE
                        problems.append(f"{where}.bgEvents[{i}] kind {kind}")

            # Read like MapHeaderGetScriptTable and MapHeaderCheckScriptTable
            scripts = int(header["mapScripts"])
            if check_ptr(scripts, f"{where}.mapScripts"):
                while u8(scripts) != 0:
                    count("map scripts")
                    tag, target = u8(scripts), ptr(scripts + 1)
                    if not 1 <= tag <= 7:
                        problems.append(f"{where} map script type {tag}")
                        break
                    if check_ptr(target, f"{where} map script {tag}", False) and tag in (2, 4):
                        while u16(target) != 0:
                            count("map script table entries")
                            check_ptr(ptr(target + 4), f"{where} map script {tag} table entry", False)
                            target += 12
                    scripts += 9

            connections = header["connections"]
            if check_ptr(connections, f"{where}.connections"):
                C = connections.dereference()
                aligned.append((int(connections), 8, f"{where}.connections"))
                total = int(C["count"])
                if not 0 <= total <= 6:
                    problems.append(f"{where} connection count {total}")
                    total = 0
                if total:
                    check_ptr(C["connections"], f"{where}.connections.connections", False)
                for i in range(total):
                    count("connections")
                    k = C["connections"][i]
                    dest = (int(k["mapGroup"]), int(k["mapNum"]))
                    offset = int(k["offset"])
                    offset = offset - (1 << 64) if offset >= 1 << 63 else offset
                    if not 1 <= int(k["direction"]) <= 6:
                        problems.append(f"{where}.connections[{i}] direction {int(k['direction'])}")
                    if dest not in valid_maps:
                        problems.append(f"{where}.connections[{i}] goes to {dest}")
                    if abs(offset) > 512:
                        problems.append(f"{where}.connections[{i}] offset {offset}")

    # ---- sound: songs from sound/song_table.inc, voicegroups as declared ----
    tone_size = int(ev("sizeof(struct ToneData)"))
    song_size = int(ev("sizeof(struct Song)"))
    with open(f"{repo}/sound/song_table.inc") as f:
        song_count = sum(1 for line in f if re.match(r"\s*song\s", line))

    # voice_group NAME[, starting note], followed by its voices
    declared = {}
    for root, _, files in os.walk(f"{repo}/sound/voicegroups"):
        for file in files:
            name = None
            with open(os.path.join(root, file)) as f:
                for line in f:
                    if m := re.match(r"\s*voice_group\s+(\w+)\s*(?:,\s*(\d+))?", line):
                        name = m.group(1)
                        declared[name] = [int(m.group(2) or 0), 0]
                    elif name and re.match(r"\s*voice_\w+", line):
                        declared[name][1] += 1
    voicegroup_ranges = {}
    for name, (start, voices) in declared.items():
        try:
            voicegroup_ranges[address_of(f"voicegroup_{name}")] = (start, voices, name)
        except gdb.error:
            problems.append(f"voicegroup_{name} not found")

    seen_voicegroups, seen_waves = set(), set()

    def check_wave(address, where):
        if address in seen_waves:
            return
        seen_waves.add(address)
        count("wave samples")
        aligned.append((address, 4, f"{where} sample"))
        loop_start, size = struct.unpack("<II", mem(address + 8, 8))  # after type, status, freq
        if size > 1 << 22 or loop_start > size:
            problems.append(f"{where}: sample size {size}, loop start {loop_start}")

    def check_voicegroup(address, where, depth=0):
        if address in seen_voicegroups or depth > 3:
            return
        seen_voicegroups.add(address)
        count("voicegroups")
        aligned.append((address, 8, f"{where} voicegroup"))
        if address not in voicegroup_ranges:
            problems.append(f"{where}: voicegroup {address:#x} isn't a declared voice_group")
            return
        start, voices, name = voicegroup_ranges[address]
        for i in range(start, start + voices):
            tone = address + i * tone_size
            kind, wav = u8(tone), ptr(tone + 8)
            voice = f"{where} ({name}) voice {i}"
            if kind & 0xC0:  # key split or rhythm: wav is another voicegroup
                if check_ptr(wav, f"{voice} voicegroup", False):
                    check_voicegroup(wav, f"{where}>{i}", depth + 1)
                if kind & 0x40:
                    check_ptr(ptr(tone + 16), f"{voice} key split table", False)
            elif kind & 0x07 == 0:  # DirectSound
                if wav and check_ptr(wav, f"{voice} sample"):
                    check_wave(wav, voice)
            elif kind & 0x07 == 3:  # programmable wave
                check_ptr(wav, f"{voice} wave", False)
            elif kind & 0x07 not in (1, 2, 4):
                problems.append(f"{voice} type {kind:#x}")

    songs = address_of("gSongTable")
    for i in range(song_count):
        count("songs")
        header = ptr(songs + i * song_size)
        if not check_ptr(header, f"song {i} header", False):
            continue
        aligned.append((header, 8, f"song {i} header"))
        tracks, voicegroup = u8(header), ptr(header + 8)
        if tracks > 16:
            problems.append(f"song {i} trackCount {tracks}")
            continue
        for track in range(tracks):
            check_ptr(ptr(header + 16 + 8 * track), f"song {i} track {track}", False)
        if tracks and check_ptr(voicegroup, f"song {i} voicegroup", False):
            check_voicegroup(voicegroup, f"song {i}")

    problems += [f"{what} isn't {alignment}-byte aligned" for address, alignment, what in aligned
                 if address % alignment]
    print("AUDIT checked:", ", ".join(f"{n} {what}" for what, n in stats.items()))
    print(f"AUDIT alignment checked for {len(aligned)} structs")
    print(f"AUDIT problems: {len(problems)}")
    for problem in problems[:100]:
        print("AUDIT - " + problem)


if gdb is None:
    main()
else:
    try:
        audit()
    except gdb.error as e:
        print(f"AUDIT error: {e}")
