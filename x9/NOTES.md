
## audio47 (log 305): mic stream died ~1.3 s after START with the speaker running

- 305: the speaker and mic ran together (shared AFE, `START -> 0 gen=3`). At 170.551 VUL1 reported `errno=22`, the worker stopped DMA and the mic went FAULT.
- Cause: `poll_locked` timestamps each chunk by working back from `now` by the bytes still pending. When a poll ran late and picked up a DMA burst, its first chunk came out at or before the previous poll's last sample. `x9_mic_deliver` requires strictly increasing time and returned EINVAL.
- Fix: `delivered_time` is reset at arm. Each chunk's time is clamped to `max(t, delivered_time+1)` and then to at most `now`. It is recorded only after a successful deliver.
- Still open: one stream fault keeps the mic FAULT until the next boot. EOVERFLOW (worker delayed 100 ms or more) and ENOBUFS (ring of 32768 frames is full) also still end the stream.
- Also seen: the boot self-test PCM was min=-5 max=4 rms=1. If recordings now play back silent, look at the analog gain/route next.
