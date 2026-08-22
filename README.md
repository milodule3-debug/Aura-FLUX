# Powerboard

**C++20 terminal GPU power monitor with full system telemetry — btop++ style.**

Real-time hardware monitor for Linux. Samples GPU power consumption, CPU load & temp, RAM usage, disk usage, and system uptime every 100 ms, renders everything at smooth 60 FPS in your terminal.

Built by Dušan Milosavljević (Lean Progress IQ). The terminal counterpart to Aura Pulse — same telemetry, different rendering pipeline.

![License](https://img.shields.io/badge/license-MIT-blue)
![C++](https://img.shields.io/badge/C%2B%2B-20-purple)
![FTXUI](https://img.shields.io/badge/UI-FTXUI-orange)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey)

## Why

Aura Pulse gives you a windowed dashboard. Powerboard gives you the same data in your terminal — perfect for SSH sessions, servers, and headless boxes where no GUI exists.

## Features

- **GPU power tracking** — reads NVIDIA/AMD power sensors via sysfs, integrates cumulative kWh and cost
- **CPU monitoring** — load %, temperature (coretemp/k10temp/zenpower)
- **GPU monitoring** — load %, temperature, power draw
- **RAM tracking** — used / total GB with live sparkline
- **Disk usage** — used / total GB for root (`/`) with live sparkline
- **Uptime counter** — days, hours, minutes, seconds
- **Braille power graph** — overlapping traces for W, kWh, and cost over the last 5 minutes
- **CSV logging** — auto-rotating monthly files for historical analysis
- **Vim-like command bar** — `:q`, `:help`, `:graph`
- **60 FPS FTXUI rendering** — smooth, no flicker, low CPU
- **SQLite telemetry store** — for trend analysis

## Quick Start

### Requirements

- Linux
- CMake ≥ 3.22
- C++20 compiler (GCC 10+ or Clang 12+)
- [vcpkg](https://vcpkg.io) for dependencies
- ftxui (installed via vcpkg)
- SQLite3

### Build

```bash
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg.cmake
cmake --build build -j$(nproc)

# Run
./build/powerboard
```

## Screenshots

### Diagnostics Dashboard
![Diagnostics](assets/diagnostics.png)
*Real-time telemetry with CPU frequency, temperature, load metrics, and system information*

### Testing & Benchmarks
![Testing](assets/testing.png)
*CPU benchmark results and stress testing interface with performance metrics*

### Optimization Settings
![Optimization](assets/optimization.png)
*System optimization modes with active performance tuning options and CPU turbo controls*

### Vault & AI Integration
![Vault & AI](assets/vault-ai.png)
*Clipboard vault manager with AI copilot integration (Ollama-powered)*

### Alternative Theme - Light Mode
![Light Theme](assets/light-theme.png)
*Powerboard with light color scheme showing real-time monitoring data*

### Clipboard Vault Manager
![Clipboard Vault](assets/clipboard-vault.png)
*Smart clipboard manager with AI recipe suggestions and multi-provider support*

## Architecture

- `src/main.cpp` — entry point, render loop
- `src/ai.h` — AI inference hooks
- `src/benchmarks.h` — hardware benchmark routines
- `src/clipboard.h` — clipboard integration
- `src/datalogger.h` — CSV logger + SQLite writer
- `src/scanner.h` — sysfs / hwmon scanner
- `src/types.h` — common data types
- `src/utils.h` — helpers
- `policy.yaml` — runtime configuration
- `C++rules.txt` — coding style guide

## Tech Stack

- C++20
- FTXUI (terminal UI framework)
- SQLite 3 (telemetry persistence)
- vcpkg (dependency management)
- CMake (build system)

## License

MIT — see [LICENSE](LICENSE).

## Author

Built by **Dušan Milosavljević** — see [OWNERSHIP.md](OWNERSHIP.md).
