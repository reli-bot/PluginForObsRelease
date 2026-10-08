"""End-to-end test: run the real OBS Studio 31.0.4 (portable) with this module on Windows.

Run 1 loads a scene collection whose media source plays a sine tone through two
"atkAudio PluginHost" filters (AtkProbeA, then AtkProbeB). AtkProbeA's window is restored
open. AtkProbeA turns its own gain from 0.5 to 0.25 after three seconds; OBS saves the
collection when it is closed. Run 2 starts OBS again on the saved collection and checks that
the gain of 0.25 came back.

Checks: the module loads in OBS 31.0.4, VST3 plugins are instantiated from the saved state,
audio really flows through both hosted plugins in order (B's input == A's output), the editor
window opens, the plugin state survives an OBS save/restart, and OBS shuts down cleanly with
both plugins destroyed.
"""

import argparse
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import urllib.request
import uuid
import wave
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

OBS_ZIP_URL = "https://github.com/obsproject/obs-studio/releases/download/31.0.4/OBS-Studio-31.0.4-Windows.zip"
OBS_API_VER_31_0_4 = (31 << 24) | (0 << 16) | 4
MODULE = "atkaudio-pluginforobs"
FILTER_ID = "atkaudio_plugin_host"
TONE_AMPLITUDE = 0.5
TONE_RMS = TONE_AMPLITUDE / math.sqrt(2.0)

failures = []
passes = []


def check(condition, message):
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {message}", flush=True)
    (passes if condition else failures).append(message)
    return condition


def _annotation_escape(text):
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def emit_annotations():
    """Report results as GitHub annotations (readable through the checks API)."""
    if not os.environ.get("GITHUB_ACTIONS"):
        return
    chunk = []
    size = 0
    for line in ["PASS " + p for p in passes]:
        if size + len(line) > 3500:
            print(f"::notice title=OBS 31.0.4 test::{_annotation_escape(chr(10).join(chunk))}")
            chunk, size = [], 0
        chunk.append(line)
        size += len(line) + 1
    if chunk:
        print(f"::notice title=OBS 31.0.4 test::{_annotation_escape(chr(10).join(chunk))}")
    for f in failures[:9]:
        print(f"::error title=OBS 31.0.4 test failure::{_annotation_escape(f)}")


def ps(script):
    return subprocess.run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command", script], capture_output=True, text=True
    )


def screenshot(path):
    script = (
        "Add-Type -AssemblyName System.Windows.Forms,System.Drawing;"
        "$b=[System.Windows.Forms.SystemInformation]::VirtualScreen;"
        "$bmp=New-Object System.Drawing.Bitmap $b.Width,$b.Height;"
        "$g=[System.Drawing.Graphics]::FromImage($bmp);"
        "$g.CopyFromScreen($b.Left,$b.Top,0,0,$bmp.Size);"
        f"$bmp.Save('{path}',[System.Drawing.Imaging.ImageFormat]::Png)"
    )
    result = ps(script)
    print(f"screenshot -> {path} (rc={result.returncode}) {result.stderr.strip()[:300]}", flush=True)


def visible_windows(pid):
    """(hwnd, title, owner hwnd) of the visible top-level windows of a process."""
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    user32.GetWindow.restype = wintypes.HWND
    found = []
    proc_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def callback(hwnd, _):
        owner_pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner_pid))
        if owner_pid.value == pid and user32.IsWindowVisible(hwnd):
            length = user32.GetWindowTextLengthW(hwnd)
            buf = ctypes.create_unicode_buffer(length + 1)
            user32.GetWindowTextW(hwnd, buf, length + 1)
            owner = user32.GetWindow(hwnd, 4)  # GW_OWNER
            found.append((int(hwnd or 0), buf.value, int(owner or 0)))
        return True

    user32.EnumWindows(proc_type(callback), 0)
    return found


def post_close(hwnd):
    import ctypes

    return bool(ctypes.windll.user32.PostMessageW(hwnd, 0x0010, 0, 0))  # WM_CLOSE


def prepare_obs(work):
    obs_root = work / "obs"
    if (obs_root / "bin" / "64bit" / "obs64.exe").exists():
        return obs_root
    zip_path = work / "obs.zip"
    if not zip_path.exists():
        print(f"downloading {OBS_ZIP_URL}", flush=True)
        urllib.request.urlretrieve(OBS_ZIP_URL, zip_path)
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(obs_root)
    return obs_root


