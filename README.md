# Sparsity
A DeepSeek-in-the-loop in-browser editor based on Monaco and xterm

## Build

Requirements:

- `cmake` >= 4.2, `ninja`, `clang++` (C++20 modules; clang is the tested compiler)
- `node` / `npm` (the frontend is built by CMake)
- `openssl-devel` (HTTPS)
- `poppler-utils` (PDF uploads)
- optional: `clangd`, `jdtls` + JDK 21 (installed at `~/.local/share/jdtls` and
  `~/.local/share/jdk-21`), `ripgrep`

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

## Run

```sh
./build/main/sparsity ~/playground --host 0.0.0.0 --port 13010
```
