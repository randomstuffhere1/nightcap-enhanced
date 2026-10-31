#!/usr/bin/env python3
# Copyright 2026 Mocktail Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""List every FFlag the installed Roblox payload knows, with its default.

Usage:
    python3 scripts/list_fflags.py
    python3 scripts/list_fflags.py --filter shadow --type FInt,DFInt
    python3 scripts/list_fflags.py --json --filter '^FFlagRender' > fflags.json

The payload ships its app-settings baseline as
`assets/android/shared_compression_dictionaries/*.dict` inside base.apk, and
payload extraction leaves a copy in `rbx_bin/assets/android/`. The file is a
plain `{"applicationSettings": {...}}` object: the same list Roblox patches at
startup, so it describes the build that is installed rather than a web page
that may be years out of date.

`libroblox.so` no longer carries the flag table. The engine reads names from
that JSON, and the only `FFlag` text left in the binary is log messages. The
script still checks the .so (older builds embedded the object) and says so when
it finds nothing.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys
import zipfile
from typing import Iterable

SETTINGS_MARKER = b'{"applicationSettings"'
DICTIONARY_DIRECTORY = "assets/android/shared_compression_dictionaries"
MAX_EMBEDDED_OBJECT_BYTES = 64 * 1024 * 1024

# The value kinds Roblox defines, longest first so DFFlag is not read as FFlag.
FLAG_KIND = re.compile(r"^(?:DF|SF|F)(?:Flag|Int|String|Log|Double|Float|Bool)")
KIND_ORDER = (
    "FFlag", "DFFlag", "FInt", "DFInt","FString", "DFString", "FLog", "DFLog",
)
OTHER_KIND = "other"


def flag_kind(name: str) -> str:
    match = FLAG_KIND.match(name)
    return match.group(0) if match else OTHER_KIND


def stringify(value: object) -> str:
    if isinstance(value, bool):
        return "True" if value else "False"
    return str(value)


def parse_settings(raw: bytes) -> dict[str, str] | None:
    """The flag map from `raw`, or None when it is not a settings object."""
    try:
        document = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(document, dict):
        return None
    settings = document.get("applicationSettings")
    if isinstance(settings, dict):
        values = settings
    elif all(isinstance(value, (str, bool, int, float)) for value in document.values()):
        values = document
    else:
        return None
    return {str(key): stringify(value) for key, value in values.items()}


def balanced_object(data: bytes, start: int) -> bytes | None:
    """The JSON object starting at `start`, found by brace matching."""
    depth = 0
    in_string = False
    escaped = False
    for index in range(start, min(len(data), start + MAX_EMBEDDED_OBJECT_BYTES)):
        byte = data[index]
        if in_string:
            if escaped:
                escaped = False
            elif byte == 0x5C:
                escaped = True
            elif byte == 0x22:
                in_string = False
            continue
        if byte == 0x22:
            in_string = True
        elif byte == 0x7B:
            depth += 1
        elif byte == 0x7D:
            depth -= 1
            if depth == 0:
                return data[start:index + 1]
    return None


def settings_from_libroblox(path: Path) -> dict[str, str] | None:
    """The settings object some builds embed in the engine, if present."""
    data = path.read_bytes()
    start = data.find(SETTINGS_MARKER)
    if start < 0:
        match = re.search(rb'\{"applicationSettings"\s*:', data)
        if match is None:
            return None
        start = match.start()
    raw = balanced_object(data, start)
    return parse_settings(raw) if raw is not None else None


def settings_from_apk(path: Path) -> tuple[str, dict[str, str]] | None:
    """Settings from the dictionary asset inside an APK."""
    try:
        archive = zipfile.ZipFile(path)
    except (OSError, zipfile.BadZipFile):
        return None
    with archive:
        names = archive.namelist()
        dictionaries = [name for name in names if name.endswith(".dict")]
        dictionaries.sort(key=lambda name: DICTIONARY_DIRECTORY not in name)
        for name in dictionaries:
            settings = parse_settings(archive.read(name))
            if settings:
                return name, settings
        # No dictionary asset: look for a plain settings JSON instead.
        for name in names:
            if name.startswith("META-INF/") or not name.startswith("assets/"):
                continue
            info = archive.getinfo(name)
            if info.file_size > 8 * 1024 * 1024:
                continue
            settings = parse_settings(archive.read(name))
            if settings:
                return name, settings
    return None


