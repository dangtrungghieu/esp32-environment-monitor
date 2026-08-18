# esp32-environment-monitor

A lightweight, opinionated development environment and collection of helpers for ESP32 projects (C++). This repository contains documentation, example workflows, and recommended commands to get started building, flashing, and debugging ESP32 firmware using common toolchains: Espressif ESP‑IDF, Arduino-ESP32, PlatformIO, and Docker-based workflows.

Note: This README is intentionally generic so it can be adapted to your preferred workflow. Adjust commands and versions to match the toolchain you use.

## Table of contents
- [Features](#features)
- [Repository layout](#repository-layout)
- [Prerequisites](#prerequisites)
- [Getting started](#getting-started)
  - [1) Espressif ESP-IDF (recommended for native C++/C development)](#1-espressif-esp-idf-recommended-for-native-cc-development)
  - [2) Arduino-ESP32 (Arduino framework)](#2-arduino-esp32-arduino-framework)
  - [3) PlatformIO (VS Code integration)](#3-platformio-vs-code-integration)
  - [4) Docker (containerized builds)](#4-docker-containerized-builds)
- [Building, flashing and monitoring](#building-flashing-and-monitoring)
- [Examples](#examples)
- [Development tips & troubleshooting](#development-tips--troubleshooting)
- [Contributing](#contributing)
- [License](#license)
- [Acknowledgements](#acknowledgements)

## Features
- Guidance for multiple ESP32 toolchains (ESP-IDF, Arduino, PlatformIO).
- Common build and flash commands.
- Docker usage for reproducible environments.
- Helpful tips for debugging and serial monitoring.

## Repository layout
(Adjust to match actual files in this repo)
- `examples/` — small example projects or sketches
- `scripts/` — helper scripts for building/flashing
- `docs/` — extended documentation, guides, and notes
- `docker/` — Dockerfiles or scripts for containerized builds
- `README.md` — this file

## Prerequisites
- A development machine running Linux, macOS, or Windows (WSL recommended for Windows).
- USB cable and an ESP32 board (ESP32-WROOM, WROVER, etc.).
- Basic tools:
  - git
  - Python 3.8+ (for ESP-IDF tooling)
  - pip
  - GNU Make / CMake (installed by toolchains where needed)
  - A serial terminal program (minicom, screen, picocom, or the `idf.py monitor` / PlatformIO monitor)

## Getting started

### 1) Espressif ESP‑IDF (recommended for native C++/C development)
1. Clone repository:
   ```bash
   git clone https://github.com/dangtrungghieu/esp32-env.git
   cd esp32-env
   ```
2. Install or download ESP‑IDF (follow Espressif docs for the specific release you want):
   ```bash
   git clone --branch release/vX.Y https://github.com/espressif/esp-idf.git
   cd esp-idf
   ./install.sh       # Linux/macOS — installs Python deps and toolchain
   . ./export.sh      # Set up environment variables
   ```
3. Create or open a project that uses CMake and `idf.py`. Example build commands:
   ```bash
   idf.py set-target esp32
   idf.py build
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

### 2) Arduino-ESP32 (Arduino framework)
1. Use the Arduino IDE or PlatformIO with the `espressif32` platform.
2. For Arduino CLI:
   ```bash
   arduino-cli core update-index
   arduino-cli core install esp32:esp32
   arduino-cli compile --fqbn esp32:esp32:esp32 your_sketch_folder
   arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 your_sketch_folder
   ```

### 3) PlatformIO (VS Code integration)
1. Install VS Code and the PlatformIO extension.
2. Add a `platformio.ini` to your project or open an example project; common commands:
   ```bash
   pio run                # build
   pio run -t upload     # upload to device
   pio device monitor    # serial monitor
   ```

### 4) Docker (containerized builds)
Use Docker to ensure a consistent environment across machines:
```bash
docker run --rm -it -v "$(pwd)":/project -w /project espressif/idf:latest /bin/bash
# inside container:
. $IDF_PATH/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```
Adjust the image tag to the ESP‑IDF release you need.

## Building, flashing and monitoring
Common ESP‑IDF workflow:
```bash
# Set up environment once per shell session
. $IDF_PATH/export.sh

# Build
idf.py build

# Flash and automatically start the serial monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

PlatformIO workflow:
```bash
pio run
pio run -t upload -e <env>
pio device monitor
```

Arduino CLI workflow:
```bash
arduino-cli compile --fqbn esp32:esp32:esp32 path/to/sketch
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 path/to/sketch
```

## Examples
- Check `examples/` for starter projects (blink, wifi_scan, ota_update). Each example should include a README with build/upload instructions.

## Development tips & troubleshooting
- If the board is not recognized on Linux, check udev rules; add a udev rule for your USB‑to‑serial adapter.
- On macOS, `/dev/tty.SLAB_USBtoUART` or `/dev/tty.usbserial-*` are common device names.
- Use `idf.py monitor` or `pio device monitor` for logs. Ctrl+] typically exits the `idf.py monitor`.
- If builds fail with missing Python packages, install them with `pip install -r $IDF_PATH/requirements.txt`.
- For reproducible builds, pin ESP‑IDF and toolchain versions and consider using Docker.

## Contributing
Contributions are welcome. Suggested workflow:
1. Fork the repository.
2. Create a feature branch: `git checkout -b feat/my-change`.
3. Make changes and update documentation.
4. Open a pull request describing your changes.

Please include:
- Clear description of what changed and why.
- Build/test instructions if relevant.
- Small, focused commits.

## License
This repository does not contain a license file by default. Consider adding a license (MIT, Apache-2.0, etc.) if you want to permit reuse. Example:
```
MIT License
```

## Acknowledgements
- Espressif Systems — ESP32, ESP-IDF
- PlatformIO
- Arduino-ESP32 community

If you'd like this README adapted to the exact layout and scripts in this repository (for example, to include actual example names, `platformio.ini`, or Dockerfiles), tell me and I will update it or commit it directly to the repo.
