#!/usr/bin/env python3
"""Generate MAME-derived System 11 control bindings for the native board."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PINNED_COMMIT = "deb2f54fe7300e7a192690318beb860159bfd52a"
DEFAULT_SOURCE = ROOT.parent / "mamedev/src/mame/namco/namcos11.cpp"
DEFAULT_PROFILES = ROOT / "scripts/system11_boards.json"
DEFAULT_OUTPUT = ROOT / "libpcsxcore/arcade/system11_inputs.inc"


class InputSourceError(ValueError):
    pass


@dataclass
class Control:
    control: str
    mask: int
    player: int | None = None
    analog: bool = False


@dataclass
class Port:
    name: str
    width: int = 8
    value: int = 0
    controls: list[Control] = field(default_factory=list)


def input_blocks(source: str) -> dict[str, str]:
    blocks = {}
    pattern = re.compile(r"static\s+INPUT_PORTS_START\s*\(\s*(\w+)\s*\)(.*?)INPUT_PORTS_END", re.S)
    for match in pattern.finditer(source):
        name = match.group(1)
        if name in blocks:
            raise InputSourceError(f"duplicate INPUT_PORTS_START({name})")
        blocks[name] = match.group(2)
    if not blocks:
        raise InputSourceError("no INPUT_PORTS_START blocks found")
    return blocks


def _args(text: str) -> list[str]:
    result, part, depth, quoted, escaped = [], [], 0, False, False
    for char in text:
        if quoted:
            part.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
            part.append(char)
        elif char == "(":
            depth += 1
            part.append(char)
        elif char == ")":
            depth -= 1
            part.append(char)
        elif char == "," and depth == 0:
            result.append("".join(part).strip())
            part = []
        else:
            part.append(char)
    result.append("".join(part).strip())
    return result


def _integer(value: str) -> int:
    value = value.strip().replace("'", "")
    if not re.fullmatch(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)", value):
        raise InputSourceError(f"unsupported numeric literal {value!r}")
    return int(value, 0)


def _parse_call(line: str, macro: str) -> list[str] | None:
    match = re.search(rf"\b{macro}\s*\(", line)
    if not match:
        return None
    start = match.end()
    depth, quoted, escaped = 1, False, False
    for position in range(start, len(line)):
        char = line[position]
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return _args(line[start:position])
    raise InputSourceError(f"unterminated {macro} declaration")


def _control_name(port: str, kind: str, player: int | None) -> str | None:
    if kind.startswith("IPT_COIN") and kind[-1:] in "1234":
        return "coin" + kind[-1]
    if kind == "IPT_SERVICE1":
        return "service"
    if kind in {"IPT_JOYSTICK_RIGHT", "IPT_JOYSTICK_LEFT", "IPT_JOYSTICK_DOWN", "IPT_JOYSTICK_UP"}:
        if port.startswith("PLAYER"):
            player = int(port[-1]) if player is None else player
        if port.startswith("ADC"):
            player = 1 if player is None else player
        if player in (1, 2, 3, 4):
            return f"p{player}" + kind.removeprefix("IPT_JOYSTICK_").lower()
    if kind.startswith("IPT_BUTTON") and kind[-1:] in "12345678":
        if port.startswith("PLAYER"):
            player = int(port[-1]) if player is None else player
        if port.startswith("ADC"):
            player = 1 if player is None else player
        if player in (1, 2, 3, 4):
            return f"p{player}button{kind[-1]}"
    if kind.startswith("IPT_START") and kind[-1:] in "1234":
        return "p" + kind[-1] + "start"
    return None


def _expand(name: str, blocks: dict[str, str], cache: dict[str, dict[str, Port]], stack=()) -> dict[str, Port]:
    if name in cache:
        return {key: Port(p.name, p.width, p.value, list(p.controls)) for key, p in cache[name].items()}
    if name in stack:
        raise InputSourceError("cyclic PORT_INCLUDE: " + " -> ".join((*stack, name)))
    try:
        body = blocks[name]
    except KeyError as exc:
        raise InputSourceError(f"unknown included input profile {name!r}") from exc
    ports: dict[str, Port] = {}
    current: Port | None = None
    modifying = False
    lines = body.splitlines()
    for line_number, raw in enumerate(lines, 1):
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue
        include = _parse_call(line, "PORT_INCLUDE")
        if include:
            if len(include) != 1 or not re.fullmatch(r"\w+", include[0]):
                raise InputSourceError(f"{name}:{line_number}: unsupported PORT_INCLUDE")
            included = _expand(include[0], blocks, cache, (*stack, name))
            ports.update(included)
            current = None
            modifying = False
            continue
        modify = _parse_call(line, "PORT_MODIFY")
        if modify:
            if len(modify) != 1:
                raise InputSourceError(f"{name}:{line_number}: malformed PORT_MODIFY")
            port_name = modify[0].strip('"')
            if port_name not in ports:
                raise InputSourceError(f"{name}:{line_number}: PORT_MODIFY of unknown port {port_name}")
            current, modifying = ports[port_name], True
            continue
        start = _parse_call(line, "PORT_START")
        if start:
            if len(start) != 1:
                raise InputSourceError(f"{name}:{line_number}: malformed PORT_START")
            port_name = start[0].strip('"')
            current = Port(port_name, width=16 if port_name.startswith("GUN") else 8)
            ports[port_name] = current
            modifying = False
            continue
        if not re.match(r"PORT_", line):
            raise InputSourceError(f"{name}:{line_number}: unrecognized input declaration {line!r}")
        if line.startswith("PORT_BIT("):
            if current is None:
                raise InputSourceError(f"{name}:{line_number}: PORT_BIT outside a port")
            args = _parse_call(line, "PORT_BIT")
            if args is None or len(args) < 3:
                raise InputSourceError(f"{name}:{line_number}: malformed PORT_BIT")
            mask, active, kind = _integer(args[0]), args[1], args[2]
            if active not in {"IP_ACTIVE_LOW", "IP_ACTIVE_HIGH"}:
                try:
                    default = _integer(active)
                except InputSourceError as exc:
                    raise InputSourceError(f"{name}:{line_number}: unsupported polarity/default {active!r}") from exc
            else:
                default = mask if active == "IP_ACTIVE_LOW" else 0
            player_match = re.search(r"\bPORT_PLAYER\s*\(\s*(\d+)\s*\)", line)
            player = int(player_match.group(1)) if player_match else None
            if kind in {"IPT_PADDLE", "IPT_PEDAL", "IPT_LIGHTGUN_X", "IPT_LIGHTGUN_Y"} and len(args) >= 2 and active not in {"IP_ACTIVE_LOW", "IP_ACTIVE_HIGH"}:
                default = _integer(active)
            if mask == 0 or mask & ~((1 << current.width) - 1):
                raise InputSourceError(f"{name}:{line_number}: mask outside {current.width}-bit port")
            current.value = (current.value & ~mask) | (default & mask)
            current.controls = [control for control in current.controls if not control.mask & mask]
            if kind not in {"IPT_UNUSED", "IPT_UNKNOWN"}:
                control_name = _control_name(current.name, kind, player)
                analog = current.name.startswith("ADC")
                if control_name:
                    current.controls.append(Control(control_name, mask, player, analog))
                elif kind not in {"IPT_PADDLE", "IPT_PEDAL", "IPT_LIGHTGUN_X", "IPT_LIGHTGUN_Y", "IPT_SERVICE", "IPT_BUTTON5", "IPT_BUTTON6", "IPT_BUTTON7", "IPT_BUTTON8"}:
                    raise InputSourceError(f"{name}:{line_number}: unsupported input type {kind}")
            continue
        service = _parse_call(line, "PORT_SERVICE")
        if service:
            if current is None or len(service) != 2:
                raise InputSourceError(f"{name}:{line_number}: malformed PORT_SERVICE")
            mask, active = _integer(service[0]), service[1]
            default = mask if active == "IP_ACTIVE_LOW" else 0
            current.value = (current.value & ~mask) | default
            current.controls = [control for control in current.controls if not control.mask & mask]
            current.controls.append(Control("test", mask))
            continue
        if line.startswith("PORT_DIPNAME(") or line.startswith("PORT_DIPSETTING("):
            args = _parse_call(line, "PORT_DIPNAME" if line.startswith("PORT_DIPNAME(") else "PORT_DIPSETTING")
            if line.startswith("PORT_DIPNAME("):
                if current is None or len(args) < 2:
                    raise InputSourceError(f"{name}:{line_number}: malformed PORT_DIPNAME")
                mask, default = _integer(args[0]), _integer(args[1])
                current.value = (current.value & ~mask) | (default & mask)
                if len(args) < 3 or not (args[2].startswith('"') and args[2].endswith('"')):
                    raise InputSourceError(f"{name}:{line_number}: DIP name is not a source string")
                dip_name = re.sub(r"[^a-z0-9]+", "_", args[2][1:-1].lower()).strip("_")
                current.controls = [control for control in current.controls if not control.mask & mask]
                current.controls.append(Control(dip_name, mask))
            elif len(args) < 2:
                raise InputSourceError(f"{name}:{line_number}: malformed PORT_DIPSETTING")
            continue
        # MAME annotations following the handled input macros are not declarations.
        if re.match(r"PORT_(8WAY|4WAY|NAME|CODE|CHAR|MINMAX|SENSITIVITY|KEYDELTA|CENTERDELTA|REVERSE|CROSSHAIR|PLAYER|DIPLOCATION)\b", line):
            continue
        raise InputSourceError(f"{name}:{line_number}: unsupported macro {line.split('(', 1)[0]}")
    cache[name] = {key: Port(p.name, p.width, p.value, list(p.controls)) for key, p in ports.items()}
    return ports


def generate(source: str, profile_doc: dict) -> str:
    if profile_doc.get("mame_source_commit") != PINNED_COMMIT:
        raise InputSourceError("profile registry does not match pinned MAME commit")
    blocks = input_blocks(source)
    cache: dict[str, dict[str, Port]] = {}
    rows: list[str] = [
        "/* Generated from MAME src/mame/namco/namcos11.cpp at " + PINNED_COMMIT + "; do not edit. */",
        "/* Binding profile is the MAME INPUT_PORTS_START name; analog=1 selects ADC/GUN stream. */",
        "static const struct System11InputBinding input_bindings[] = {",
    ]
    input_names = sorted({profile["input"] for profile in profile_doc["profiles"]})
    missing = set(input_names) - blocks.keys()
    if missing:
        raise InputSourceError("profile registry references missing input blocks: " + ", ".join(sorted(missing)))
    all_bindings, all_defaults = [], []
    for input_name in input_names:
        ports = _expand(input_name, blocks, cache)
        bindings = []
        defaults = []
        for port_name, port in ports.items():
            if port_name == "SWITCH":
                analog, index = 0, 1
            elif port_name == "PLAYER4":
                analog, index = 0, 0
            elif port_name == "PLAYER1":
                analog, index = 0, 2
            elif port_name == "PLAYER2":
                analog, index = 0, 3
            elif re.fullmatch(r"ADC[0-7]", port_name):
                analog, index = 1, int(port_name[3:])
            elif port_name in {"GUN1X", "GUN1Y", "GUN2X", "GUN2Y"}:
                analog, index = 1, 8 + (int(port_name[3]) - 1) * 2 + (port_name.endswith("Y"))
            else:
                raise InputSourceError(f"{input_name}: unsupported port name {port_name}")
            defaults.append((input_name, analog, index, port.value))
            for control in port.controls:
                bindings.append((input_name, control.control, analog, index, control.mask))
        all_bindings.extend(bindings)
        all_defaults.extend(defaults)
    for name, control, analog, index, mask in all_bindings:
        rows.append(f'{{"{name}", "{control}", {analog}, {index}, 0x{mask:x}}},')
    rows.extend(["};", "", "/* profile, analog stream (0=byte ports, 1=ADC/GUN), index, raw power-on value */",
                 "static const struct System11InputDefault input_defaults[] = {"])
    for name, analog, index, value in all_defaults:
        rows.append(f'{{"{name}", {analog}, {index}, 0x{value:x}}},')
    rows.append("};")
    return "\n".join(rows) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--profiles", type=Path, default=DEFAULT_PROFILES)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--allow-unpinned", action="store_true", help="skip local MAME git HEAD check")
    args = parser.parse_args()
    try:
        if not args.allow_unpinned:
            commit = subprocess.run(["git", "-C", str(args.source.parent.parent.parent.parent), "rev-parse", "HEAD"],
                                    check=True, text=True, capture_output=True).stdout.strip()
            if commit != PINNED_COMMIT:
                raise InputSourceError(f"MAME checkout is {commit}, expected {PINNED_COMMIT}")
        profile_doc = json.loads(args.profiles.read_text(encoding="utf-8"))
        generated = generate(args.source.read_text(encoding="utf-8"), profile_doc)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(generated, encoding="utf-8")
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError, InputSourceError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
