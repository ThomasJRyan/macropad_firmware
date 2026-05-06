# macropad_3

Firmware for a Raspberry Pi Pico W macropad.

## Current Behavior

- On boot, the Pico W starts an open Wi-Fi access point named
  `macropad_setup`.
- The AP uses `172.16.4.1/24`.
- The DHCP server offers `172.16.4.2` to the first client.
- The HTTP server listens on `http://172.16.4.1/` and serves a page that says
  `Hello World`.
- The page has numeric configuration inputs for blink count and blink frequency,
  defaulting to `0` and `0.0`.
- Configuration is saved to flash and loaded across boots.
- On boot, the onboard LED runs the saved blink sequence once when both saved
  values are nonzero.
- `GET /api/config` returns the saved configuration.
- `POST /api/config` saves `blinks` and `frequency` form values.
- `POST /api/blink` queues the web-triggered LED blink sequence using the saved
  configuration.
- GP5: active-low button with internal pull-up; press once to blink the onboard
  LED 5 times at 0.25 second intervals.
- GP6: active-low button with internal pull-up; press once to blink the onboard
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
