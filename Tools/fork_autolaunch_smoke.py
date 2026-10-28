"""Check startup opt-in, cancellation, persistence, and countdown using disposable profiles."""
import argparse
import configparser
from contextlib import contextmanager
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build-win-release/python-tools"))
from pywinauto import Application
import win32api
import win32con
import win32gui
import win32process

TITLE = "Automatic Wii Menu Launch"
OPTION = "Automatically Launch Wii Menu on Startup"


def configure(profile, enabled=None, covers=False):
    path = profile / "Config/Dolphin.ini"
    path.parent.mkdir(parents=True, exist_ok=True)
    ini = configparser.ConfigParser()
    ini.optionxform = str
    ini.read(path, encoding="utf-8")
    for section in ("General", "Core", "Display", "AutoUpdate"):
        if not ini.has_section(section):
            ini.add_section(section)
    if enabled is not None:
        ini["General"]["WiiMenuAutoLaunch"] = str(enabled)
    ini["General"]["UseGameCovers"] = str(covers)
    ini["Core"]["ConfirmStop"] = "False"
    ini["Display"]["Fullscreen"] = "False"
    ini["AutoUpdate"]["UpdateTrack"] = ""
    with path.open("w", encoding="utf-8") as stream:
        ini.write(stream)


def preference(profile):
    ini = configparser.ConfigParser()
    ini.read(profile / "Config/Dolphin.ini", encoding="utf-8")
    return ini.getboolean("General", "WiiMenuAutoLaunch", fallback=False)


def wait_for(get, timeout=15):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = get()
        if value:
            return value
        time.sleep(0.05)
    raise AssertionError("Timed out waiting for UI condition")


class Session:
    def __init__(self, exe, profile, extra):
        self.process = subprocess.Popen([str(exe), "-u", str(profile), *extra])
        self.app = Application(backend="uia").connect(process=self.process.pid, timeout=30)
        self.main = wait_for(lambda: next((h for h, title in self.windows()
                                         if title.startswith("Better Wii Menu DE") and
                                         not win32gui.GetWindow(h, win32con.GW_OWNER)), None))

    def windows(self):
        found = []
        def visit(handle, _):
            if (win32process.GetWindowThreadProcessId(handle)[1] == self.process.pid
                    and win32gui.IsWindowVisible(handle)):
                found.append((handle, win32gui.GetWindowText(handle)))
        win32gui.EnumWindows(visit, None)
        return found

    def window(self, title):
        handle = next((h for h, text in self.windows() if text == title), None)
        return self.app.window(handle=handle) if handle else None

    def main_window(self):
        return self.app.window(handle=self.main)

    def toggle_context_option(self):
        main = self.main_window()
        entry = wait_for(lambda: next((x for x in main.descendants()
                                      if x.window_text() == "Wii Menu" and
                                      x.element_info.control_type in ("DataItem", "ListItem")), None))
        entry.iface_selection_item.Select()
        point = entry.rectangle().mid_point()
        client = win32gui.ScreenToClient(self.main, (point.x, point.y))
        position = win32api.MAKELONG(*client)
        win32gui.PostMessage(self.main, win32con.WM_RBUTTONDOWN, win32con.MK_RBUTTON, position)
        win32gui.PostMessage(self.main, win32con.WM_RBUTTONUP, 0, position)
        def find_option():
            for handle, _ in self.windows():
                controls = self.app.window(handle=handle).descendants(control_type="MenuItem")
                option = next((x for x in controls if x.window_text() == OPTION), None)
                if option:
                    return option
        wait_for(find_option).invoke()


@contextmanager
def session(exe, profile, extra=()):
    app = Session(exe, profile, extra)
    try:
        yield app
        wait_for(lambda: win32gui.IsWindowEnabled(app.main))
        time.sleep(0.2)  # Allow modal-dialog completion to unwind before posting Close.
        win32gui.PostMessage(app.main, win32con.WM_CLOSE, 0, 0)
        app.process.wait(timeout=20)
        assert app.process.returncode == 0, app.process.returncode
    finally:
        if app.process.poll() is None:
            app.process.terminate()
            app.process.wait(timeout=15)


