# Aura Standing Rules

## Surfaces
This repo is a C++20 terminal application. There is no website and
no HTML. The app is `src/` plus `CMakeLists.txt`. The repo page is
`README.md`, with images in `assets/`.

If a request says "the website" or "the page", stop and ask what is
meant — do not create one. Putting an image "in the repo" means
commit it to `assets/` and reference it from `README.md`; committing
the file alone renders nothing. Committing an asset and displaying
it are two separate steps; do only the one asked.

## This repo is not Aura Pulse
`DusanCar-sudo/aura-pulse` is the windowed desktop app with the same
telemetry. Powerboard is the terminal counterpart with a different
rendering pipeline. Requests about the GUI belong in that repo. Do
not port GUI code here.

## Build
CMake with vcpkg — `vcpkg.json` and `vcpkg-configuration.json`
declare dependencies. UI is FTXUI. See `BUILD.md` before changing
build steps. `C++rules.txt` and `powerboard-instructions.md` hold
project-specific conventions; read them before restructuring code.

## Layout
Headers carry the implementation: `src/main.cpp` is the entry point,
with `scanner.h` for sysfs sampling, `benchmarks.h`, `clipboard.h`,
`datalogger.h`, `ai.h`, `tokens.h`, `types.h` and `utils.h`.

## Hardware access is privileged
The scanner reads NVIDIA and AMD power sensors through sysfs and the
logger writes cumulative kWh and cost. Sampling runs every 100 ms at
60 FPS. Treat changes to sampling, sensor paths or the accumulation
maths as high-risk and verify against real hardware before claiming
they work. Do not fabricate sensor values to make a build pass.

## Branch and secrets
The default branch is `master`, not `main`. Never inline a token
into a shell command; use `gh` or an env file.
