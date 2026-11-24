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

## Cost and images

Prices are bundled with the binary (DeepSeek exposes no pricing API) and are
versioned in `modules/pricing/pricing.cppm`. Per-request cost, price bucket,
cache usage, and the price-table version are stored in the session log.

Image-heavy sessions can be bounded in `~/.sparsity/config.toml` or
`<playground>/.sparsity/config.toml`:

```toml
[images]
max_request_images = 500
max_request_bytes = 41943040  # base64 payload, below the 48 MiB API limit
```

Older images beyond the budget are replaced by a text placeholder containing
their local path, which the model can re-open with `view_image`.
