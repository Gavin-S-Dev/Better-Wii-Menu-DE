"""Extract hash-pinned Microsoft compiler packages locally; no installer or registry changes."""
import concurrent.futures
import hashlib
import json
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "build-win-release" / "toolchain"


def extract(package):
    assert package["url"].startswith("https://download.visualstudio.microsoft.com/")
    archive = DEST / package["fileName"]
    if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != package["sha256"]:
        with urllib.request.urlopen(package["url"], timeout=90) as response:
            archive.write_bytes(response.read())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != package["sha256"]:
        raise RuntimeError(f"Hash verification failed: {archive.name}")
    with zipfile.ZipFile(archive) as package_zip:
        for name in package_zip.namelist():
            if name.startswith("Contents/") and not name.endswith("/"):
                target = (DEST / "msvc" / name[len("Contents/"):]).resolve()
                if not target.is_relative_to((DEST / "msvc").resolve()):
                    raise RuntimeError("Unsafe compiler archive path")
                target.parent.mkdir(parents=True, exist_ok=True)
                content = package_zip.read(name)
                if not target.exists() or target.read_bytes() != content:
                    target.write_bytes(content)
    print(f"Verified and extracted {archive.name}", flush=True)


if __name__ == "__main__":
    DEST.mkdir(parents=True, exist_ok=True)
    packages = json.loads((ROOT / "Tools" / "fork-msvc-lock.json").read_text())
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(extract, packages))
