#!/usr/bin/env python3
"""Fails if code breaks the layer rules in docs/adr/0001-layered-architecture.md.

Usage: python tools/check_layers.py [--root <repository>]

Prints one `path:line: problem` per violation and exits with status 1 if there are any.
"""

import argparse
import re
import sys
from pathlib import Path

SOURCE_SUFFIXES = {".h", ".hpp", ".inl", ".cpp", ".cc"}
LAYER_DIRECTORIES = ("core", "interfaces", "modules", "runtime", "samples")

INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
LINKED_TARGET = re.compile(r"\bdream::(\w+)")

# What each layer may use from the engine, as library names and target-name prefixes. Backends are
# in no list: nothing may use a backend directly, and games get them through dream_link_modules.
ALLOWED = {
    "core": {"core"},
    "interfaces": {"core", "interface"},
    "modules": {"core", "interface"},
    "runtime": {"core", "interface", "runtime"},
    "samples": {"core", "interface", "runtime"},
}

# Layers that must see nothing but the C++ standard library and the engine.
BACKEND_FREE_LAYERS = {"interfaces", "samples"}


def kind_of_library(name, interfaces):
    """Classifies the `<name>` in an `#include "dream/<name>/..."`."""
    if name in ("core", "runtime"):
        return name
    if name in interfaces:
        return "interface"
    return None


def kind_of_target(name):
    """Classifies the `<name>` in a `dream::<name>` CMake target."""
    if name.startswith("interface_"):
        return "interface"
    if name.startswith("module_"):
        return "module"
    return name


def is_standard_header(path):
    return "/" not in path and "." not in path


def check_source(path, relative, layer, layer_root, interfaces):
    problems = []
    is_test = "tests" in relative.parts
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()

    for number, line in enumerate(lines, start=1):
        match = INCLUDE.match(line)
        if match is None:
            continue
        where = f"{relative.as_posix()}:{number}"
        included = match.group(2)

        if ".." in included.split("/"):
            problems.append(f"{where}: include '{included}' reaches outside its directory")
        elif included.startswith("dream/"):
            library = included.split("/")[1]
            if kind_of_library(library, interfaces) not in ALLOWED[layer]:
                problems.append(f"{where}: {layer} must not include '{included}'")
        elif layer not in BACKEND_FREE_LAYERS or is_test:
            continue
        elif match.group(1) == "<":
            if not is_standard_header(included):
                problems.append(
                    f"{where}: {layer} must not include third-party header '{included}'"
                )
        elif next(layer_root.rglob(included), None) is None:
            problems.append(f"{where}: {layer} must not include '{included}'")

    return problems


def check_cmake(path, relative, layer):
    problems = []
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()

    for number, line in enumerate(lines, start=1):
        code = line.split("#", 1)[0]
        for target in LINKED_TARGET.findall(code):
            if kind_of_target(target) not in ALLOWED[layer]:
                problems.append(
                    f"{relative.as_posix()}:{number}: {layer} must not link dream::{target}"
                )

    return problems


def check_repository(root):
    """Returns a sorted list of every violation found under `root`."""
    interface_root = root / "interfaces"
    interfaces = (
        {entry.name for entry in interface_root.iterdir() if entry.is_dir()}
        if interface_root.is_dir()
        else set()
    )

    problems = []
    for layer in LAYER_DIRECTORIES:
        layer_root = root / layer
        if not layer_root.is_dir():
            continue
        for path in layer_root.rglob("*"):
            if not path.is_file():
                continue
            relative = path.relative_to(root)
            if path.suffix in SOURCE_SUFFIXES:
                problems += check_source(path, relative, layer, layer_root, interfaces)
            elif path.name == "CMakeLists.txt" or path.suffix == ".cmake":
                problems += check_cmake(path, relative, layer)

    return sorted(problems)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parent.parent, help="repository root"
    )
    arguments = parser.parse_args()

    problems = check_repository(arguments.root)
    for problem in problems:
        print(problem)
    if problems:
        print(f"{len(problems)} layer rule violation(s).", file=sys.stderr)
        return 1
    print("Layer rules hold.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
