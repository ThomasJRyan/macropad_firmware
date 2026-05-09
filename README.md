# Alien Macropad Firmware

Firmware for a Raspberry Pi Pico W macropad that turns physical button presses
into configurable HTTP requests.

The firmware artifact is:

```text
alien_macropad_firmware.uf2
```

## Quick Start

### 1. Flash The Firmware

Download `alien_macropad_firmware.uf2` from a GitHub Release, or build it
locally from the development instructions below.

To flash manually, hold the Pico W `BOOTSEL` button while plugging it in, then
copy the UF2 to the mounted `RPI-RP2` drive.

### 2. Connect The Pico W To Wi-Fi

On first boot, or after a factory reset, the Pico W starts an open setup access
point:

```text
SSID: macropad_setup
Address: http://172.16.4.1/
```

Connect a computer or phone to `macropad_setup`, open `http://172.16.4.1/`,
scan for nearby Wi-Fi networks, select your network, enter the password, and
save.

After saving Wi-Fi credentials, reboot the Pico W. It should join your network
and advertise itself at:

```text
http://macropad.local/
```

If mDNS is not available on your machine, use your router to find the Pico W's
assigned IP address and open that address directly.

### 3. Configure The Buttons

Once connected over your normal network, open `http://macropad.local/` and
configure the six button actions.

The physical button layout is:

```text
GP5  GP4  GP3
GP6  GP7  GP8
```

Buttons are active-low and should be wired between the GPIO pin and ground. The
firmware enables the Pico W's internal pull-ups.

Each button can be disabled or configured as a `GET` or `POST` request to an
`http://` URL. A button can store up to three URLs and run them either as a
burst or one per press in round-robin order. POST actions can also include a
body, content type, and custom headers.

Configuration is saved to flash and persists across reboots.

## Development

### Prerequisites

- CMake
- Git
- Arm GNU embedded toolchain, including `arm-none-eabi-gcc`
- Ninja, optional but recommended

On Ubuntu/Debian:

```sh
sudo apt-get install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi ninja-build
```

### Set Up The Repository

The Raspberry Pi Pico SDK is tracked as a Git submodule.

```sh
git clone <repo-url>
cd <repo-directory>
git submodule update --init --recursive
```

### Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

The UF2 is generated at:

```text
build/alien_macropad_firmware.uf2
```

### Test

There are no firmware tests yet, but the project is wired for CTest:

```sh
ctest --test-dir build --output-on-failure
```

### Flash During Development

If local USB permissions are configured for picotool:

```sh
build/picotool-usb/picotool load -f -x -v build/alien_macropad_firmware.uf2
```

Manual BOOTSEL flashing is still a useful fallback when debugging flashing or
USB enumeration issues.

## Release

GitHub Actions publishes firmware releases from semantic version tags.

```sh
git tag v1.0.0
git push origin v1.0.0
```

The release workflow builds the firmware, runs `ctest`, and uploads
`alien_macropad_firmware.uf2` to the matching GitHub Release.
