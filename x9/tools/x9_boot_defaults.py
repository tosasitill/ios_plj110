"""Board defaults of sweet_x9_arg(), read from the kernel source.

The kernel table in kernel/src/pexpert/arm/pe_serial.c (between the
X9_BOARD_DEFAULTS markers) is the only copy. Packaging decisions that depend
on a boot arg (ADT nodes, required firmware) call value() so an image built
without the explicit token still matches what the kernel will do.
"""
from pathlib import Path
import re

_SRC = Path(__file__).resolve().parents[1] / "kernel/src/pexpert/arm/pe_serial.c"
_ENTRY = re.compile(r"\{\s*\"([a-z0-9_]+)\"\s*,\s*(-?(?:0x[0-9a-fA-F]+|\d+))\s*\}")


def board_defaults(src=_SRC):
    text = src.read_text()
    try:
        body = text.split("X9_BOARD_DEFAULTS_BEGIN", 1)[1].split("X9_BOARD_DEFAULTS_END", 1)[0]
    except IndexError:
        raise SystemExit(f"{src}: X9_BOARD_DEFAULTS markers missing")
    table = {name: int(num, 0) for name, num in _ENTRY.findall(body)}
    if not table:
        raise SystemExit(f"{src}: empty X9_BOARD_DEFAULTS table")
    return table


def value(args, name, default=0):
    """First exact name=value token, else the kernel board default, else default."""
    for arg in args:
        key, sep, text = arg.partition("=")
        if key == name:
            if not sep:
                raise SystemExit(f"{name} needs an integer value")
            try:
                return int(text, 0)
            except ValueError:
                raise SystemExit(f"invalid boot arg: {arg!r}")
    return board_defaults().get(name, default)
