#!/usr/bin/env python3
"""Host-only checks for the unpublished Bluetooth PCM boundary.

Compile the real AudioServerPlugIn implementation on macOS and exercise its
publication/admission surface without Initialize, IOKit, DSP, or device access.
These checks do not emulate a codec, PCM callback, XPC peer, or audio route.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
AUDIO = ROOT / "x9/audio"
BLUETOOTH = ROOT / "x9/bluetooth"

HARNESS = r'''
#include "X9AudioPlugIn.c"
#include <assert.h>
#include <stdio.h>

static void expect_devices(void)
{
    AudioObjectPropertyAddress address = {
        kAudioPlugInPropertyDeviceList,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectID devices[4] = {0};
    UInt32 size = 0;
    assert(propertyData(kPlugIn, &address, 0, NULL, sizeof(devices), &size, devices) == 0);
    assert(size == 2 * sizeof(AudioObjectID));
    assert(devices[0] == kDevice && devices[1] == kHawking);
}

static void expect_unpublished_uid(const char *uid)
{
    CFStringRef value = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    assert(value != NULL);
    const void *query = value;
    AudioObjectPropertyAddress address = {
        kAudioPlugInPropertyTranslateUIDToDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectID object = kDevice;
    UInt32 size = 0;
    assert(propertyData(kPlugIn, &address, sizeof(query), &query,
                       sizeof(object), &size, &object) == 0);
    assert(size == sizeof(object));
    assert(object == kAudioObjectUnknown);
    CFRelease(value);
}

static void expect_no_bluetooth_io_admission(void)
{
    const AudioObjectID unpublished = 0x42425043; /* 'BBPC' */
    AudioServerPlugInIOCycleInfo cycle = {0};
    Boolean will = true, in_place = true;

    assert(StartIO(NULL, unpublished, 1) == kAudioHardwareBadObjectError);
    assert(StopIO(NULL, unpublished, 1) == kAudioHardwareBadObjectError);
    assert(WillDoIOOperation(NULL, unpublished, 1,
                            kAudioServerPlugInIOOperationReadInput,
                            &will, &in_place) == kAudioHardwareBadObjectError);
    assert(DoIOOperation(NULL, unpublished, kOutStream, 1,
                         kAudioServerPlugInIOOperationReadInput, 0,
                         &cycle, NULL, NULL) == kAudioHardwareBadObjectError);
}

int main(void)
{
    expect_devices();
    expect_unpublished_uid("Bluetooth");
    expect_unpublished_uid("HFPAudioDevice");
    expect_no_bluetooth_io_admission();
    puts("PASS: Speaker/Hawking only; unpublished Bluetooth has no IO admission");
    return 0;
}
'''


class BluetoothPCMPublicationBoundaryTests(unittest.TestCase):
    def test_real_plugin_does_not_publish_or_admit_bluetooth_pcm(self):
        out = ROOT / "x9/out"
        out.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="bt-audio-boundary-", dir=out) as tmp:
            work = Path(tmp)
            source = work / "boundary.c"
            binary = work / "boundary"
            source.write_text(HARNESS)
            subprocess.run([
                "xcrun", "--sdk", "macosx", "clang", "-std=c11", "-O2",
                "-Wall", "-Wextra", "-Werror", "-fblocks", "-I", str(AUDIO),
                str(source), "-framework", "CoreFoundation", "-framework",
                "CoreAudio", "-framework", "IOKit", "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_transport_and_build_metadata_remain_hci_only(self):
        transport = (BLUETOOTH / "X9H4Transport.c").read_text()
        self.assertIn("*uptr(0x100844180)!=address(NATIVE_WRITE)", transport)
        self.assertNotIn("DoIOOperation", transport)
        self.assertNotIn("AudioServerPlugIn", transport)
        for name in ("build.py", "candidate.py", "package.py"):
            with self.subTest(metadata=name):
                self.assertIn("'audio_verified': False",
                              (BLUETOOTH / name).read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2)