def run(exe, output, installed_profile, game):
    output.mkdir(parents=True, exist_ok=False)
    result = {}
    fresh = output / "fresh"
    configure(fresh)
    with session(exe, fresh) as app:
        time.sleep(3.3)
        assert app.window(TITLE) is None
        app.toggle_context_option()
        wait_for(lambda: preference(fresh))
        assert app.window(TITLE) is None, "Enabling must apply on next startup"
    result["default_off_and_context_enable"] = True
    print("PASS default off and right-click enable", flush=True)

    with session(exe, fresh) as app:
        dialog = wait_for(lambda: app.window(TITLE))
        assert "3 seconds" in [x.window_text() for x in dialog.descendants(control_type="Text")]
        dialog.child_window(title="Automatically launch Wii Menu on startup",
                            control_type="CheckBox").toggle()
        wait_for(lambda: app.window(TITLE) is None)
        assert not preference(fresh)
        time.sleep(3.3)
        assert app.window("Set up the Wii Menu") is None
    result["countdown_disable_saved_immediately"] = True
    print("PASS countdown disable cancels and saves", flush=True)

    configure(fresh, covers=True)
    with session(exe, fresh) as app:
        time.sleep(3.3)
        assert app.window(TITLE) is None
        app.toggle_context_option()
        wait_for(lambda: preference(fresh))
    result["disable_survives_restart_and_grid_context_reenable"] = True
    print("PASS restart stays disabled and cover right-click re-enables", flush=True)

    with session(exe, fresh) as app:
        dialog = wait_for(lambda: app.window(TITLE))
        start = time.monotonic()
        seen = set()
        while app.window(TITLE):
            seen.update(x.window_text() for x in dialog.descendants(control_type="Text"))
            time.sleep(0.05)
        elapsed = time.monotonic() - start
        assert {"3 seconds", "2 seconds", "1 second"} <= seen, seen
        assert elapsed >= 2.7, elapsed
        setup = wait_for(lambda: app.window("Set up the Wii Menu"))
        setup.child_window(title="Cancel", control_type="Button").invoke()
        wait_for(lambda: app.window("Set up the Wii Menu") is None)
        assert preference(fresh)
    result["countdown_3_2_1_then_fresh_setup"] = {"seconds_observed": elapsed}
    print("PASS visible 3-2-1 countdown and fresh-profile setup", flush=True)

    with session(exe, fresh) as app:
        dialog = wait_for(lambda: app.window(TITLE))
        wait_for(lambda: any(x.window_text() == "1 second"
                             for x in dialog.descendants(control_type="Text")))
        dialog.child_window(title="Automatically launch Wii Menu on startup",
                            control_type="CheckBox").toggle()
        time.sleep(1.3)
        assert not preference(fresh)
        assert app.window("Set up the Wii Menu") is None
    result["last_second_disable"] = True
    print("PASS last-second cancellation", flush=True)

    configure(fresh, enabled=True)
    with session(exe, fresh) as app:
        dialog = wait_for(lambda: app.window(TITLE))
        win32gui.PostMessage(dialog.handle, win32con.WM_CLOSE, 0, 0)
        wait_for(lambda: app.window(TITLE) is None)
        time.sleep(3.3)
        assert preference(fresh)
        assert app.window("Set up the Wii Menu") is None
    result["close_skips_current_launch_without_disabling"] = True
    print("PASS closing countdown skips only this launch", flush=True)

    if installed_profile:
        installed = output / "installed"
        shutil.copytree(installed_profile, installed,
                        ignore=shutil.ignore_patterns("*.rvz", "*.iso", "ScreenShots", "Cache",
                                                     "Logs", "*evidence*", "background-native-test"))
        configure(installed, enabled=True)
        with session(exe, installed) as app:
            wait_for(lambda: app.window(TITLE))
            wait_for(lambda: any("Wii Menu (0000" in title for _, title in app.windows()), timeout=40)
            assert app.window(TITLE) is None
            time.sleep(3)
            app.main_window().child_window(title="Stop", control_type="Button").invoke()
            wait_for(lambda: not any("Wii Menu (0000" in title for _, title in app.windows()))
            time.sleep(3.3)
            assert app.window(TITLE) is None, "Auto-launch must only run at startup"
        result["installed_menu_auto_boot"] = True
        print("PASS installed Wii Menu auto-boots", flush=True)
        if game:
            with session(exe, installed, ("--exec", str(game))) as app:
                end = time.monotonic() + 5
                while time.monotonic() < end:
                    assert app.window(TITLE) is None, "Explicit game boot must take priority"
                    time.sleep(0.1)
                assert app.main_window().child_window(title="Stop", control_type="Button").is_enabled()
                app.main_window().child_window(title="Stop", control_type="Button").invoke()
            result["explicit_game_boot_priority"] = True
            print("PASS explicit game boot skips countdown", flush=True)
    (output / "results.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--installed-profile", type=Path)
    parser.add_argument("--game", type=Path)
    args = parser.parse_args()
    run(args.exe.resolve(), args.output.resolve(), args.installed_profile, args.game)
