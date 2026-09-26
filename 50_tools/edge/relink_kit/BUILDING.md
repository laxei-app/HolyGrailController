# TwyLapse Edge firmware — relink archive

This archive lets you replace the **Arduino-ESP32** library inside a published
TwyLapse Edge firmware image with your own, possibly modified, version — and
then build a firmware image you can flash to the device.

It is published because Arduino-ESP32 is licensed under the **GNU Lesser
General Public License, version 2.1 or later**, and section 6 of that license
requires that you be able to modify the library and relink the program that
uses it. The ESP32 has no shared-library mechanism, so option 6(b) does not
apply; this archive is the option 6(a) route: the rest of the firmware is
supplied **as object code**, together with everything needed to link it.

You are permitted to reverse engineer and debug modifications you make to
Arduino-ESP32 in order to exercise those rights.

Everything else in this firmware is proprietary. No source code for TwyLapse
itself is included, and none is needed to relink.


## What is in here

| Path | Contents |
|---|---|
| `objects/` | The compiled object code of the firmware: TwyLapse code, the bundled libraries, and the Arduino-ESP32 archives you are going to replace. Debug information has been stripped; the symbol tables needed for linking are intact. |
| `link/link_command.txt` | The exact linker command line that produced the official image, with machine-specific paths replaced by `@ARDUINO@`, `@IDF_LIBS@` and `@TOOLCHAIN@`. |
| `link/bootloader.bin`, `link/partitions.bin`, `link/boot_app0.bin` | The other flash regions, needed to build a complete flashable image. |
| `arduino/compile_commands.json` | The exact compiler command line used for every Arduino-ESP32 translation unit in the official build. |
| `arduino/archive_members.json` | Which object belongs to which archive in `objects/`, so a rebuilt object can be put back in the right place. |
| `reference/reference.json` | Model, version, build type and the SHA-256 of the official `firmware.elf` and `firmware.bin`. |
| `relink.py` | Does the whole job: recompile, replace, link, package. |
| `MANIFEST.txt` | SHA-256 and size of every file in this archive. |


## What you need

* **Python 3.8 or later.**
* **The Xtensa toolchain** used for the official build:
  `toolchain-xtensa-esp-elf` from PlatformIO. The exact version is recorded in
  the compiler commands.
* **The precompiled ESP-IDF libraries**:
  `framework-arduinoespressif32-libs` from PlatformIO. These are Espressif's
  Apache-2.0 libraries and linker scripts; they are not modified by us.
* **The Arduino-ESP32 source tree** — the part you may change.
* **esptool** (`pip install esptool`) to turn the linked ELF into a flashable
  image. `relink.py` prints the two commands if esptool is missing.

The easiest way to obtain the toolchain and the ESP-IDF libraries is to install
PlatformIO and build any ESP32-S3 project once with the same platform that we
use:

```
pip install platformio
pio pkg install --global --platform \
  "https://github.com/pioarduino/platform-espressif32/releases/download/53.03.13/platform-espressif32.zip"
```

`relink.py` then finds both packages under `~/.platformio/packages`
automatically.

The official build used **Arduino-ESP32 3.1.3**, which is the version that
ships inside that platform release. Its source is also available from
<https://github.com/espressif/arduino-esp32>. Start from that version: an
unrelated version will not link, because the rest of the firmware was compiled
against its headers.


## Relinking

```
python relink.py --arduino /path/to/arduino-esp32
```

`--arduino` must point at the directory that contains `cores/` and
`libraries/`. Options:

```
--idf-libs  <dir>   framework-arduinoespressif32-libs   (default: from PlatformIO)
--toolchain <dir>   toolchain-xtensa-esp-elf            (default: from PlatformIO)
--jobs      N       parallel compiles                   (default: CPU count)
--skip-compile      reuse objects from a previous run
```

The script

1. recompiles every Arduino-ESP32 file from **your** tree, with the recorded
   flags,
2. replaces those objects inside the archives in `objects/` using `ar r`,
3. runs the recorded link command, producing `out/firmware.elf`,
4. produces `out/firmware.bin` and `out/firmware-merged.bin`.

To check your setup before changing anything, run it once against an unmodified
Arduino-ESP32 3.1.3. The result will not be byte-for-byte identical to
`reference/reference.json` — absolute paths and build timestamps differ — but it
should link without errors and be within a few hundred bytes of the recorded
size.


## Flashing

```
esptool.py --chip esp32s3 --port <PORT> --baud 921600 \
           write_flash 0x0 out/firmware-merged.bin
```

Writing from address `0x0` erases the stored settings — device name, Wi-Fi
credentials and the registered owner. The device recreates its own access point
on the next boot and has to be set up again from the app.

To keep the stored settings, write only the application partition:

```
esptool.py --chip esp32s3 --port <PORT> --baud 921600 \
           write_flash 0x10000 out/firmware.bin
```

The device may need to be put into download mode by hand: hold the reset button
while connecting, or use `esptool.py --before default_reset`.

An image built here is not stamped with the identification block that the
TwyLapse app writes. It runs normally, but the app's own USB flashing screen
will not recognise it and will fall back to a full-chip write. Use esptool for
images you build yourself.


## Licensing summary

* **Arduino-ESP32** — GNU Lesser General Public License, version 2.1 or later.
  Copyright (c) Arduino SA, Espressif Systems and contributors. The full text
  of the license is shown in the app under *Menu → 著作権表示 → Arduino-ESP32*
  and is available at <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>.
* **ESP-IDF and the precompiled libraries** — Apache License 2.0,
  copyright (C) Espressif Systems (Shanghai) CO LTD.
* **Everything under `objects/` that is not Arduino-ESP32** — either proprietary
  TwyLapse code or third-party components under permissive licenses. Every one
  of them is listed, with its full license text, in the TwyLapse app under
  *Menu → 著作権表示*. Redistributing this archive as a whole is permitted only
  for the purpose of exercising the LGPL rights described above.
* **TwyLapse** — Copyright (c) 2026 laxei.app. All rights reserved.

If anything in this archive is incomplete or does not work for relinking,
that is a defect on our side: please report it.
