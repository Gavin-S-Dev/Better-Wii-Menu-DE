"""Package the Release application and its runtime data, without development output."""
import hashlib
import json
import re
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


# These match the pinned Qt/MSVC deployment. Review when upgrading dependencies.
QT_DLLS = ("Qt6Core.dll", "Qt6Gui.dll", "Qt6Svg.dll", "Qt6Widgets.dll")
QT_PLUGINS = (
    "iconengines/qsvgicon.dll", "imageformats/qsvg.dll",
    "platforms/qwindows.dll", "styles/qmodernwindowsstyle.dll",
)
MSVC_DLLS = (
    "concrt140.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
    "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll", "vccorlib140.dll",
    "vcruntime140.dll", "vcruntime140_1.dll", "vcruntime140_threads.dll",
)
DEVELOPMENT_DIRS = {
    ".git", "__pycache__", "cache", "evidence", "fixtures", "logs", "qa",
    "screenshots", "test", "tests", "user",
}
DEVELOPMENT_SUFFIXES = {
    ".cpp", ".exp", ".h", ".idb", ".ilk", ".iobj", ".ipdb", ".lib",
    ".log", ".obj", ".pdb", ".pch", ".py", ".pyc", ".sln", ".vcxproj",
}
DEBUG_DLL = re.compile(
    r"(?:qt6.*d|q(?:windows|svg|svgicon|modernwindowsstyle)d|"
    r"(?:msvcp|vcruntime|concrt|vccorlib)\d+d.*|ucrtbased)\.dll$", re.IGNORECASE,
)


def validate_release_path(name):
    path = PurePosixPath(name)
    if (path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name or
            any(part.lower() in DEVELOPMENT_DIRS for part in path.parts) or
            path.suffix.lower() in DEVELOPMENT_SUFFIXES or DEBUG_DLL.fullmatch(path.name)):
        raise RuntimeError(f"Development or unsafe file in release: {name}")
    if path.suffix.lower() == ".exe" and name != "BetterWiiMenuDE.exe":
        raise RuntimeError(f"Unexpected executable in release: {name}")


def collect_release_files(root):
    source = root / "build-candidate" / "Binaries"
    files = {}

    def add(name, source_file):
        validate_release_path(name)
        if not source_file.is_file():
            raise RuntimeError(f"Missing required release file: {source_file}")
        files[name] = source_file

    for name in ("BetterWiiMenuDE.exe", "qt.conf", *QT_DLLS):
        add(name, source / name)
    for name in QT_PLUGINS:
        add("QtPlugins/" + name, source / "QtPlugins" / name)

    # Package only tracked assets, not possibly contaminated build/Sys copies.
    tracked = subprocess.check_output(
        ["git", "ls-files", "-z", "--", "Data/Sys/", "LICENSES/"], cwd=root
    ).decode("utf-8").split("\0")
    for relative in filter(None, tracked):
        source_file = root / relative
        if not source_file.resolve().is_relative_to(root.resolve()):
            raise RuntimeError(f"Runtime asset points outside the source checkout: {relative}")
        name = ("Sys/" + relative[len("Data/Sys/"):]
                if relative.startswith("Data/Sys/")
                else "Licenses/" + relative[len("LICENSES/"):])
        add(name, source_file)
    if not any(name.startswith("Sys/") for name in files):
        raise RuntimeError("No tracked system data found")
    if not any(name.startswith("Licenses/") for name in files):
        raise RuntimeError("No tracked license files found")
    for translation in sorted((source / "Languages").glob("*/dolphin-emu.mo")):
        add("Languages/" + translation.relative_to(source / "Languages").as_posix(), translation)

    runtimes = sorted((root / "build-win-release" / "toolchain" / "msvc").glob(
        "VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT"))
    if not runtimes:
        raise RuntimeError("Missing release runtime: run Tools/setup_fork_compiler.py")
    for name in MSVC_DLLS:
        add(name, runtimes[-1] / name)
    add("COPYING", root / "COPYING")
    add("README.md", root / "Readme-BetterWiiMenu.md")
    return files


def package(root=ROOT):
    root = Path(root).resolve()
    header = (root / "Source/Core/Common/BetterWiiMenu.h").read_text(encoding="utf-8")
    match = re.search(r'VERSION\[\] = "(\d+\.\d+\.\d+)"', header)
    if match is None:
        raise RuntimeError("Cannot read the fork version")
    version = match.group(1)
    output = root / "dist" / f"BetterWiiMenuDE-{version}-2609a-x64"
    archive = output.parent / (output.name + ".zip")
    checksum_path = archive.with_suffix(".zip.sha256")
    for destination in (output, archive, checksum_path):
        if destination.exists():
            raise RuntimeError(f"Refusing to overwrite existing package: {destination}")
    # Validate inputs before creating output. Never copy an entire build tree.
    files = collect_release_files(root)
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root))
    output.mkdir(parents=True)
    for name, source_file in sorted(files.items()):
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_file, target)
    manifest = {"fork_version": version, "upstream": "2609a", "base_commit": revision,
                "working_tree_changes": dirty,
                "files": {name: hashlib.sha256((output / name).read_bytes()).hexdigest()
                          for name in sorted(files)}}
    (output / "build-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    with zipfile.ZipFile(archive, "x", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path in sorted(output.rglob("*")):
            if path.is_file():
                name = path.relative_to(output).as_posix()
                validate_release_path(name)
                z.write(path, name)
    checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
    checksum_path.write_text(f"{checksum}  {archive.name}\n", encoding="utf-8")
    print(archive)
    print(checksum)
    return archive


if __name__ == "__main__":
    package()
