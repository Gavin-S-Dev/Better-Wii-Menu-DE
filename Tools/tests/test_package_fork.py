"""Development-only checks for accidental release contamination."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

SPEC = importlib.util.spec_from_file_location(
    "package_fork", Path(__file__).resolve().parents[1] / "package_fork.py"
)
packager = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(packager)


class ReleasePackagingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="better-wii-package-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build-candidate/Binaries"
        self.runtime = self.root / "build-win-release/toolchain/msvc/VC/Redist/MSVC/1/x64/Microsoft.VC1.CRT"
        self.write(".gitignore", b"/build*/\n/dist/\n")
        self.write("Source/Core/Common/BetterWiiMenu.h", b'inline constexpr char VERSION[] = "1.3.1";\n')
        self.write("Data/Sys/Resources/wii-menu-cover.svg", b"real tracked menu artwork")
        self.write("LICENSES/GPL.txt", b"required license")
        self.write("COPYING", b"copyright notice")
        self.write("Readme-BetterWiiMenu.md", b"user instructions")
        for name in ("BetterWiiMenuDE.exe", "qt.conf", *packager.QT_DLLS):
            self.write(self.build / name, b"release application or dependency")
        for name in packager.QT_PLUGINS:
            self.write(self.build / "QtPlugins" / name, b"release Qt plugin")
        for name in packager.MSVC_DLLS:
            self.write(self.runtime / name, b"matching release runtime")
        self.write(self.build / "Languages/fr/dolphin-emu.mo", b"translation")
        self.git("init", "--quiet")
        self.git("add", ".")
        self.git("-c", "user.name=Packaging fixture", "-c", "user.email=fixture@example.invalid",
                 "commit", "--quiet", "-m", "Runtime fixture")

    def write(self, path, content):
        path = self.root / path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.root), *args], stderr=subprocess.PIPE)

    def package(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return packager.package(self.root)

    def test_excludes_stray_tests_logs_profiles_and_debug_libraries(self):
        for name in (
            "Tests/tests.exe", "User/Config/Dolphin.ini", "screenshots/qa.png",
            "build.log", "test-results.xml", "BetterWiiMenuDE.pdb", "tracking.dll",
            "Qt6Cored.dll", "QtPlugins/platforms/qwindowsd.dll",
            "QtPlugins/platforms/debug.log", "Languages/fr/test-results.json",
            "Sys/qa/fixture.bin", "Sys/build.log",
        ):
            self.write(self.build / name, b"must never ship")
        self.write(self.runtime / "vcruntime140d.dll", b"debug runtime")
        self.write("Data/Sys/Resources/qa-screenshot.png", b"untracked source-side artifact")
        with zipfile.ZipFile(self.package()) as release:
            self.assertFalse(any(release.read(name) == b"must never ship" for name in release.namelist()))
            self.assertNotIn("Sys/Resources/qa-screenshot.png", release.namelist())
            self.assertNotIn("vcruntime140d.dll", release.namelist())
            self.assertIn("Languages/fr/dolphin-emu.mo", release.namelist())
            self.assertIn("Licenses/GPL.txt", release.namelist())

    def test_uses_tracked_runtime_data_instead_of_build_directory_copies(self):
        self.write(self.build / "Sys/Resources/wii-menu-cover.svg", b"stale build data")
        with zipfile.ZipFile(self.package()) as release:
            self.assertEqual(release.read("Sys/Resources/wii-menu-cover.svg"), b"real tracked menu artwork")

    def test_refuses_a_development_artifact_even_if_added_to_git(self):
        self.write("Data/Sys/Resources/debug.log", b"accidentally staged log")
        self.git("add", "Data/Sys/Resources/debug.log")
        with self.assertRaisesRegex(RuntimeError, "Development or unsafe"):
            self.package()
        self.assertFalse((self.root / "dist").exists())

    def test_refuses_missing_required_runtime_before_creating_output(self):
        (self.build / "QtPlugins/platforms/qwindows.dll").unlink()
        with self.assertRaisesRegex(RuntimeError, "Missing required release file"):
            self.package()
        self.assertFalse((self.root / "dist").exists())

    def test_writes_correct_payload_hashes_and_archive_checksum(self):
        archive = self.package()
        self.assertEqual(archive.name, "BetterWiiMenuDE-1.3.1-2609a-x64.zip")
        with zipfile.ZipFile(archive) as release:
            manifest = json.loads(release.read("build-manifest.json"))
            self.assertFalse(manifest["working_tree_changes"])
            self.assertEqual(manifest["base_commit"], self.git("rev-parse", "HEAD").decode().strip())
            self.assertEqual(set(release.namelist()), set(manifest["files"]) | {"build-manifest.json"})
            for name, digest in manifest["files"].items():
                self.assertEqual(hashlib.sha256(release.read(name)).hexdigest(), digest)
        checksum = archive.with_suffix(".zip.sha256").read_text().split()[0]
        self.assertEqual(checksum, hashlib.sha256(archive.read_bytes()).hexdigest())

    def test_refuses_to_overwrite_an_existing_archive(self):
        archive = self.root / "dist/BetterWiiMenuDE-1.3.1-2609a-x64.zip"
        self.write(archive, b"existing release")
        with self.assertRaisesRegex(RuntimeError, "Refusing to overwrite"):
            self.package()
        self.assertEqual(archive.read_bytes(), b"existing release")

    def test_rejects_unsafe_paths_and_non_application_executables(self):
        for name in ("../outside.txt", "/outside.txt", "C:/outside.txt", "Sys\\outside.txt",
                     "Sys/QA/results.json", "Sys/tests.exe", "Updater.exe", "Qt6Cored.dll"):
            with self.subTest(name=name), self.assertRaises(RuntimeError):
                packager.validate_release_path(name)


if __name__ == "__main__":
    unittest.main()
