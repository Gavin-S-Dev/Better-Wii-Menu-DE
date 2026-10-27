"""Use the matching release runtime for local application and test launches."""
from pathlib import Path
import shutil

root = Path(__file__).resolve().parents[1]
runtime = next((root / "build-win-release/toolchain/msvc/VC/Redist/MSVC").glob(
    "*/x64/Microsoft.VC*.CRT"))
for target in [root / "build-candidate/Binaries", root / "build-candidate/Binaries/Tests"]:
    target.mkdir(parents=True, exist_ok=True)
    for dll in runtime.glob("*.dll"):
        shutil.copy2(dll, target / dll.name)
