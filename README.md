# ENRAGE

ENRAGE is an experimental native runtime and tooling project.

The repository is intentionally early and intentionally light on public documentation while the foundations are being built.

## Status

Pre-alpha. Interfaces, formats, and behavior may change without notice.

## Building

A C++23 compiler and CMake 3.24 or newer are required.

```sh
cmake -S . -B build -DENRAGE_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Layout

- `include/enrage` public SDK surface
- `src/core` runtime-neutral core
- `src/adapters` host integration boundary
- `src/platform` platform-specific primitives
- `src/host` development host
- `tests` regression tests

More documentation will be published when the project is ready for it.
