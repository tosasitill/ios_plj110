#!/usr/bin/env python3
"""Build/sign own-code BT client and version-locked iOS16 H4 adapter, offline."""
import argparse
import importlib.util
import json
import plistlib
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def launch_jobs(source, out):
    """Derive candidate Program overrides; never install or edit stock jobs."""
    # Private original plists are explicit repo-local inputs, not implicit
    # dependencies on a mounted root or on another checkout's experiment.
    source = source.resolve()
    source.relative_to(HERE.parent.parent)
    jobs = (('com.apple.bluetoothd', '/usr/sbin/bluetoothd', [],
             'com.apple.server.bluetooth', '/usr/libexec/X9BluetoothLaunch'),
            ('com.apple.BlueTool', '/usr/sbin/BlueTool', ['-R'],
             'com.apple.BlueTool', '/usr/libexec/X9BlueTool'))
    originals = out / 'launchd-originals'
    candidates = out / 'launchd-candidates'
    validated = []
    # Check BOTH jobs before emitting ANY derived configuration.
    for label, program, args, service, replacement in jobs:
        data = (source / (label + '.plist')).read_bytes()
        job = plistlib.loads(data)
        if (not isinstance(job, dict) or job.get('Label') != label or
                job.get('UserName') != 'mobile' or
                job.get('ProgramArguments') != [program, *args] or
                job.get('Program', program) != program or
                not isinstance(job.get('MachServices'), dict) or
                service not in job['MachServices']):
            raise ValueError(f'Unexpected original 20H392 launch job: {label}')
        validated.append((label, data, job, replacement))
    originals.mkdir(parents=True, exist_ok=True)
    candidates.mkdir(parents=True, exist_ok=True)
    for label, data, job, replacement in validated:
        (originals / (label + '.plist')).write_bytes(data)
        # Program selects our exec gate/listener; the original arguments,
        # MachServices, KeepAlive, events, identity and all other keys remain.
        job['Program'] = replacement
        (candidates / (label + '.plist')).write_bytes(
            plistlib.dumps(job, fmt=plistlib.FMT_BINARY, sort_keys=False))
    return {'source': str(source.relative_to(HERE.parent.parent)),
            'originals': originals.name, 'candidates': candidates.name,
            'installed': False}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--developer-dir', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--launchd-source', type=Path,
                    help='explicit repo-local original 20H392 launch plist directory; emit candidates only')
    ap.add_argument('--kernel-objects', action='store_true',
                    help='also compile only Bluetooth/shared-power overlay objects, no link/boot/tests')
    a = ap.parse_args()
    dev = a.developer_dir
    if dev is None:
        dev = Path(json.loads((HERE.parent / 'build/local.json').read_text())['developer_dir'])
    sdk = dev / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
    clang = dev / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    a.out.mkdir(parents=True, exist_ok=True)
    binary = a.out / 'btctl'
    subprocess.run([str(clang), '-target', 'arm64-apple-ios16.0', '-isysroot', str(sdk),
                    '-Os', '-Wall', '-Wextra', '-Werror', '-framework', 'CoreFoundation',
                    '-framework', 'IOKit', str(HERE / 'btctl.c'), '-o', str(binary)], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(binary)], check=True)
    bridge = a.out / 'X9H4Transport.dylib'
    subprocess.run([str(clang), '-target', 'arm64-apple-ios16.0', '-isysroot', str(sdk),
                    '-Os', '-Wall', '-Wextra', '-Werror', '-dynamiclib',
                    '-install_name', '/usr/lib/X9H4Transport.dylib',
                    str(HERE / 'X9H4Transport.c'), str(HERE / 'X9Vendor16.c'),
                    str(HERE / 'X9Capabilities16.c'),
                    str(HERE / 'X9Bootstrap16.c'),
                    '-framework', 'CoreFoundation', '-framework', 'IOKit',
                    '-o', str(bridge)], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(bridge)], check=True)
    for name in ('X9BluetoothLaunch', 'X9BlueTool'):
        target = a.out / name
        subprocess.run([str(clang), '-target', 'arm64-apple-ios16.0', '-isysroot', str(sdk),
                        '-Os', '-Wall', '-Wextra', '-Werror', '-fblocks',
                        '-fstack-protector-strong', '-framework', 'CoreFoundation', '-framework', 'IOKit',
                        str(HERE / (name + '.c')), str(HERE / 'X9Bootstrap16.c'),
                        '-o', str(target)], check=True)
        subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(target)], check=True)
    launchd = launch_jobs(a.launchd_source, a.out) if a.launchd_source else None
    # Deliberately not a stageable runtime: vendor routing and complete board
    # initialization are separate required gates. No installed plist changes.
    (a.out / 'build.json').write_text(json.dumps({
        'schema': 1, 'client': 'btctl', 'bridge': 'X9H4Transport.dylib',
        'launch_gate': 'X9BluetoothLaunch', 'bluetool_adapter': 'X9BlueTool',
        'launchd': launchd,
        'ios_build': '20H392',
        'bluetoothd_uuid': 'AA9424E8-0648-34E2-B3CF-E6C793B9D471',
        'bluetoothd_sha256': '8b7c0a0bea51544db82269d7f2775ab7996b8740927283428c4753e032df3a8f',
        'stageable': False,
        'candidate_ready': False,
        'runtime_verified': False,
        'power_interface': 'kern.x9_bt_power',
        'power_lifecycle': 'closed-transport-only',
        'requires': ['current SSPM request/resources and observed 208MHz dev0/CMM profile readback; XNU handoff hardware-unverified',
                     'on-device completion of the embedded Android HAL constructors and preferred FC37 A2DP append; real function power-on',
                     'explicit effective Android UART_FC selection (x9_bt_fc); MTK SW PIO/escape implemented, hardware-unverified; Linux/HW unsupported',
                     'on-device validation of 20H392 capability publication and native cache routing',
                     'on-device deepsleep/firmware-own/resume validation; worker lifecycle and explicit script/reopen bridge implemented',
                     'launchd/BlueTool integration and on-device injection validation',
                     'on-device scan/pair/BLE/audio validation'],
        'transport_environment': {'BT_HCI_TRANSPORT': 'H4:POSIX', 'X9_BT_VENDOR': 'MT6653'},
    }, indent=2) + '\n')
    if a.kernel_objects:
        spec = importlib.util.spec_from_file_location('x9_builder', HERE.parent / 'build/build.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        cfg = module.load_config(HERE.parent / 'build/local.json')
        if a.developer_dir:
            cfg['developer_dir'] = str(a.developer_dir)
        subprocess.run([sys.executable, '-I', str(HERE.parent / 'tools/bluetooth_firmware.py'),
                        '--src', cfg.get('bluetooth_firmware') or cfg['wifi_firmware']], check=True)
        subprocess.run([sys.executable, '-I', str(HERE.parent / 'tools/bluetooth_board.py'),
                        '--required'], check=True)
        subprocess.run([sys.executable, '-I', str(HERE.parent / 'tools/bluetooth_vendor.py'),
                        '--required'], check=True)
        # Shared supply changes compile the actual Wi-Fi owner as well; use
        # its normal private generators, never a hand-written firmware stub.
        subprocess.run([sys.executable, '-I', str(HERE.parent / 'tools/wifi_firmware.py'),
                        '--src', cfg['wifi_firmware']], check=True)
        subprocess.run([sys.executable, '-I', str(HERE.parent / 'tools/wifi_nvram.py'),
                        '--src', cfg['wifi_nvram']], check=True)
        directory = (a.out / 'kernel').resolve()
        directory.mkdir(parents=True, exist_ok=True)
        builder = module.Builder(cfg, directory, False, 1)
        builder.objdir.mkdir(parents=True, exist_ok=True)
        try:
            for name in ('x9_conn_power.o', 'x9_bt_hardware.o', 'x9_bt_download.o',
                         'x9_bt_session.o', 'x9_bt_serial.o', 'x9_bluetooth.o',
                         'sleh.o', 'SweetX9NetworkStack.cpo', 'SweetX9Bluetooth.o'):
                entry = next(e for e in builder.recipe['objects'] if e['name'] == name)
                result = builder.compile_one(entry)
                if isinstance(result, str):
                    raise SystemExit(f'Bluetooth object compile failed: {result}')
        finally:
            builder.logfile.close()
    print(f'BT client, 20H392 H4/vendor adapter, launch gate and BlueTool built: {a.out}; not installed or hardware-tested')


if __name__ == '__main__':
    main()
