# macropad_3

Firmware for a Raspberry Pi Pico W macropad.

## Current Behavior

- On boot, the Pico W attempts to connect to the saved Wi-Fi network when an
  SSID has been configured.
- A successful Wi-Fi connection uses DHCP, advertises `macropad.local` with
  mDNS, and serves the HTTP UI on the assigned network address.
- Without a saved SSID, the Pico W starts an open Wi-Fi access point named
  `macropad_setup`.
- The AP uses `172.16.4.1/24`.
- The DHCP server offers `172.16.4.2` to the first client.
- The HTTP server listens on `http://172.16.4.1/` in setup AP mode and on the
  DHCP-assigned address in station mode.
- The page configures one HTTP action for GP5 and one HTTP action for GP6.
- Each button action can be disabled or configured as an HTTP `GET` or `POST`
  request to an `http://` URL.
- `POST` actions can include a JSON request body.
- The page can scan nearby Wi-Fi access points and save an SSID/password to
  flash.
- Configuration is saved to flash and loaded across boots.
- `GET /api/config` returns the saved configuration.
- `POST /api/config` saves the two button action records.
- `GET /api/wifi/scan` returns the latest scan status and nearby network list.
- `POST /api/wifi/scan` starts a Wi-Fi scan.
- `POST /api/wifi` saves `ssid` and `password` form values.
- GP5 and GP6 are active-low buttons with internal pull-ups.
- Holding a button does not retrigger the request. Release and press the button
  again to trigger another request.

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
