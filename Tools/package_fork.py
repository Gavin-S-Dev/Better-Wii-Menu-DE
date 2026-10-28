"""Package the tested Release output, excluding all developer/test/user profiles."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def package():
    source = ROOT / "build-candidate" / "Binaries"
    output = ROOT / "dist" / "BetterWiiMenuDE-1.1.0-2609a-x64"
    if output.exists():
        raise RuntimeError(f"Refusing to overwrite existing package: {output}")
    if not (source / "BetterWiiMenuDE.exe").is_file():
        raise RuntimeError("Build the fork first with build_fork.cmd")
    output.mkdir(parents=True)
    for item in source.iterdir():
        if item.is_file() and (item.suffix.lower() == ".dll" or
                              item.name in ["BetterWiiMenuDE.exe", "qt.conf"]):
            shutil.copy2(item, output / item.name)
        elif item.name in ["Sys", "QtPlugins", "Languages", "Licenses"] and item.is_dir():
            shutil.copytree(item, output / item.name)
    # App-local release runtime avoids relying on an installed developer runtime.
    runtimes = list((ROOT / "build-win-release" / "toolchain" / "msvc").glob(
        "VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT"))
    if not runtimes:
        raise RuntimeError("Missing release runtime: run Tools/setup_fork_compiler.py")
    for dll in runtimes[-1].glob("*.dll"):
        shutil.copy2(dll, output / dll.name)
    shutil.copy2(ROOT / "COPYING", output / "COPYING")
    shutil.copy2(ROOT / "Readme-BetterWiiMenu.md", output / "README.md")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    manifest = {"fork_version": "1.1.0", "upstream": "2609a", "base_commit": revision,
                "working_tree_changes": bool(subprocess.check_output(
                    ["git", "status", "--porcelain"], cwd=ROOT)),
                "files": {str(p.relative_to(output)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in output.rglob("*") if p.is_file()}}
    (output / "build-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    assert not (output / "User").exists()
    assert not (output / "Updater.exe").exists()
    assert not list(output.rglob("*.pdb"))
    archive = output.parent / (output.name + ".zip")
    with zipfile.ZipFile(archive, "x", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path in output.rglob("*"):
            if path.is_file():
                z.write(path, str(path.relative_to(output)))
    checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(".zip.sha256").write_text(f"{checksum}  {archive.name}\n")
    print(archive)
    print(checksum)


if __name__ == "__main__":
    package()