def install_module(obs_root, package):
    bin_dir = obs_root / "obs-plugins" / "64bit"
    for f in (package / "bin" / "64bit").iterdir():
        shutil.copy2(f, bin_dir / f.name)
    data_dst = obs_root / "data" / "obs-plugins" / MODULE
    if (package / "data").exists():
        shutil.copytree(package / "data", data_dst, dirs_exist_ok=True)
    (obs_root / "portable_mode.txt").write_text("")


def scan_plugin(scanner, bundle):
    result = subprocess.run([str(scanner), str(bundle)], capture_output=True, text=True, timeout=300)
    out = result.stdout
    start = out.find("<?xml")
    check(result.returncode == 0 and start >= 0, f"scanner exit 0 and XML for {bundle.name} (rc={result.returncode})")
    root = ET.fromstring(out[start:])
    check(root.get("success") in ("1", "true"), f"scanner reports success for {bundle.name}")
    plugins = root.findall("PLUGIN")
    check(len(plugins) >= 1, f"scanner found {len(plugins)} plugin(s) in {bundle.name}")
    return ET.tostring(plugins[0], encoding="unicode")


def make_state(plugin_xml, window_open):
    xml = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<state editor_style="0">{plugin_xml}<inner_state></inner_state></state>'
    )
    header = (
        "MULTICORE:0\n"
        f"DOCKVISIBLE:{1 if window_open else 0}\n"
        "DOCKFLOATING:1\nDOCKAREA:0\nDOCKGEOM:120,120,0,0\n"
    )
    return header + xml


def write_tone(path, seconds=4, rate=48000, freq=440.0):
    frames = int(seconds * rate)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        data = bytearray()
        for i in range(frames):
            v = int(max(-1.0, min(1.0, TONE_AMPLITUDE * math.sin(2 * math.pi * freq * i / rate))) * 32767)
            data += struct.pack("<hh", v, v)
        w.writeframes(bytes(data))


def write_config(obs_root, tone, state_a, state_b):
    cfg = obs_root / "config" / "obs-studio"
    (cfg / "basic" / "scenes").mkdir(parents=True, exist_ok=True)
    (cfg / "basic" / "profiles" / "AtkTest").mkdir(parents=True, exist_ok=True)

    (cfg / "global.ini").write_text(
        "[General]\n"
        f"LastVersion={OBS_API_VER_31_0_4}\n"
        "Pre31Migrated=true\n"
        "EnableAutoUpdates=false\n"
        f"InfoLastVersion={OBS_API_VER_31_0_4}\n",
        encoding="utf-8",
    )
    (cfg / "user.ini").write_text(
        "[General]\nFirstRun=true\n"
        "[Basic]\nProfile=AtkTest\nProfileDir=AtkTest\nSceneCollection=AtkTest\nSceneCollectionFile=AtkTest\n"
        "ConfigOnNewProfile=false\n",
        encoding="utf-8",
    )
    (cfg / "basic" / "profiles" / "AtkTest" / "basic.ini").write_text(
        "[General]\nName=AtkTest\n"
        "[Video]\nBaseCX=640\nBaseCY=360\nOutputCX=640\nOutputCY=360\nFPSType=0\nFPSCommon=30\n"
        "[Audio]\nSampleRate=48000\nChannelSetup=Stereo\n",
        encoding="utf-8",
    )

    tone_uuid = str(uuid.uuid4())
    scene_uuid = str(uuid.uuid4())

    def filt(name, state):
        return {
            "name": name,
            "uuid": str(uuid.uuid4()),
            "id": FILTER_ID,
            "versioned_id": FILTER_ID,
            "enabled": True,
            "mixers": 0,
            "settings": {FILTER_ID: state},
        }

    collection = {
        "name": "AtkTest",
        "current_scene": "Scene",
        "current_program_scene": "Scene",
        "scene_order": [{"name": "Scene"}],
        "groups": [],
        "quick_transitions": [],
        "transitions": [],
        "saved_projectors": [],
        "current_transition": "Fade",
        "transition_duration": 300,
        "sources": [
            {
                "name": "Scene",
                "uuid": scene_uuid,
                "id": "scene",
                "versioned_id": "scene",
                "enabled": True,
                "mixers": 0,
                "settings": {
                    "id_counter": 1,
                    "custom_size": False,
                    "items": [
                        {
                            "name": "Tone",
                            "source_uuid": tone_uuid,
                            "visible": True,
                            "locked": False,
                            "id": 1,
                            "pos": {"x": 0.0, "y": 0.0},
                            "scale": {"x": 1.0, "y": 1.0},
                            "rot": 0.0,
                            "align": 5,
                            "bounds_type": 0,
                            "bounds": {"x": 0.0, "y": 0.0},
                            "bounds_align": 0,
                            "crop_left": 0,
                            "crop_top": 0,
                            "crop_right": 0,
                            "crop_bottom": 0,
                        }
                    ],
                },
            },
            {
                "name": "Tone",
                "uuid": tone_uuid,
                "id": "ffmpeg_source",
                "versioned_id": "ffmpeg_source",
                "enabled": True,
                "mixers": 255,
                "volume": 1.0,
                "muted": False,
                "monitoring_type": 0,
                "settings": {
                    "is_local_file": True,
                    "local_file": str(tone).replace("\\", "/"),
                    "looping": True,
                    "restart_on_activate": True,
                    "close_when_inactive": False,
                    "hw_decode": False,
                },
                "filters": [filt("Probe A", state_a), filt("Probe B", state_b)],
            },
        ],
    }
    path = cfg / "basic" / "scenes" / "AtkTest.json"
    path.write_text(json.dumps(collection, indent=2), encoding="utf-8")

    # Same scene without the module's filters: baseline for OBS' own leak count.
    baseline = json.loads(json.dumps(collection))
    baseline["name"] = "AtkBaseline"
    for src in baseline["sources"]:
        src.pop("filters", None)
    (cfg / "basic" / "scenes" / "AtkBaseline.json").write_text(json.dumps(baseline, indent=2), encoding="utf-8")
    return path


