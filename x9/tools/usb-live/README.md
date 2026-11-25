# USB live log reader

`usb_log.py` reads the X9 kernel/LK log over USB EP0 with libusb
(`/opt/homebrew/lib/libusb-1.0.dylib`). Read-only; it never resets the device.

    python3 x9/tools/usb-live/usb_log.py --output /tmp/N.txt

Restored on 2026-11-03 from the repository's former
`x9/legacy/work/x9-usb-live/` tree (removed in 81069e5). Historical byte-for-byte
comparisons against private/outside workspaces are not build requirements; current
inputs must stay in the repository's fixed `.local/` layout.
`test_usb_log.py` / `test_protocol.c` check the frame protocol (`log_protocol.h`).
