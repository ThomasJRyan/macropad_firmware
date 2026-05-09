# AGENTS.md

## Project Overview

This repository contains Alien Macropad Firmware for a Raspberry Pi Pico W
macropad.

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
build/alien_macropad_firmware.uf2
```

### Flashing With Picotool

Use the locally built USB-capable `picotool` binary:

```sh
build/picotool-usb/picotool load -f -x -v build/alien_macropad_firmware.uf2
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
mkdir -p /tmp/pico && sudo mount /dev/disk/by-label/RPI-RP2 /tmp/pico && sudo cp build/alien_macropad_firmware.uf2 /tmp/pico/ && sync
```

Use this fallback to confirm that a firmware image itself works before debugging
the automated flashing path.

### Serial Console

The firmware enables both USB stdio and UART0 stdio. UART0 is the preferred
debug path when using a second Pico as a Picoprobe/debugprobe because it keeps
logs available even when the target Pico W USB connection is busy or unstable.

Target Pico W UART wiring:

```text
Pico W GP0 / UART0 TX -> probe UART RX
Pico W GP1 / UART0 RX <- probe UART TX
Pico W GND            -> probe GND
```

Read the probe UART serial device at 115200 baud. The exact by-id path can vary,
so list devices first:

```sh
ls -l /dev/serial/by-id/
```

Then connect with `tio`, for example:

```sh
tio -b 115200 /dev/serial/by-id/<debug-probe-uart-device>
```

USB serial on the target Pico W is also enabled and can be read with:

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

## Release Workflow

The GitHub Actions workflow at `.github/workflows/release.yml` runs when a
semantic version tag such as `v1.0.0` is pushed.

The workflow:

- Checks out recursive submodules, including the Pico SDK.
- Installs the Arm embedded toolchain.
- Builds the `alien_macropad_firmware` CMake target.
- Runs `ctest` from the build directory.
- Uploads `build/alien_macropad_firmware.uf2` to the matching GitHub Release.

## Commit Policy

All changes, fixes, and features require a Git commit.

Use semantic commit messages, such as:

- `feat: add key matrix scanner`
- `fix: correct Pico W board configuration`
- `docs: document firmware build flow`
