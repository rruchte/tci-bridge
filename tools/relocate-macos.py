#!/usr/bin/env python3
"""Make the Qt app bundle independent of Homebrew install names.

macdeployqt bundles Qt frameworks, but Homebrew framework IDs and transitive
libraries can still refer to /opt/homebrew or /usr/local. Repair references
in every Mach-O within the bundle; copy other dynamic libraries as needed.
Fail closed if a required dependency cannot be located.
"""
from __future__ import annotations

import glob
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def command(*args: str) -> str:
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def is_macho(path: Path) -> bool:
    return "Mach-O" in command("file", "-b", str(path))


def dependencies(path: Path) -> list[str]:
    return [
        line.strip().split(" (", 1)[0]
        for line in command("otool", "-L", str(path)).splitlines()[1:]
        if line.strip()
    ]


def homebrew(path: str) -> bool:
    return path.startswith("/opt/homebrew/") or path.startswith("/usr/local/opt/") or path.startswith("/usr/local/Cellar/")


def find_brew_library(name: str) -> Path | None:
    for prefix in ("/opt/homebrew", "/usr/local"):
        options = [
            f"{prefix}/lib/{name}",
            *glob.glob(f"{prefix}/opt/*/lib/{name}"),
            *glob.glob(f"{prefix}/Cellar/*/*/lib/{name}"),
        ]
        for option in options:
            if os.path.isfile(option):
                return Path(option)
    return None


def main() -> None:
    bundle = Path(sys.argv[1]).resolve()
    contents = bundle / "Contents"
    frameworks = contents / "Frameworks"
    frameworks.mkdir(parents=True, exist_ok=True)

    for iteration in range(20):
        changed = False
        for source in sorted(contents.rglob("*")):
            if not source.is_file() or source.is_symlink() or not is_macho(source):
                continue
            for index, old in enumerate(dependencies(source)):
                # A dylib's first otool entry is its own LC_ID_DYLIB, not
                # an imported library. This includes multimedia plugin dylibs
                # in Contents/PlugIns, which need not live in Frameworks.
                if index == 0 and source.suffix == ".dylib" and Path(old).name == source.name:
                    if homebrew(old):
                        command("install_name_tool", "-id", "@rpath/" + source.name, str(source))
                        changed = True
                    continue
                if old.startswith(("/System/", "/usr/lib/")) or old.startswith(("@loader_path/", "@executable_path/")):
                    continue
                if not (homebrew(old) or old.startswith("@rpath/")):
                    continue

                framework_match = re.search(r"(Qt[^/]+\.framework)/(Versions/[^/]+/[^/]+)$", old)
                if framework_match:
                    framework_name, internal_path = framework_match.groups()
                    target = frameworks / framework_name / internal_path
                    if not target.exists():
                        # macdeployqt normally copies all used Qt frameworks.
                        # Try the Homebrew source location for transitive Qt dependencies.
                        if homebrew(old):
                            name_root = old.split(framework_name, 1)[0] + framework_name
                            origin = Path(name_root)
                        else:
                            origin = next(
                                (p for p in [
                                    Path(p) / framework_name
                                    for p in glob.glob("/opt/homebrew/opt/*/lib") + glob.glob("/usr/local/opt/*/lib")
                                ] if p.exists()), None
                            )
                        if origin is None or not origin.exists():
                            raise RuntimeError(f"Missing framework for {source}: {old}")
                        shutil.copytree(origin, frameworks / framework_name, symlinks=True, dirs_exist_ok=True)
                        changed = True
                elif old.endswith(".dylib"):
                    target = frameworks / Path(old).name
                    if not target.exists():
                        origin = Path(old) if homebrew(old) else find_brew_library(target.name)
                        if origin is None or not origin.is_file():
                            raise RuntimeError(f"Missing dylib for {source}: {old}")
                        shutil.copy2(origin.resolve(), target)
                        changed = True
                else:
                    # @rpath can be a Qt private framework with a different
                    # layout; do not silently permit unresolved paths.
                    if old.startswith("@rpath/"):
                        raise RuntimeError(f"Unsupported unresolved dependency in {source}: {old}")
                    continue

                if not target.exists():
                    raise RuntimeError(f"Bundled target missing for {source}: {old} -> {target}")

                # LC_ID_DYLIB is the first otool entry for a framework/dylib.
                # It is metadata, not a dependency load, and requires -id.
                is_id = source == target or (
                    source.parent == target.parent and source.name == target.name
                )
                if is_id:
                    new = "@rpath/" + target.relative_to(frameworks).as_posix()
                    if old != new:
                        command("install_name_tool", "-id", new, str(source))
                        changed = True
                else:
                    rel = os.path.relpath(target, source.parent)
                    new = "@loader_path/" + rel
                    if old != new:
                        command("install_name_tool", "-change", old, new, str(source))
                        changed = True
        if not changed:
            break
    else:
        raise RuntimeError("Dependency relocation did not converge")

    # Reject every remaining direct dependency on a build machine location.
    for source in sorted(contents.rglob("*")):
        if source.is_file() and not source.is_symlink() and is_macho(source):
            for index, dep in enumerate(dependencies(source)):
                if index == 0 and source.suffix == ".dylib" and Path(dep).name == source.name:
                    continue
                if homebrew(dep):
                    raise RuntimeError(f"Nonrelocatable dependency in {source}: {dep}")
                if dep.startswith("@rpath/"):
                    name = dep.removeprefix("@rpath/")
                    if not (frameworks / name).exists():
                        raise RuntimeError(f"Unbundled @rpath dependency in {source}: {dep}")
    print("All bundled Mach-O dependencies are relocatable")


if __name__ == "__main__":
    main()
