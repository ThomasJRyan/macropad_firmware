# macropad_3

Firmware for a Raspberry Pi Pico W macropad.

## Current Behavior

- GP6: active-low button with internal pull-up; press once to blink the onboard
  LED 5 times at 0.25 second intervals.
- GP7: active-low button with internal pull-up; press once to blink the onboard
  LED 5 times at 1 second intervals.
- Holding a button does not retrigger the blink sequence. Release and press the
  button again to trigger another sequence.

## Prerequisites

- CMake
- Git
- Arm GNU embedded toolchain, including `arm-none-eabi-gcc`

## SDK Setup

The Raspberry Pi Pico SDK is tracked as a Git submodule at
`external/pico-sdk`.

After cloning this repository, initialize dependencies with:

```sh
git submodule update --init --recursive
```

## Build

Configure and build from the repository root:

```sh
cmake -S . -B build
cmake --build build
```

The Pico W UF2 firmware is generated at:

```text
build/macropad_3.uf2
```

## Upload

Hold the Pico W `BOOTSEL` button while connecting it over USB, then copy
`build/macropad_3.uf2` to the mounted bootloader drive.
