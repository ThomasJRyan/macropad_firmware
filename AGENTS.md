# AGENTS.md

## Project Overview

This repository contains firmware for a Raspberry Pi Pico W macropad.

The firmware must be written in C and built into a `.uf2` binary suitable for
uploading to the Pico W.

## Firmware Target

- Hardware: Raspberry Pi Pico W
- Language: C
- Final artifact: `.uf2`
- Upload flow: copy the generated `.uf2` file to the Pico W while it is mounted
  in bootloader mode

## Build System Requirements

Maintain a build system that can compile the C firmware and produce a `.uf2`
artifact for the Raspberry Pi Pico W.

Expected direction:

- Use the Raspberry Pi Pico SDK.
- Prefer CMake, which is the standard Pico SDK build flow.
- Configure the target board as `pico_w`.
- Ensure the build emits firmware formats needed for development, especially
  `.uf2`.
- Keep build outputs outside source directories, for example in `build/`.

Typical build shape:

```sh
cmake -S . -B build
cmake --build build
```

The resulting `.uf2` should be located under the build directory after a
successful compile.

## Pico W Development Loop

Build the firmware before flashing:

```sh
cmake --build build
```

The expected UF2 path is:

```sh
build/macropad_3.uf2
```

### Flashing With Picotool

Use the locally built USB-capable `picotool` binary:

```sh
build/picotool-usb/picotool load -f -x -v build/macropad_3.uf2
```

Important flags:

- `-f` forces the Pico W to reboot into BOOTSEL mode from a running firmware
  image.
- `-x` executes the newly loaded firmware after flashing. Do not omit this flag;
  without it, the command can appear to succeed while the new application does
  not start as expected.
- `-v` verifies the flash contents after writing.

If `picotool` requires `sudo`, check the host udev rules for Raspberry Pi Pico
devices and reload them before continuing. The working flow should not require
holding BOOTSEL for each iteration once udev and USB access are configured.

### Manual BOOTSEL Flashing Fallback

If `picotool` behavior is suspect, hold BOOTSEL while plugging in the Pico W and
copy the UF2 manually:

```sh
mkdir -p /tmp/pico && sudo mount /dev/disk/by-label/RPI-RP2 /tmp/pico && sudo cp build/macropad_3.uf2 /tmp/pico/ && sync
```

Use this fallback to confirm that a firmware image itself works before debugging
the automated flashing path.

### Serial Console

Use `tio` to read USB serial output from the Pico W:

```sh
tio -b 115200 /dev/serial/by-id/usb-Raspberry_Pi_Pico_E6614104037C782B-if00
```

Exit `tio` with `Ctrl-T`, then `q`.

If the by-id path changes or the command cannot connect, list serial devices:

```sh
ls -l /dev/serial/by-id/
```

When debugging missing serial output, first verify that the current UF2 was
actually flashed. A manual BOOTSEL copy is a useful control test.

## Development Notes

- Keep firmware code portable across normal Pico SDK toolchains.
- Do not commit generated build artifacts.
- Document any required SDK, toolchain, or environment variables when adding the
  build system.
- Prefer small, focused modules for device setup, input scanning, HID behavior,
  and wireless functionality.

## Commit Policy

All changes, fixes, and features require a Git commit.

Use semantic commit messages, such as:

- `feat: add key matrix scanner`
- `fix: correct Pico W board configuration`
- `docs: document firmware build flow`