def payload_apks(payload: Path) -> list[Path]:
    found: list[Path] = []
    for pattern in ("sober_apk/*.apk", "*.apk", "updates/*/*.apk"):
        found.extend(sorted(payload.glob(pattern)))
    unique: dict[Path, Path] = {}
    for path in found:
        if path.is_file():
            unique.setdefault(path.resolve(), path)
    return list(unique.values())


def payload_version(payload: Path) -> str | None:
    for manifest in sorted(payload.glob("versions/*/roblox_payload.json")):
        try:
            document = json.loads(manifest.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError):
            continue
        version = document.get("version_name")
        if version:
            abi = document.get("abi")
            return f"Roblox {version}" + (f", {abi}" if abi else "")
    return None


def default_payloads() -> list[Path]:
    candidates = [Path.cwd() / "rbx_bin"]
    data_root = os.environ.get("MOCKTAIL_DATA_ROOT")
    if data_root:
        candidates.append(Path(data_root) / "rbx_bin")
    xdg_data = os.environ.get("XDG_DATA_HOME")
    base = Path(xdg_data) if xdg_data else Path.home() / ".local" / "share"
    candidates.append(base / "mocktail" / "rbx_bin")
    candidates.append(
        Path.home() / ".var" / "app" / "io.github.CoderDayton.nightcap"
        / "data" / "mocktail" / "rbx_bin"
    )
    return candidates


def find_settings(
    arguments: argparse.Namespace,
) -> tuple[str, dict[str, str], list[str], Path | None, str | None]:
    """(source, settings, notes, payload, version). Exits when nothing is found."""
    notes: list[str] = []

    if arguments.settings:
        path = Path(arguments.settings)
        try:
            settings = parse_settings(path.read_bytes())
        except OSError as error:
            raise SystemExit(f"cannot read {path}: {error}")
        if not settings:
            raise SystemExit(f"{path} is not an app-settings JSON object")
        return str(path), settings, notes, None, None

    def check_library(library: Path) -> dict[str, str] | None:
        embedded = settings_from_libroblox(library)
        if embedded:
            return embedded
        notes.append(
            "libroblox.so has no flag table; the engine reads the names from "
            f"its app settings (checked {library})"
        )
        return None

    if arguments.apk:
        apk = Path(arguments.apk)
        found = settings_from_apk(apk)
        if found:
            member, settings = found
            return f"{apk}:{member}", settings, notes, None, None
        notes.append(f"{apk} does not contain an app-settings asset")

    if arguments.libroblox:
        library = Path(arguments.libroblox)
        if not library.is_file():
            raise SystemExit(f"cannot read {library}")
        embedded = check_library(library)
        if embedded:
            return str(library), embedded, notes, None, None

    payloads = [Path(arguments.payload)] if arguments.payload else default_payloads()
    payloads = [path for path in payloads if path.is_dir()]

    for payload in payloads:
        version = payload_version(payload)
        if not arguments.libroblox:
            library = payload / "libroblox.so"
            if library.is_file():
                embedded = check_library(library)
                if embedded:
                    return str(library), embedded, notes, payload, version

        dictionaries = sorted(
            payload.glob(f"{DICTIONARY_DIRECTORY}/*.dict")
        )
        best: tuple[str, dict[str, str]] | None = None
        for dictionary in dictionaries:
            try:
                settings = parse_settings(dictionary.read_bytes())
            except OSError:
                continue
            if settings and (best is None or len(settings) > len(best[1])):
                best = (str(dictionary), settings)
        if best:
            return best[0], best[1], notes, payload, version

        apks: Iterable[Path] = (
            [Path(arguments.apk)] if arguments.apk else payload_apks(payload)
        )
        for apk in apks:
            found = settings_from_apk(apk)
            if found:
                member, settings = found
                return f"{apk}:{member}", settings, notes, payload, version
            notes.append(f"{apk} does not contain an app-settings asset")

    if not payloads:
        searched = "\n  ".join(str(path) for path in default_payloads())
        raise SystemExit(
            "no Roblox payload found; pass --payload, --apk or --settings.\n"
            f"Searched:\n  {searched}"
        )
    searched = "\n  ".join(str(path) for path in payloads)
    raise SystemExit(
        "no app-settings baseline found in the payload; pass --settings with a "
        "client settings JSON.\nSearched:\n  " + searched
    )