def run_obs(obs_root, out_dir, env_extra, seconds, collection="AtkTest", extra_args=()):
    out_dir.mkdir(parents=True, exist_ok=True)
    logs_dir = obs_root / "config" / "obs-studio" / "logs"
    before = set(logs_dir.glob("*.txt")) if logs_dir.exists() else set()

    env = dict(os.environ)
    env["ATK_PROBE_LOG_DIR"] = str(out_dir)
    env.pop("ATK_PROBE_SELF_SET", None)
    env.update(env_extra)

    exe = obs_root / "bin" / "64bit" / "obs64.exe"
    args = [
        str(exe),
        "--portable",
        "--multi",
        "--disable-updater",
        "--disable-missing-files-check",
        "--disable-shutdown-check",
        "--collection",
        collection,
        "--profile",
        "AtkTest",
        *extra_args,
    ]
    proc = subprocess.Popen(args, cwd=str(exe.parent), env=env)
    print(f"started OBS pid={proc.pid}", flush=True)

    deadline = time.time() + seconds
    shot_taken = False
    while time.time() < deadline:
        if proc.poll() is not None:
            break
        if not shot_taken and time.time() > deadline - seconds / 2:
            screenshot(out_dir / "screenshot-running.png")
            shot_taken = True
        time.sleep(1)

    check(proc.poll() is None, f"{out_dir.name}: OBS still running after {seconds}s")
    screenshot(out_dir / "screenshot-before-close.png")

    windows = visible_windows(proc.pid)
    print(f"visible windows: {windows}", flush=True)
    main = [w for w in windows if w[1].startswith("OBS ") and w[2] == 0]
    plugin_windows = [w for w in windows if w[1].startswith("Probe ")]
    if os.environ.get("GITHUB_ACTIONS"):
        listing = chr(10).join(f"{w[0]:#x} owner={w[2]:#x} '{w[1]}'" for w in windows)
        print(f"::notice title=OBS windows ({out_dir.name})::{_annotation_escape(listing)}")
    check(len(main) == 1, f"{out_dir.name}: found the OBS main window ({len(main)})")
    if main and plugin_windows:
        check(
            all(w[2] == main[0][0] for w in plugin_windows),
            "plugin window is owned by the OBS main window",
        )

    # Ask OBS to close like a user would (WM_CLOSE to the main window) so the scene collection
    # is saved and the module unload path runs.
    if main:
        print(f"WM_CLOSE -> {post_close(main[0][0])}", flush=True)
    try:
        rc = proc.wait(timeout=90)
    except subprocess.TimeoutExpired:
        screenshot(out_dir / "screenshot-hung-on-close.png")
        proc.kill()
        rc = None
    check(rc == 0, f"{out_dir.name}: OBS exited cleanly after close (exit code {rc})")

    after = set(logs_dir.glob("*.txt")) if logs_dir.exists() else set()
    new_logs = sorted(after - before, key=lambda p: p.stat().st_mtime)
    log_text = ""
    if new_logs:
        log_text = new_logs[-1].read_text(encoding="utf-8", errors="replace")
        shutil.copy2(new_logs[-1], out_dir / "obs-log.txt")
    check(bool(log_text), f"{out_dir.name}: OBS wrote a log file")
    if os.environ.get("GITHUB_ACTIONS") and log_text:
        keys = ("atkaudio", "atkAudio", "error", "Error", "Failed", "failed", "LoadLibrary", "warning", "crash")
        excerpt = [l for l in log_text.splitlines() if any(k in l for k in keys)][:60]
        text = chr(10).join(excerpt)[-3800:]
        print(f"::notice title=OBS log excerpt ({out_dir.name})::{_annotation_escape(text)}")
    return log_text


