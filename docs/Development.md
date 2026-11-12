# Building Better Wii Menu DE

The Windows build uses CMake, Ninja, a Windows 10.0.22621 or newer SDK, and the Microsoft compiler and runtime packages pinned in `Tools/fork-msvc-lock.json`. Initialize submodules before building:

```powershell
git submodule update --init --recursive
python Tools/setup_fork_compiler.py
```

From a Visual Studio developer command prompt with the Windows SDK available:

```bat
build_fork.cmd
build-candidate\Binaries\Tests\tests.exe
```

Run the packaging checks and create the download:

```powershell
python -m unittest discover -s Tools/tests -p test_package_fork.py -v
python Tools/package_fork.py
```

The packager creates one versioned ZIP under `dist/`, containing `BetterWiiMenuDE.exe`, required release libraries, system data, licenses, and setup instructions. It rejects missing dependencies, development files, and existing output paths. The archive's build manifest records its source revision and payload hashes. Keep diagnostics and test profiles outside the download.

The Windows GitHub Actions workflow builds the app, runs the C++ and packaging tests, and uploads the application ZIP. Build logs and test results are retained as a separate diagnostic artifact. The workflow does not publish a GitHub release automatically.

## Runtime checks

The helpers `Tools/fork_gui_smoke.py`, `Tools/fork_runtime_smoke.py`, and `Tools/fork_autolaunch_smoke.py` exercise setup, channel launching, and optional startup. Use disposable profiles and your own game files; the helpers contain no games or personal Wii data. Check each script's arguments and profile requirements before running it.

Previous runtime checks covered Wii Menu 4.3U, Mario Kart Wii, and the USA/Asia Mario Party 9 disc. Capacity tests use synthetic candidates and repeated icons, so they do not establish compatibility with a full menu of distinct games. Additional Windows installations, games, controllers, and menu regions still need coverage.
