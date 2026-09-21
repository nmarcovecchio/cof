# CallOnFail current version

The ESP32 reads `manifest.json` from this directory over Ethernet.

Expected production files:

- `manifest.json`: firmware, audio and runtime configuration metadata.
- `firmware.bin`: PlatformIO build output copied from `.pio/build/wt32-eth01/firmware.bin`.
- `audio/cof_test.wav`: 8 kHz, 16-bit, mono WAV uploaded to the A7672 modem.

Keep the firmware version in `manifest.json` higher than the version compiled in
`include/cof_config.h` when you want deployed devices to update OTA.
