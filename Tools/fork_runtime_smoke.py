"""Use Dolphin's TAS input to inspect the menu without depending on physical controller timing."""
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build-win-release/python-tools"))
from pywinauto import Application

profile = ROOT / "build-candidate/qa/menu-runtime"
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
    assert render is not None, "Wii Menu did not boot"
    time.sleep(5)
    main.menu_select("Movie->TAS Input")
    time.sleep(1)
    tas = next(x for x in app.windows() if x.window_text().startswith("Wii TAS Input 1"))
    boxes = tas.descendants(control_type="CheckBox")
    print("TAS controls:", [(x.window_text(), x.get_toggle_state()) for x in boxes], flush=True)
    a = next(x for x in boxes if x.window_text() == "A")
    a.toggle()
    time.sleep(2)
    a.toggle()
    render.set_focus()
    time.sleep(6)
    render.capture_as_image().save(profile.parent / "wii-menu-after-tas.png")
    print("Captured Wii Menu after an emulated A press", flush=True)
    fields = tas.descendants(control_type='Spinner')
    print('IR SPINNERS:', [(x.window_text(), x.iface_value.CurrentValue) for x in fields], flush=True)
    if len(fields) >= 2:
        fields[0].iface_value.SetValue('400')
        fields[1].iface_value.SetValue('640')
        print('IR VALUES:', [(x.window_text(), x.iface_value.CurrentValue) for x in fields[:2]], flush=True)
    render.set_focus();time.sleep(2)
    render.capture_as_image().save(profile.parent / 'channel-hover.png')
    a.toggle();time.sleep(1);a.toggle();time.sleep(3)
    render.set_focus();render.capture_as_image().save(profile.parent / 'channel-preview.png')
    fields[0].iface_value.SetValue('660')
    fields[1].iface_value.SetValue('270')
    time.sleep(2)  # Let the emulated pointer settle before the A edge.
    a.toggle();time.sleep(1);a.toggle()
    launched = False
    for _ in range(30):
        names = [w.window_text() for w in app.windows()]
        if any('Mario Kart Wii' in name for name in names):
            launched = True
            break
        time.sleep(1)
    print('CHANNEL LAUNCH:', launched, names, flush=True)
    if not launched:
        render.set_focus();render.capture_as_image().save(profile.parent / 'channel-launch-failed.png')
    assert launched, 'The selected channel did not launch Mario Kart Wii'
    time.sleep(3)
    for w in app.windows():
        if 'Mario Kart Wii' in w.window_text():
            w.set_focus();w.capture_as_image().save(profile.parent / 'channel-launch.png')

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
