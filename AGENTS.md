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
