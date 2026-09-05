#!/usr/bin/env python3
"""Extract a sound entry from a PopCap main.pak archive."""

import argparse
import pathlib
import struct


SOUND_NAME = "sounds/peghit.ogg"
XOR_KEY = 0xF7


def extract(pak_path: pathlib.Path, output_path: pathlib.Path) -> None:
    data = bytearray(pak_path.read_bytes())
    for index in range(len(data)):
        data[index] ^= XOR_KEY

    if data[:4] != bytes.fromhex("c04ac0ba"):
        raise SystemExit(f"not a PopCap PAK archive: {pak_path}")

    offset = 8
    records: list[tuple[str, int]] = []
    while offset < len(data):
        flag = data[offset]
        offset += 1
        if flag == 0x80:
            break
        if offset >= len(data):
            raise SystemExit("truncated PAK record")
        name_length = data[offset]
        offset += 1
        record_end = offset + name_length + 12
        if record_end > len(data):
            raise SystemExit("truncated PAK record")
        name = bytes(data[offset : offset + name_length]).decode("utf-8")
        offset += name_length
        size = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        offset += 8  # Microsoft FILETIME timestamp
        records.append((name.replace("\\", "/"), size))

    data_offset = offset
    for name, size in records:
        if data_offset + size > len(data):
            raise SystemExit("truncated PAK file data")
        if name.casefold() == SOUND_NAME.casefold():
            output_path.parent.mkdir(parents=True, exist_ok=True)
            output_path.write_bytes(data[data_offset : data_offset + size])
            return
        data_offset += size

    raise SystemExit(f"sound entry not found: {SOUND_NAME}")


parser = argparse.ArgumentParser(description="Extract Peggle's peg-hit sound from main.pak.")
parser.add_argument("source", type=pathlib.Path, help="Peggle Deluxe.app or its main.pak file")
parser.add_argument("--output", type=pathlib.Path, required=True, help="output sound path")
args = parser.parse_args()

source = args.source.expanduser()
pak = source if source.name == "main.pak" else source / "Contents/Resources/main.pak"
if not pak.is_file():
    raise SystemExit(f"main.pak not found: {pak}")
extract(pak, args.output.expanduser())