def load_overrides(path: Path | None) -> tuple[dict[str, str], str | None]:
    if path is None:
        return {}, None
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {}, None
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        return {}, f"cannot read overrides from {path}: {error}"
    if not isinstance(document, dict):
        return {}, f"{path} is not a JSON object; ignoring it"
    overrides = {}
    for key, value in document.items():
        if isinstance(value, (str, bool, int, float)):
            overrides[str(key)] = stringify(value)
    return overrides, None


FILTER_SUFFIXES = (
    ("_PlaceFilter", "places"),
    ("_UniverseFilter", "universes"),
    ("_IXP", "layers"),
)


def format_value(name: str, value: str) -> str:
    for suffix, label in FILTER_SUFFIXES:
        if name.endswith(suffix) and ";" in value:
            enabled, rest = value.split(";", 1)
            return f"{enabled} ({label}: {rest.replace(';', ', ')})"
    return value


def matches(name: str, arguments: argparse.Namespace) -> bool:
    if arguments.type:
        kinds = {
            kind.strip().upper()
            for kind in arguments.type.split(",")
            if kind.strip()
        }
        if flag_kind(name).upper() not in kinds:
            return False
    if arguments.filter and not arguments.filter.search(name):
        return False
    return True


def selected(settings: dict[str, str], arguments: argparse.Namespace) -> dict[str, str]:
    chosen = {}
    for name in sorted(settings):
        kind = flag_kind(name)
        if kind == OTHER_KIND and not arguments.all:
            continue
        if matches(name, arguments):
            chosen[name] = settings[name]
    return chosen


def grouped(names: Iterable[str]) -> dict[str, list[str]]:
    groups: dict[str, list[str]] = {}
    for name in names:
        groups.setdefault(flag_kind(name), []).append(name)

    def order(kind: str) -> tuple[int, str]:
        if kind in KIND_ORDER:
            return KIND_ORDER.index(kind), kind
        return len(KIND_ORDER), kind

    return {kind: groups[kind] for kind in sorted(groups, key=order)}


