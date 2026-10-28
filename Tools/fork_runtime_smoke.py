"""Use Dolphin's TAS input to inspect the menu without depending on physical controller timing."""
import argparse
import ctypes
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build-win-release/python-tools"))
from pywinauto import Application

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--profile', type=Path, default=ROOT / 'build-candidate/qa/menu-runtime')
parser.add_argument('--game', default='Mario Kart Wii')
parser.add_argument('--tile-x', type=int, default=400)
parser.add_argument('--tile-y', type=int, default=640)
parser.add_argument('--preview-seconds', type=int, default=3)
parser.add_argument('--menu-seconds', type=int, default=6)
parser.add_argument('--menu-only', action='store_true')
parser.add_argument('--output', type=Path)
args = parser.parse_args()
profile = args.profile.resolve()
output = args.output.resolve() if args.output else profile / 'runtime-evidence'
output.mkdir(parents=True, exist_ok=True)
process = subprocess.Popen([str(ROOT / "build-candidate/Binaries/BetterWiiMenuDE.exe"), "-u", str(profile)])
try:
    app = Application(backend="uia").connect(process=process.pid, timeout=30)
    time.sleep(5)
    main = app.top_window()
    entry = next(x for x in main.descendants() if x.window_text() == "Wii Menu" and
                 x.element_info.control_type in ["ListItem", "DataItem"])
    entry.double_click_input()
    render = None
    for _ in range(35):
        render = next((x for x in app.windows() if "Wii Menu (0000" in x.window_text()), None)
        if render:
            break
        time.sleep(1)
    if render is None:
        for index, window in enumerate(app.windows()):
            print('BOOT WINDOW', window.window_text(),
                  [(x.element_info.control_type, x.window_text()) for x in window.descendants()
                   if x.element_info.control_type in ['Text', 'Button']], flush=True)
            window.capture_as_image().save(output / f'boot-window-{index}.png')
    assert render is not None, "Wii Menu did not boot"
    time.sleep(12)
    main.menu_select("Movie->TAS Input")
    time.sleep(1)
    tas = next(x for x in app.windows() if x.window_text().startswith("Wii TAS Input 1"))
    # Keep the TAS tool from obscuring render-window evidence captures.
    ctypes.windll.user32.MoveWindow(render.handle, 0, 0, 680, 540, True)
    ctypes.windll.user32.MoveWindow(tas.handle, 850, 0, 650, 1000, True)
    boxes = tas.descendants(control_type="CheckBox")
    print("TAS controls:", [(x.window_text(), x.get_toggle_state()) for x in boxes], flush=True)
    a = next(x for x in boxes if x.window_text() == "A")
    a.toggle()
    time.sleep(2)
    a.toggle()
    render.set_focus()
    time.sleep(args.menu_seconds)
    render.capture_as_image().save(output / "wii-menu-after-tas.png")
    print("Captured Wii Menu after an emulated A press", flush=True)
    dialogs = []
    for window in app.windows():
        if window.window_text() == 'Warning':
            text = [x.window_text() for x in window.descendants(control_type='Text')]
            dialogs.append(text)
            window.capture_as_image().save(output / 'warning.png')
    (output / 'warnings.json').write_text(json.dumps(dialogs, indent=2))
    assert not dialogs, f'Emulation raised warning dialogs: {dialogs}'
    if args.menu_only:
        main.child_window(title="Stop", control_type="Button").click_input()
        time.sleep(3)
        main.close()
        process.wait(timeout=15)
        sys.exit(0)
    fields = tas.descendants(control_type='Spinner')
    print('IR SPINNERS:', [(x.window_text(), x.iface_value.CurrentValue) for x in fields], flush=True)
    if len(fields) >= 2:
        fields[0].iface_value.SetValue(str(args.tile_x))
        fields[1].iface_value.SetValue(str(args.tile_y))
        print('IR VALUES:', [(x.window_text(), x.iface_value.CurrentValue) for x in fields[:2]], flush=True)
    render.set_focus();time.sleep(2)
    render.capture_as_image().save(output / 'channel-hover.png')
    a.toggle();time.sleep(1);a.toggle();time.sleep(args.preview_seconds)
    render.set_focus();render.capture_as_image().save(output / 'channel-preview.png')
    fields[0].iface_value.SetValue('660')
    fields[1].iface_value.SetValue('270')
    time.sleep(2)  # Let the emulated pointer settle before the A edge.
    a.toggle();time.sleep(1);a.toggle()
    launched = False
    for _ in range(30):
        names = [w.window_text() for w in app.windows()]
        if any(args.game in name for name in names):
            launched = True
            break
        time.sleep(1)
    print('CHANNEL LAUNCH:', launched, names, flush=True)
    if not launched:
        render.set_focus();render.capture_as_image().save(output / 'channel-launch-failed.png')
    assert launched, f'The selected channel did not launch {args.game}'
    time.sleep(3)
    for w in app.windows():
        if args.game in w.window_text():
            w.set_focus();w.capture_as_image().save(output / 'channel-launch.png')

    main.child_window(title="Stop", control_type="Button").click_input()
    time.sleep(3)
    for window in app.windows():
        for button in window.descendants(control_type="Button"):
            if button.window_text() == "Yes":
                button.click_input()
    main.close()
    process.wait(timeout=15)
finally:
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=15)
