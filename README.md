# Protocol8

[![CI](https://github.com/RiantZ/p8/actions/workflows/ci.yml/badge.svg)](https://github.com/RiantZ/p8/actions/workflows/ci.yml)

## Building

### Prerequisites

- CMake 4.0+
- Python 3
- C++20 compatible compiler (MSVC, GCC, Clang)

### Build

All builds go through the helper script `scripts/cmake_build.py`:

```bash
# Configure only
python scripts/cmake_build.py

# Configure + build
python scripts/cmake_build.py --build

# Configure + build using presets
python scripts/cmake_build.py --preset macos --build

# Build With Tracy
python scripts/cmake_build.py --preset macos-tracy --build

# Clean build (wipe build directory, then configure + build)
python scripts/cmake_build.py --clean --build
```

The script uses `CMakePresets.json` to select the right preset for each platform automatically.

Build output is placed in a platform-specific directory at the project root:

| Platform | Directory     | Preset    |
|----------|---------------|-----------|
| Windows  | `_Build_wnd/` | `windows` |
| macOS    | `_Build_mac/` | `macos`   |
| Linux    | `_Build_lnx/` | `linux`   |

You can also invoke CMake directly with a preset: `cmake --preset macos`.

### Formatting
The pre-commit hooks required code to be formatted using Clang format, to format code please use command:
```bash
python scripts/code_format.py --format --staged
```

### Testing

```bash
# Run regression tests (macOS example; substitute _Build_wnd or _Build_lnx on other platforms)
cd _Build_mac && ctest && cd ..

# Run performance tests (disabled by default, must be requested explicitly)
cd _Build_mac && ./tests/regression/P8_RegressionTests --gtest_filter="*c_log_perf*" --gtest_also_run_disabled_tests && cd ..
```

Logs: _Build_mac/Testing/Temporary/LastTest.log

### Performance baselines

Performance tests are disabled in the normal test suite and must be run explicitly.
The build must use `Release` configuration (`CMAKE_BUILD_TYPE=Release`, the default for macOS/Linux presets).

```bash
cd _Build_mac && ./tests/regression/P8_RegressionTests \
    --gtest_filter="*c_log_perf*" \
    --gtest_also_run_disabled_tests
```

Reference numbers (Apple M4 Max, macOS, Release).

Single-thread emit latency (`c_log_perf_test`, 32 batches x 1 000 000 iterations, median ns/call):

| Test | Per call | Iterations |
|------|----------|------------|
| `send_hello_d_no_attrs` | 26.8 ns | 1 000 000 |
| `send_hello_d_3_attrs` (str + i64 + f64) | 39.3 ns | 1 000 000 |

`full_cycle` throughput (init + emit + release) through the in-memory null sink,
1 000 000 iterations per thread. Numbers are aggregated over 25 runs of the test
(`ns/call` = full-cycle wall time / total calls):

| Threads | Total calls | Median ns/call | p95 ns/call | Stdev | Throughput (median) |
|---------|-------------|----------------|-------------|-------|---------------------|
| 1 | 1 000 000 | 26.6 ns | 32.8 ns | 3.16 ns | 37.6 M calls/s |
| 2 | 2 000 000 | 13.7 ns | 16.1 ns | 1.85 ns | 72.8 M calls/s |
| 4 | 4 000 000 | 7.9 ns  | 11.0 ns | 1.29 ns | 125.9 M calls/s |
| 8 | 8 000 000 | 4.5 ns  | 6.5 ns  | 0.94 ns | 221.1 M calls/s |

Relative spread (stdev/mean) grows with thread count — from ~12 % at 1 thread to
~19 % at 8 threads — so multi-threaded runs are less stable and p95 sits noticeably
above the median.

### Profiling with Tracy

[Tracy](https://github.com/wolfpld/tracy) is an optional, sampling + instrumentation
profiler. It is **OFF by default** (`P8_ENABLE_TRACY=OFF`): when disabled Tracy is not
fetched, compiled or linked, and the `P8_PROF_*` macros in `engine/p8_profiler.hpp`
expand to no-ops — zero build-time and run-time cost. p8 pins **Tracy v0.13.1**.

Profiling has two halves:

1. **The client** — your instrumented p8 binary. Built by enabling `P8_ENABLE_TRACY`.
2. **The viewer (GUI)** — the standalone *Tracy Profiler* app that displays the timeline.

The client and viewer **must be the same Tracy version** (`v0.13.1`); the wire protocol
is not compatible across versions.

#### 1. Build the client with Tracy enabled

Use a `*-tracy` preset. Release presets exist for every platform; Debug presets for
macOS and Linux:

| Platform | Release preset  | Debug preset          |
|----------|-----------------|-----------------------|
| Windows  | `windows-tracy` | *(use Release)*       |
| macOS    | `macos-tracy`   | `macos-debug-tracy`   |
| Linux    | `linux-tracy`   | `linux-debug-tracy`   |

```bash
# Windows
python scripts/cmake_build.py --preset windows-tracy --build

# macOS
python scripts/cmake_build.py --preset macos-tracy --build

# Linux
python scripts/cmake_build.py --preset linux-tracy --build
```

Or without the helper script: `cmake --preset macos-tracy && cmake --build _Build_mac`.
To add Tracy to any other preset, pass `-DP8_ENABLE_TRACY=ON`.

At configure time the log confirms the state:

```
p8: Tracy profiler ENABLED (v0.13.1)     # instrumentation active
p8: Tracy profiler disabled (default)    # macros are no-ops
```

#### 2. Build the Tracy viewer (GUI)

The viewer is a separate application. Building it from the sources p8 already fetched
guarantees the version matches the client. The sources live under the build directory:

- Windows: `_Build_wnd/_deps/tracy-src/`
- macOS:   `_Build_mac/_deps/tracy-src/`
- Linux:   `_Build_lnx/_deps/tracy-src/`

Build the viewer with CMake (substitute the `tracy-src` path for your platform):

```bash
# macOS example (swap _Build_mac for _Build_wnd / _Build_lnx)
cmake -S _Build_mac/_deps/tracy-src/profiler -B _Build_tracy_gui -DCMAKE_BUILD_TYPE=Release
cmake --build _Build_tracy_gui --config Release -j
```

The resulting `tracy-profiler` (`tracy-profiler.exe` on Windows) binary is in
`_Build_tracy_gui`.

Platform notes for the viewer build:

- **Windows** — build with the Visual Studio generator; the profiler pulls its
  dependencies via vcpkg automatically. Alternatively grab a prebuilt release from the
  [Tracy releases page](https://github.com/wolfpld/tracy/releases/tag/v0.13.1).
- **macOS** — requires `glfw` and `freetype` (`brew install glfw freetype`). A prebuilt
  `tracy` cask also exists via Homebrew, but only use it if its version is `v0.13.1`.
- **Linux** — install the dev packages the profiler needs, e.g. on Debian/Ubuntu:
  `sudo apt install libglfw3-dev libfreetype-dev libcapstone-dev libtbb-dev libdbus-1-dev`.

#### 3. Capture a profile

1. Launch the viewer and press **Connect** (it listens on `localhost:8086` by default).
2. Run your Tracy-enabled p8 binary, for example:

```bash
./_Build_mac/examples/logs_file_sink/p8_example_multithread_file_sink /tmp/p8out
```

The client connects to the viewer and streams the timeline live. p8 configures Tracy
with `TRACY_ON_DEMAND=OFF`, so the client buffers data from process start — nothing is
lost for short-lived tools even if you connect the viewer slightly late.

#### 4. Instrument your own code

Include the shim, make sure the target links `p8::profiler` (the `p8` library and the
examples already do), and drop in the macros:

```cpp
#include "p8_profiler.hpp"

void p8_flush_buffer()
{
    P8_PROF_ZONE();                 // zone name = function name
    // ... hot code ...
}
```

Available macros (all no-ops unless built with `P8_ENABLE_TRACY`):

| Macro | Purpose |
|-------|---------|
| `P8_PROF_ZONE()` | Scoped CPU zone, named after the function |
| `P8_PROF_ZONE_NAMED("x")` | Scoped zone with an explicit name |
| `P8_PROF_FRAME_MARK()` | Frame boundary marker |
| `P8_PROF_FRAME_MARK_NAMED("x")` | Named continuous frame set |
| `P8_PROF_THREAD_NAME("x")` | Label the current OS thread |
| `P8_PROF_PLOT("x", value)` | Numeric plot over time |
| `P8_PROF_MESSAGE(ptr, size)` | Free-form timeline message |
| `P8_PROF_ALLOC(ptr, size)` / `P8_PROF_FREE(ptr)` | Allocation tracking |

A zone lives until the end of its lexical scope, so wrap non-scope regions in `{ }` to
bound them (see `examples/logs_file_sink/multithread_file_sink.cpp`).

### Custom presets

To override settings without changing tracked files, create `CMakeUserPresets.json` (git-ignored) in the project root. A preset defined there can inherit from any base preset in `CMakePresets.json`.

Example — use a local kit checkout instead of fetching from GitHub:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "macos-local-kit",
      "inherits": "macos",
      "cacheVariables": {
        "FETCHCONTENT_SOURCE_DIR_KIT": "${sourceDir}/../kit"
      }
    }
  ]
}
```

Then build with:

```bash
python scripts/cmake_build.py --preset macos-local-kit --build
```
