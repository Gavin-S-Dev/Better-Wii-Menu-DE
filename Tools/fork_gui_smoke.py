"""Inspect a candidate with an explicit disposable profile; never resets a global profile."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build-win-release" / "python-tools"))
from pywinauto import Application
import win32api
import win32con
import win32gui


def run(executable, profile, output):
    output.mkdir(parents=True, exist_ok=True)
    process = subprocess.Popen([str(executable), "-u", str(profile)])
    try:
        app = Application(backend="uia").connect(process=process.pid, timeout=30)
        time.sleep(5)
        window = app.top_window()
        window.wait("visible", timeout=25)
        controls = [(x.element_info.control_type, x.window_text()) for x in window.descendants()]
        (output / "controls.json").write_text(json.dumps(controls, indent=2), encoding="utf-8")
        window.capture_as_image().save(output / "library.png")
        print("WINDOW", window.window_text())
        print("LIBRARY ITEMS", [(kind, name) for kind, name in controls if kind in ["DataItem", "ListItem"]])
        assert any(name == "Wii Menu" for _, name in controls), "Missing Wii Menu library entry"
        menus = [x for x in window.descendants() if x.window_text() == "Wii Menu"]
        # A fresh profile must offer setup rather than failing to boot a nonexistent title.
        installed = (profile / "Wii/title/00000001/00000002/content/title.tmd").exists()
        if not installed:
            menus[0].iface_selection_item.Select()
            # Qt's synchronous accessibility invocation blocks on modal setup. Post the click
            # directly to our window instead; this never moves the user's mouse or steals focus.
            button = window.child_window(title="Play", control_type="Button").rectangle()
            point = win32gui.ScreenToClient(window.handle, (button.mid_point().x, button.mid_point().y))
            position = win32api.MAKELONG(*point)
            win32gui.PostMessage(window.handle, win32con.WM_LBUTTONDOWN, win32con.MK_LBUTTON, position)
            win32gui.PostMessage(window.handle, win32con.WM_LBUTTONUP, 0, position)
            dialog = None
            for _ in range(50):
                dialog = next((x for x in window.descendants(control_type="Window")
                               if x.window_text() == "Set up the Wii Menu"), None)
                if dialog is not None:
                    break
                time.sleep(0.2)
            assert dialog is not None, "Missing first-run setup dialog"
            print("FIRST RUN", dialog.window_text())
            dialog.capture_as_image().save(output / "setup.png")
            assert "Set up the Wii Menu" in dialog.window_text()
            next(x for x in dialog.descendants(control_type="Button")
                 if x.window_text() == "Cancel").invoke()
        window.close()
        process.wait(timeout=15)
        print("PASS")
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=15)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    run(args.exe.resolve(), args.profile.resolve(), args.output.resolve())
