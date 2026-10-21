# Selenite Lite SDR++ source module

Native SDR++ source module for **Selenite Lite SDR RX** on Windows and Linux.

## Features

- Stereo USB Audio I/Q capture at **96 kHz**
- FT-817 CAT frequency tuning, **9600 baud, 8N1**
- Selectable audio capture device and **Refresh** control
- Configurable CAT serial port
- **Invert Q** option

## Platforms and test status

| Platform | SDR++ | Status |
| --- | --- | --- |
| Windows x64 | 1.3.0 | Module built; spectrum and waterfall displayed. CAT tuning not independently verified. |
| Linux Mint x86_64 | 1.3.1 | Module built and loaded; Source menu and settings displayed. Physical SDR reception and CAT not yet tested. |

**Note:** SDR++ plugins can depend on the exact version/ABI of `libsdrpp_core.so`. A successful build does not guarantee compatibility with a different installation.

## Source code

- `windows/` — Windows source module using WASAPI and Win32 serial CAT
- `linux/` — Linux source module using ALSA and POSIX serial CAT

Source files will be added separately. Prebuilt `.dll` and `.so` files will be attached to Releases after they are uploaded and verified.

## Linux build

Requires a compatible SDR++ source tree, CMake, C++ compiler, and ALSA development headers:

```bash
sudo apt install build-essential cmake pkg-config libasound2-dev
```

Copy `linux/CMakeLists.txt` and `linux/src/main.cpp` into `SDRPlusPlus/source_modules/selenite_source/` and add to the SDR++ root `CMakeLists.txt`:

```cmake
if(UNIX AND NOT APPLE)
    add_subdirectory("source_modules/selenite_source")
endif()
```

Configure SDR++ with the required dependencies, then:

```bash
cmake --build build --target selenite_source -j$(nproc)
```

Before installing the resulting plugin, check its `RUNPATH`, linked `libsdrpp_core.so`, and compatibility with your SDR++ installation.

## Device setup

Select the Selenite USB Audio capture device, set the CAT port (`COM10` on Windows or e.g. `/dev/ttyACM0` / `/dev/ttyUSB0` on Linux), then start reception. Actual hardware operation must be validated on each platform.

## License

A license has not yet been selected for this repository.
