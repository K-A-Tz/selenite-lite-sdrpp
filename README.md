# Selenite Lite SDR RX — SDR++ Plugin (Windows & Linux)

**Selenite Lite SDR++** is a source plugin for the **Selenite Lite SDR RX** receiver, compatible with tested builds of **SDR++ 1.3.1** on Windows x64 and Linux Mint x86_64.

**Русский:** Плагин для подключения приёмника **Селенит Лайт (Selenite Lite SDR RX)** к **SDR++** в Windows и Linux Mint. Приём I/Q через USB Audio 96 кГц и управление частотой по CAT FT-817.

**[Download v0.3.0 / Скачать готовые модули](https://github.com/K-A-Tz/selenite-lite-sdrpp/releases/tag/v0.3.0)**

## Features / Возможности

- Stereo USB Audio I/Q capture at **96 kHz** (Windows WASAPI / Linux ALSA)
- FT-817-compatible CAT frequency tuning: **9600 baud, 8N2**
- **200 ms delay between consecutive CAT bytes**, with an additional guard interval between commands
- Background CAT worker that retains only the most recent pending tuning request
- Tuning range: **1.5–32 MHz**
- Selectable capture device, Refresh control, configurable serial port, Invert Q option

## Downloads and compatibility / Загрузка и совместимость

| Platform | File | Tested status |
| --- | --- | --- |
| Windows x64, SDR++ 1.3.1 | `selenite_source.dll` | CAT and reception reported working on real Selenite Lite hardware |
| Linux Mint x86_64, SDR++ 1.3.1 | `selenite_source.so` | Built successfully and loaded in SDR++; hardware CAT and I/Q not yet confirmed |

Get binaries from **[GitHub Releases](https://github.com/K-A-Tz/selenite-lite-sdrpp/releases/tag/v0.3.0)**.

SDR++ plugins may require the same or a compatible SDR++ core ABI as the build they were compiled against. A matching version number alone does not guarantee binary compatibility.

## Installation / Установка

**Windows:** Close SDR++ and copy `selenite_source.dll` into the `modules` directory of your SDR++ installation (for example, `C:\\sdrpp_win_x64\\modules\\`). Restart SDR++.

**Linux Mint:** Close SDR++ and copy `selenite_source.so` into the system plugin directory (on the tested installation, `/usr/lib/sdrpp/plugins/`):

```bash
sudo cp selenite_source.so /usr/lib/sdrpp/plugins/selenite_source.so
```

Restart SDR++ and select **Selenite Lite** as the source. Select the correct USB Audio input and CAT serial port (`COM10` is an example on Windows; `/dev/ttyACM0` or `/dev/ttyUSB0` on Linux). On Linux, the user must have permission to access the serial device.

**Русский:** Закройте SDR++, скопируйте модуль в каталог плагинов, запустите SDR++ и выберите источник **Selenite Lite**. Укажите звуковое устройство Селенита и правильный CAT-порт. В Linux может потребоваться доступ к последовательному порту.

## Building from source / Сборка

- `windows/` — Windows module using WASAPI and Win32 serial CAT
- `linux/` — Linux module using ALSA and POSIX serial CAT

For Linux, install prerequisites:

```bash
sudo apt install build-essential cmake pkg-config libasound2-dev
```

Copy `linux/CMakeLists.txt` and `linux/src/main.cpp` into `SDRPlusPlus/source_modules/selenite_source/` in a compatible SDR++ source tree. Ensure the SDR++ root CMake configuration includes:

```cmake
if(UNIX AND NOT APPLE)
    add_subdirectory("source_modules/selenite_source")
endif()
```

Then build:

```bash
cmake --build build --target selenite_source -j$(nproc)
```

## Search terms / Поисковые запросы

Selenite Lite SDR++, Selenite Lite SDR RX, Селенит Лайт SDR++, Селенит SDR Linux, Селенит SDR Windows, FT-817 CAT, SDR++ source plugin.

## License

A license has not yet been selected for this repository.