LEAKS_RE = re.compile(r"Number of memory leaks: (\d+)")


def leak_count(log_text):
    m = LEAKS_RE.search(log_text)
    return int(m.group(1)) if m else None


PROCESS_RE = re.compile(r"process blocks=(\d+) ch=(\d+) sr=(\d+) gain=([\d.]+) in_rms=([\d.]+) out_rms=([\d.]+)")


def probe_lines(path):
    if not path.exists():
        return [], ""
    text = path.read_text(encoding="utf-8", errors="replace")
    rows = []
    for m in PROCESS_RE.finditer(text):
        rows.append(
            {
                "blocks": int(m.group(1)),
                "ch": int(m.group(2)),
                "sr": int(m.group(3)),
                "gain": float(m.group(4)),
                "in": float(m.group(5)),
                "out": float(m.group(6)),
            }
        )
    return rows, text


def near(value, target, tol=0.2):
    return abs(value - target) <= tol * target


def check_obs_log(log_text, run):
    check(f"[{MODULE}] plugin loaded successfully" in log_text, f"{run}: module loaded in OBS 31.0.4")
    check("[atkAudio][PluginHost] loaded AtkProbeA" in log_text, f"{run}: AtkProbeA instantiated")
    check("[atkAudio][PluginHost] loaded AtkProbeB" in log_text, f"{run}: AtkProbeB instantiated")
    bad = [l for l in log_text.splitlines() if MODULE in l and ("Failed" in l or "not loaded" in l or "LoadLibrary" in l)]
    check(not bad, f"{run}: no load errors for the module {bad[:3]}")
    check(f"[{MODULE}] plugin unloaded" in log_text, f"{run}: module unloaded at shutdown")
    errors = [l for l in log_text.splitlines() if "[atkAudio]" in l and ("error" in l.lower() or "warning" in l.lower())]
    check(not errors, f"{run}: no atkAudio warnings/errors in the OBS log {errors[:5]}")


