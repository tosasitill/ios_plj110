#!/usr/bin/env python3
"""Build iOS camera bridge and optional public OPPO sensor tables; no device IO.

Vendor tables are generated local artifacts (GPL-2.0-only, OPPO notices kept),
not relicensed/copied into the tracked XNU overlay. Missing vendor root builds
an explicitly unavailable sensor backend, never reuses stale generated tables.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
GENERATED = HERE.parent / 'kernel/generated'


def write_changed(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != data:
        path.write_text(data)


def sensor_tables(root, out):
    lines = ['/* Generated from public OPPO/MediaTek sensor register data.',
             ' * Original licenses/notices follow. Not for git. */']
    manifest = {}
    for sensor in ('hhmain', 'hhfront'):
        text = ''
        if root:
            p = root / f'{sensor}_mipi_raw/{sensor}_Sensor_setting.h'
            text = p.read_text()
            license_id = 'GPL-2.0-only' if sensor == 'hhmain' else 'GPL-2.0'
            if not re.search(r'SPDX-License-Identifier:\s*' + re.escape(license_id) + r'\s*(?:\*/)?\s*\n', text):
                raise ValueError(f'{p}: unrecognized provenance')
            lines.append(text.split('/*****', 1)[0])
            manifest[sensor] = {'source': str(p), 'license': license_id,
                                'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
        for mode in ('init', 'preview'):
            name = f'{sensor}_{mode}'
            values = []
            if root:
                stripped = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)
                m = re.search(r'\b' + name + r'_setting\s*\[\s*\]\s*=\s*\{([^{}]*)\}', stripped)
                if not m or '#' in m[1]:
                    raise ValueError(f'{name}: missing/conditional table')
                tokens = [s.strip() for s in m[1].split(',') if s.strip()]
                if any(not re.fullmatch(r'0x[0-9a-fA-F]+|[0-9]+', t) for t in tokens):
                    raise ValueError(f'{name}: not a literal register table')
                values = [int(t, 0) for t in tokens]
                if not values or len(values) % 2 or len(values) > 4096:
                    raise ValueError(f'{name}: invalid size')
                for reg, val in zip(values[::2], values[1::2]):
                    if (not 0 <= reg <= 65535 or not 0 <= val <= 255 or
                            reg in (0x0016, 0x0017, 0x0100, 0x0101, 0x0103, 0x0104) or
                            0x0a00 <= reg <= 0x0aff):
                        raise ValueError(f'{name}: unsafe/non-byte/stream/reset/hold/OTP register pair')
                manifest[sensor][mode + '_pairs'] = len(values) // 2
            lines.append(f'#define X9_{name.upper()}_COUNT {len(values)}u')
            lines.append(f'static const uint16_t x9_{name}[] = {{')
            for i in range(0, len(values), 16):
                lines.append(','.join(f'0x{v:04x}' for v in values[i:i+16]) + ',')
            if not values:
                lines.append('0 /* unavailable */')
            lines.append('};')
    write_changed(GENERATED / 'x9_camera_sensor_tables.inc', '\n'.join(lines) + '\n')
    (out / 'sensor-tables.json').write_text(json.dumps(manifest, indent=2) + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--developer-dir', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--vendor-root', type=Path, help='OPPO modules/vendor/oplus/kernel/camera/imgsensor/common')
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    sensor_tables(a.vendor_root, a.out)
    clang = a.developer_dir / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    sdk = a.developer_dir / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
    common = [str(clang), '-target', 'arm64-apple-ios16.0', '-isysroot', str(sdk),
              '-Os', '-fblocks', '-Wall', '-Wextra', '-Wno-deprecated-declarations']
    library = a.out / 'libx9-camera.dylib'
    subprocess.run(common + ['-dynamiclib', '-framework', 'Foundation', '-framework', 'CoreMedia',
                            '-framework', 'CoreVideo', '-framework', 'IOSurface',
                            '-install_name', '/usr/lib/libx9-camera.dylib',
                            str(HERE / 'media_bridge.m'), str(HERE / 'camera_device.c'),
                            str(HERE / 'camera_isp.c'), '-o', str(library)], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(library)], check=True)
    ctl = a.out / 'cameractl'
    subprocess.run(common + [str(HERE / 'cameractl.c'), '-o', str(ctl)], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(ctl)], check=True)
    data = library.read_bytes()
    if not data or len(data) > 512 * 1024:
        raise ValueError('camera library outside 512 KiB embedding budget')
    sha = hashlib.sha256(data).digest()
    path = f'/private/var/x9/camera/libx9-camera-{sha.hex()[:16]}.dylib'
    lines = ['/* Generated own/MIT-code bridge. Not for git. */',
             f'#define X9_CAMERA_BRIDGE_PATH "{path}"',
             'static const uint8_t x9_camera_bridge_sha[32] = {' + ','.join(str(b) for b in sha) + '};',
             'static const uint8_t x9_camera_bridge_data[] = {']
    lines += [','.join(str(b) for b in data[i:i+24]) + ',' for i in range(0, len(data), 24)]
    lines.append('};')
    write_changed(GENERATED / 'x9_camera_bridge.inc', '\n'.join(lines) + '\n')
    (a.out / 'camera-bridge.json').write_text(json.dumps({
        'bytes': len(data), 'sha256': sha.hex(), 'ios_path': path,
        'profile': '20H392 / CMCapture 45d37488-2cc7-3028-8a4d-936bce7a220b',
        'vendor_tables': bool(a.vendor_root), 'platform_adapter': False,
        'sensor_probe_backend': 'MT6993 cold AP FIFO/WL28681C; shared providers required',
        'sensor_standby_backend': 'explicit mode3 cold init/preview + stable readback + restore',
        'sensor_factory_calibration': False,
        'hardware_tested': False}, indent=2) + '\n')
    print(f'X9 camera: {len(data)} bytes; explicit cold probe/standby, no capture adapter; install only with x9_camera=2', flush=True)


if __name__ == '__main__':
    main()
