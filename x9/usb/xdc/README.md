# X9Mtu3Xdci: MTU3 device controller behind Apple's USB device stack

Goal: a kernel `X9Mtu3Xdci : IOUSBDeviceController` drives the MT6993 MTU3 device
controller so the unmodified 20H392 `IOUSBDeviceFamily`, `AppleUSBDeviceMux`,
configd `USBDeviceConfiguration` and `lockdownd` enumerate as an iPhone and pair
with a Mac (the UniXNU QcomDwc3 model). The existing X9 USB log stays reachable
through vendor control requests 0x46/0x47 on EP0, which the controller answers
itself before handing other setup packets to the family (`usb_log.py` then has to
open the Apple VID/PID).

## Why not AppleUSBXDCI

QcomDwc3 derives from Apple's `AppleUSBXDCI` because SM8550 has a DWC3 core, the
IP Apple's class programs. MTU3 is MediaTek IP, so X9 implements the controller
interface itself, the way Apple's DWC2 driver `AppleSynopsysOTGDevice`
(KDK `AppleT7000USBOTGDevice.kext`, with symbols) derives directly from
`IOUSBDeviceController`.

## ABI (verified, 2026-11-12)

- `kext_vtable.py`: dumps vtables of KDK arm64e kexts (chained fixups, pointer
  format 7; llvm-objdump cannot read them).
- `kc_vtables.py`: lists vtables of one kext in the 20H392 legacy kernelcache
  (plain pointers, no chained fixups, no PAC).
- `gen_usbdc_abi.py` -> `usbdc-20H392-abi.json`: KDK 13.5 `IOUSBDeviceController`
  vtable without the macOS `_RESERVED` padding = 232 slots; IOService ends at 167
  (as the iOS ABI patch 0002 expects); the 20H392 vtable at kc 0xfffffff006e11200
  (prelinked 0xfffffe000c449200) has 232 slots and the same 18
  `__cxa_pure_virtual` positions (175, 177, 179-189, 192, 193, 198, 201, 225).
  Pure slots, which the controller must implement: softDisconnect, goOnBus,
  connectedWithAccessoryCable, handleStallEndpoint, handleAbortEndpoint,
  handleSetAddressPre/PostStatusPhase, getConnectionSpeed,
  provideEndpointIDsForConfiguration, prepareConfiguration, activateEndpoint,
  deactivateEndpoint, createIOBuffer, isEndpointStalled, resetEndpointDataToggle,
  forceReEnumeration, doRemoteWakeGated, ioGated.

## Linking from the kernel

The driver is built into the kernel; IOUSBDeviceFamily is a prelinked kext at a
fixed offset from it (build-usb-prelink.py, one uniform slide). As in UniXNU, the
base class's constructor, destructor, gMetaClass, vtable, inherited virtuals and
the few non-virtual APIs are bound to their prelinked addresses at link time
(absolute symbols; PC-relative references stay valid after sliding).

Non-virtual APIs the DWC2 driver imports (all a controller needs):
`IOUSBDeviceControlRequest::{inputSetupPacket, inputDataPacket, processSetupPacket, reset}`,
`IOUSBDeviceController::{completeEndpointIO, abortEndpoint, readyForHost, tryRemoteWake}`,
`IOUSBDeviceInterface::{getAlternateSettingCount, getPipeCountForAlternateSetting, getPipeForAlternateSetting}`,
`IOUSBDevicePipe::getDirection`, ctor/dtor.

## Next

1. Locate those non-virtual functions, the ctor/dtor and gMetaClass in the
   stripped 20H392 IOUSBDeviceFamily (match against the symbolized KDK code:
   string references, call graph, instruction shape without PAC).
2. From the KDK DWC2 disassembly: return types of the pure slots, the
   `tIOUSBDeviceInterfacePipeProperty` by-value size, the `_IOUSBDeviceIORequestTag`
   fields a controller reads/writes, base-class fields it touches, and the EP0
   setup/data/status flow (ep0OutIntOccurred -> inputSetupPacket -> processSetupPacket
   -> handleControlRequestStateChange; ioGated -> hardware -> completeEndpointIO).
3. Generate `X9UsbDeviceControllerABI.h` + bindings; compile and compare every
   mangled name against the KDK list.
4. `X9Mtu3Xdci`: MTU3/QMU from `x9/kernel/src/bsd/dev/x9_usb_device.c` (already
   enumerates on this board), EP0 vendor 0x46/0x47 for the log, publish after the
   kexts load (X9NativeUsbProbe).
