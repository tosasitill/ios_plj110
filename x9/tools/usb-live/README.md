# USB live log reader

`usb_log.py` reads the X9 kernel/LK log over USB EP0 with libusb
(`/opt/homebrew/lib/libusb-1.0.dylib`). Read-only; it never resets the device.

    python3 x9/tools/usb-live/usb_log.py --output /tmp/N.txt

Restored 2026-11-03 from `x9/legacy/work/x9-usb-live/` (removed in 81069e5).
Byte-identical to `.local/x9/work/x9-usb-live/usb_log.py` and to the former
`/Users/tosasitill/HelioDarwin-x9-lk-preserve/work/x9-usb-live/usb_log.py`.
`test_usb_log.py` / `test_protocol.c` check the frame protocol (`log_protocol.h`).
