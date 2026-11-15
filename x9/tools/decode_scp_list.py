#!/usr/bin/env python3
"""Decode buffered X9 ALS LIST mailbox evidence; no device access.

Usage: python3 x9/tools/decode_scp_list.py ~/N.txt > x9/out/list-N.jsonl
Raw words are the driver's LE32 rendering of the 28-byte sensor_comm_notify.
This checks the outer CRC8 only; CRC32 needs the complete shared payload,
not merely the shared-memory header printed in the log.
"""
import argparse
import json
from pathlib import Path
import re
import struct


RAW = re.compile(
    r"X9 ALS LIST: (OUT31|IN32\[\d+\]|IN32 pending).*?raw="
    r"((?:[0-9a-fA-F]{8}\s+){6}[0-9a-fA-F]{8})(?:\s|$)"
)


def crc8(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x8C if crc & 1 else 0)
    return crc


def decode(raw):
    result = {
        "hex": raw.hex(),
        "sequence": raw[0],
        "sensor_type": raw[1],
        "command": raw[2],
        "length": raw[3],
        "crc8_received": raw[4],
        "crc8_computed": crc8(raw[:4]),
        "crc8_ok": raw[4] == crc8(raw[:4]),
        "length_ok": raw[3] <= 20,
    }
    if raw[2] == 9 and raw[3] == 12:
        sequence, sensor_type, channel_command, sub_command, tx_len, rx_len = (
            struct.unpack_from("<BBBBII", raw, 8)
        )
        result["share_buffer_header"] = {
            # Android does not correlate this inner sequence; it uses the
            # outer sensor_comm_notify sequence. Do not conflate the two.
            "sequence": sequence,
            "sensor_type": sensor_type,
            "channel": channel_command & 0xF,
            "command": channel_command >> 4,
            "sub_command": sub_command,
            "tx_len": tx_len,
            "rx_len": rx_len,
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    with args.log.open(errors="replace") as source:
        for line_number, line in enumerate(source, 1):
            match = RAW.search(line)
            if not match:
                continue
            raw = struct.pack("<7I", *(int(word, 16) for word in match[2].split()))
            print(json.dumps({"line": line_number, "kind": match[1], **decode(raw)}))


if __name__ == "__main__":
    main()
