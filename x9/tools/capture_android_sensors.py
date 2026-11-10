#!/usr/bin/env python3
"""Read-only PLJ110 GNSS/SCP baseline; private output under .local only.

Usage: X9_SERIAL=<serial> python3 x9/tools/capture_android_sensors.py
Does not enable GPS/sensors, change properties, use /dev/mem, reboot, write a
device file, or pull unrelated data. A missing PRESSURE entry is conclusive
only when the full Android sensorservice list was actually captured.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
PRIVATE = ROOT / ".local"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = (args.out or PRIVATE / "x9" / "sensor-baseline" / stamp).resolve()
    if not out.is_relative_to(PRIVATE.resolve()):
        parser.error("Output must be inside this checkout's private .local directory")
    if out.exists():
        parser.error("Refusing to overwrite an existing capture")
    adb = shutil.which("adb")
    if not adb:
        parser.error("adb not found")
    device = ["-s", os.environ["X9_SERIAL"]] if os.environ.get("X9_SERIAL") else []
    # Stop before creating an apparent baseline when there is no selected,
    # authorized Android device. Never pick an arbitrary attached device.
    try:
        state = subprocess.run([adb, *device, "get-state"], capture_output=True, timeout=10)
    except subprocess.TimeoutExpired:
        parser.error("adb get-state timed out")
    if state.returncode or state.stdout.strip() != b"device":
        print("No selected authorized Android device; no capture created. "
              "Connect Android with ADB, and set X9_SERIAL if multiple devices are attached.", file=sys.stderr)
        return 2
    out.mkdir(parents=True)
    os.chmod(out, 0o700)
    records = []

    def capture(name, command, root=False, timeout=30):
        argv = [adb, *device, "exec-out"] + (["su", "-c", command] if root else ["sh", "-c", command])
        try:
            result = subprocess.run(argv, capture_output=True, timeout=timeout)
            code, data, errors = result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired as error:
            code, data, errors = 124, error.stdout or b"", error.stderr or b""
        (out / name).write_bytes(data)
        (out / (name + ".stderr")).write_bytes(errors)
        os.chmod(out / name, 0o600)
        os.chmod(out / (name + ".stderr"), 0o600)
        records.append({"file": name, "command": command, "root": root, "exit": code,
                        "bytes": len(data), "complete": code == 0})
        print(f"{name}: exit={code}, bytes={len(data)}")
        return code, data

    capture("platform.txt", "getprop ro.product.device; getprop ro.board.platform; "
            "getprop ro.build.version.release; getprop ro.build.display.id; uname -r")
    sensors_code, sensors = capture("sensorservice.txt", "dumpsys sensorservice", timeout=45)
    capture("device-tree.dtb", "cat /sys/firmware/fdt", root=True)
    capture("gnss-scp-kernel.txt", "dmesg | grep -i -E 'gps_mcudl|GDLX|GDL-|gpsmdl|"
            "sensorhub|sensor_ready|sensor_list|ipi_comm|share_buf|\\[SCP\\]'", root=True)
    capture("gnss-firmware-files.txt", "for d in /vendor/firmware /odm/firmware "
            "/odm/firmware/oplus_direct_load_kernel /vendor/firmware_mnt; do "
            "[ ! -d \"$d\" ] || find \"$d\" -maxdepth 2 -type f; done | "
            "grep -i -E 'gps|gnss|mnl|scp|sensorhub'", root=True)
    capture("gnss-program-files.txt", "ls -l /vendor/bin/mnld /vendor/bin/hw/*gnss* "
            "/vendor/lib64/*mnl* /vendor/lib64/*gps* /vendor/lib64/*gnss* 2>/dev/null", root=True)
    # scp_helper.c registers safe status/reset-count attributes on misc scp.
    # Do NOT read *_awake_lock/unlock or *_reg_status: despite their RO file
    # modes their show handlers wake/change the SCP hardware state.
    capture("scp-status.txt", "for d in /sys/class/misc/scp "
            "/sys/devices/platform/1d000000.scp "
            "/sys/bus/platform/devices/1d000000.scp; do "
            "[ ! -d \"$d\" ] || { ls -l \"$d\"; "
            "for n in scp_A_status scp_reset_counts; do "
            "[ ! -f \"$d/$n\" ] || { echo \"$n\"; cat \"$d/$n\"; }; done; }; done", root=True)
    capture("module-parameters.txt", "for d in /sys/module/gps_drv_dl_v062/parameters "
            "/sys/module/scp/parameters /sys/module/sensorhub/parameters; do "
            "[ ! -d \"$d\" ] || for p in \"$d\"/*; do "
            "[ ! -f \"$p\" ] || { echo \"$p\"; cat \"$p\"; }; done; done", root=True)
    manifest = {"captured_at": stamp, "read_only": True, "sensorservice_exit": sensors_code,
                "sensorservice_nonempty": bool(sensors.strip()), "captures": records,
                "physical_pressure_present": "unclassified: inspect sensor list, not keyword counts",
                "firmware_pulled": False}
    (out / "capture.json").write_text(json.dumps(manifest, indent=2) + "\n")
    os.chmod(out / "capture.json", 0o600)
    print(f"Private baseline: {out}")
    return 0 if sensors_code == 0 and sensors.strip() else 2

if __name__ == "__main__":
    raise SystemExit(main())