def print_report(
    source: str,
    settings: dict[str, str],
    payload: str | None,
    version: str | None,
    notes: list[str],
    overrides: dict[str, str],
    override_path: Path | None,
    override_error: str | None,
    arguments: argparse.Namespace,
) -> int:
    chosen = selected(settings, arguments)
    if not chosen:
        print("no flags matched", file=sys.stderr)
        return 1

    width = min(max(len(name) for name in chosen), 64)
    kinds = grouped(chosen)
    flag_count = sum(len(names) for kind, names in kinds.items() if kind != OTHER_KIND)
    settings_count = sum(
        len(names) for kind, names in kinds.items() if kind == OTHER_KIND
    )

    print("Roblox FFlags")
    if payload:
        print(f"  payload   : {payload}" + (f" ({version})" if version else ""))
    print(f"  source    : {source}")
    for note in notes:
        print(f"  note      : {note}")
    counts = ", ".join(
        f"{kind} {len(names)}" for kind, names in kinds.items() if kind != OTHER_KIND
    )
    print(f"  flags     : {flag_count}" + (f" ({counts})" if counts else ""))
    if settings_count:
        print(f"  settings  : {settings_count} shown that are not flags (--all)")
    elif not arguments.all:
        hidden = sum(1 for name in settings if flag_kind(name) == OTHER_KIND)
        if hidden:
            print(f"  settings  : {hidden} non-flag settings hidden (--all shows them)")
    if override_error:
        print(f"  overrides : {override_error}")
    elif overrides:
        print(f"  overrides : {len(overrides)} from {override_path}")

    if overrides and override_path is not None:
        visible = {
            name: value
            for name, value in sorted(overrides.items())
            if name not in settings or matches(name, arguments)
        }
        if visible:
            print(f"\nOverrides in {override_path} ({len(overrides)})")
            override_width = min(
                max([width] + [len(name) for name in visible]), 64
            )
            for name, value in visible.items():
                if name not in settings:
                    state = "not in this build"
                elif settings[name] == value:
                    state = "same as default"
                else:
                    state = f"default {format_value(name, settings[name])}"
                print(
                    f"  {name.ljust(override_width)}  = "
                    f"{format_value(name, value).ljust(16)}  {state}"
                )

    for kind, names in kinds.items():
        title = kind if kind != OTHER_KIND else "other app settings (not flags)"
        print(f"\n{title} ({len(names)})")
        for name in names:
            print(f"  {name.ljust(width)}  = {format_value(name, chosen[name])}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="List the FFlags and defaults of the installed Roblox payload.",
    )
    parser.add_argument(
        "--payload",
        metavar="DIR",
        help="rbx_bin directory (default: ./rbx_bin, then the Nightcap data dir)",
    )
    parser.add_argument(
        "--apk", metavar="FILE", help="read the settings asset from this APK"
    )
    parser.add_argument(
        "--libroblox",
        metavar="FILE",
        help="check this libroblox.so for an embedded table",
    )
    parser.add_argument(
        "--settings",
        metavar="FILE",
        help="read a client settings or app-settings JSON instead of the payload",
    )
    parser.add_argument(
        "--overrides",
        metavar="FILE",
        help="fflags.json to compare against (default: ~/.config/mocktail/fflags.json)",
    )
    parser.add_argument(
        "--no-overrides",
        action="store_true",
        help="do not read an fflags.json",
    )
    parser.add_argument(
        "--filter",
        metavar="REGEX",
        help="only names matching this regular expression (case-insensitive)",
    )
    parser.add_argument(
        "--type",
        metavar="KIND",
        help="only these kinds, comma separated: FFlag, DFFlag, FInt, DFInt, ...",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="also list app settings that are not flags",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="print an fflags.json object instead of the report",
    )
    arguments = parser.parse_args()

    if arguments.filter:
        try:
            arguments.filter = re.compile(arguments.filter, re.IGNORECASE)
        except re.error as error:
            parser.error(f"--filter is not a regular expression: {error}")
    if arguments.type:
        unknown = [
            kind.strip()
            for kind in arguments.type.split(",")
            if not FLAG_KIND.fullmatch(kind.strip())
        ]
        if unknown:
            parser.error(
                "unknown --type value(s): "
                + ", ".join(unknown)
                + "; known: "
                + ", ".join(KIND_ORDER)
            )

    source, settings, notes, payload_path, version = find_settings(arguments)
    payload = str(payload_path) if payload_path is not None else None

    if arguments.json:
        json.dump(selected(settings, arguments), sys.stdout, indent=2, sort_keys=True)
        print()
        return 0

    if arguments.no_overrides:
        overrides, override_path, override_error = {}, None, None
    else:
        if arguments.overrides:
            override_path = Path(arguments.overrides)
        else:
            config_home = os.environ.get("XDG_CONFIG_HOME")
            base = Path(config_home) if config_home else Path.home() / ".config"
            override_path = base / "mocktail" / "fflags.json"
        overrides, override_error = load_overrides(override_path)
        if override_error:
            override_path = None

    return print_report(
        source,
        settings,
        payload,
        version,
        notes,
        overrides,
        override_path,
        override_error,
        arguments,
    )


if __name__ == "__main__":
    sys.exit(main())