def saved_filter_states(collection_path):
    data = json.loads(collection_path.read_text(encoding="utf-8"))
    states = {}
    for src in data.get("sources", []):
        for f in src.get("filters", []) or []:
            if f.get("id") == FILTER_ID:
                states[f["name"]] = f.get("settings", {}).get(FILTER_ID, "")
    return states


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--package", required=True, type=Path, help="installed atkaudio-pluginforobs folder")
    ap.add_argument("--probes", required=True, type=Path, help="folder with AtkProbeA.vst3 and AtkProbeB.vst3")
    ap.add_argument("--work", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--seconds", type=int, default=25)
    a = ap.parse_args()

    # OBS runs with bin/64bit as its working directory: every path handed to it must be absolute.
    a.package = a.package.resolve()
    a.probes = a.probes.resolve()
    a.work = a.work.resolve()
    a.out = a.out.resolve()

    a.work.mkdir(parents=True, exist_ok=True)
    a.out.mkdir(parents=True, exist_ok=True)

    obs_root = prepare_obs(a.work)
    install_module(obs_root, a.package)

    scanner = a.package / "bin" / "64bit" / f"{MODULE}_scanner.exe"
    xml_a = scan_plugin(scanner, a.probes / "AtkProbeA.vst3")
    xml_b = scan_plugin(scanner, a.probes / "AtkProbeB.vst3")

    tone = a.work / "tone.wav"
    write_tone(tone)
    collection = write_config(obs_root, tone, make_state(xml_a, True), make_state(xml_b, False))

    # ---- run 0: OBS alone (third-party plugins disabled), for OBS' own memory-leak baseline ----
    log0 = run_obs(obs_root, a.out / "run0", {}, 15, collection="AtkBaseline", extra_args=("--only-bundled-plugins",))
    leaks0 = leak_count(log0)
    check(f"[{MODULE}]" not in log0, "run0: baseline ran without the module")

    # ---- run 1: fresh state, probe A changes its own gain to 0.25 after 3 s ----
    log1 = run_obs(obs_root, a.out / "run1", {"ATK_PROBE_SELF_SET": "1"}, a.seconds)
    check_obs_log(log1, "run1")
    leaks1 = leak_count(log1)
    check(
        leaks0 is not None and leaks1 is not None and leaks1 <= leaks0,
        f"run1: no extra OBS memory leaks with the module ({leaks1} vs baseline {leaks0})",
    )
    rows_a, text_a = probe_lines(a.out / "run1" / "probe1.log")
    rows_b, text_b = probe_lines(a.out / "run1" / "probe2.log")
    check(len(rows_a) >= 5 and len(rows_b) >= 5, f"run1: both probes processed audio ({len(rows_a)}/{len(rows_b)} s)")
    check("editor created" in text_a, "run1: AtkProbeA window (editor) opened from saved DOCKVISIBLE:1")
    if rows_a:
        check(rows_a[0]["sr"] == 48000 and rows_a[0]["ch"] == 2, f"run1: 48 kHz stereo ({rows_a[0]['sr']}, {rows_a[0]['ch']})")
        first = rows_a[0]
        check(near(first["in"], TONE_RMS), f"run1: A input RMS {first['in']:.4f} ~ tone {TONE_RMS:.4f}")
        check(first["gain"] == 0.5 and near(first["out"], TONE_RMS * 0.5), f"run1: A gain 0.5 -> out {first['out']:.4f}")
        check(any(r["gain"] == 0.25 for r in rows_a), "run1: A switched its gain to 0.25")
    if rows_b:
        check(near(rows_b[0]["in"], TONE_RMS * 0.5), f"run1: B input {rows_b[0]['in']:.4f} == A output (~{TONE_RMS * 0.5:.4f})")
        check(near(rows_b[-1]["in"], TONE_RMS * 0.25), f"run1: B input {rows_b[-1]['in']:.4f} follows A gain 0.25")
    check("destroyed" in text_a and "destroyed" in text_b, "run1: both plugins destroyed at shutdown")

    states = saved_filter_states(collection)
    st_a = states.get("Probe A", "")
    check("DOCKVISIBLE:1" in st_a, "run1: open window state saved (DOCKVISIBLE:1)")
    m = re.search(r"<inner_state>([^<]+)</inner_state>", st_a)
    check(m is not None and len(m.group(1)) > 16, "run1: AtkProbeA plugin state saved in the scene collection")
    shutil.copy2(collection, a.out / "run1" / "AtkTest-saved.json")

    # ---- run 2: restart on the saved collection; the 0.25 gain must be restored ----
    log2 = run_obs(obs_root, a.out / "run2", {}, a.seconds)
    check_obs_log(log2, "run2")
    leaks2 = leak_count(log2)
    check(
        leaks0 is not None and leaks2 is not None and leaks2 <= leaks0,
        f"run2: no extra OBS memory leaks with the module ({leaks2} vs baseline {leaks0})",
    )
    rows_a2, text_a2 = probe_lines(a.out / "run2" / "probe1.log")
    rows_b2, _ = probe_lines(a.out / "run2" / "probe2.log")
    check("setState gain=0.250" in text_a2, "run2: AtkProbeA state restored (gain 0.25)")
    check(bool(rows_a2) and all(r["gain"] == 0.25 for r in rows_a2), "run2: A processed with the restored gain from the start")
    if rows_b2:
        check(near(rows_b2[0]["in"], TONE_RMS * 0.25), f"run2: B input {rows_b2[0]['in']:.4f} ~ {TONE_RMS * 0.25:.4f}")

    summary = {"passed": passes, "failures": failures}
    (a.out / "summary.json").write_text(json.dumps(summary, indent=2))
    print(f"\n{len(passes)} passed, {len(failures)} failure(s)")
    for f in failures:
        print("  -", f)
    emit_annotations()
    return 1 if failures else 0


if __name__ == "__main__":
    # The OBS log contains box-drawing characters; the Windows console default (cp1252) cannot print them.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    try:
        sys.exit(main())
    except Exception:
        import traceback

        tb = traceback.format_exc()
        print(tb, flush=True)
        if os.environ.get("GITHUB_ACTIONS"):
            print(f"::error title=OBS test crashed::{_annotation_escape(tb[-3000:])}")
        emit_annotations()
        sys.exit(2)
