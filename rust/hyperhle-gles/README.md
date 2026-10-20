# hyperhle-gles

Standalone OpenGL ES core for the S5LBox Android port, extracted from
[HyperHLE](https://github.com/HyperHLE/HyperHLE), a touchHLE fork.

## What is vendored

- `src/gles/gles_generic.rs`, `util.rs`, `gles1_native.rs`, `gles2_native.rs`,
  `gles3_native.rs` — unmodified upstream sources (MPL-2.0).
- `src/matrix.rs` — unmodified upstream matrix helper (MPL-2.0).
- `gl-bindings/` — upstream `gl_generator` bindings crate (MPL-2.0), with the
  workspace-inherited manifest fields made concrete.
- `native/pvrt_decompress_wrapper/lib.cpp` and
  `third_party/PVRTDecompress/` — Imagination Technologies' PVRTC software
  decompressor as vendored by HyperHLE. See
  `third_party/PVRTDecompress/LICENSE.md`.

## What is replaced

The upstream core imports four things from the rest of touchHLE; this crate
provides them:

| Upstream | Here |
| --- | --- |
| `crate::window` (SDL2) | `src/window.rs`, EGL only |
| `crate::paths` | `src/paths.rs`, cache directory override |
| `crate::image::decode_pvrtc` | `src/image.rs`, calls the vendored decompressor |
| `log!` / `log_dbg!` / `log_once!` | `src/log.rs`, logcat on Android |

No upstream GLES file was edited; the crate reproduces the `super::` layout
the backends expect (`src/gles/mod.rs`).

## C ABI

`src/ffi.rs` exposes `s5l_gles_create`, `s5l_gles_destroy`,
`s5l_gles_make_current`, `s5l_gles_swap_buffers`, and a small set of ES 1.1
calls for the first end-to-end frame. The guest-to-host call translation layer
will extend this ABI.

## Building

```sh
cargo ndk -t arm64-v8a -t x86_64 -o ../../app/src/main/jniLibs \
  build --release --manifest-path android/rust/hyperhle-gles/Cargo.toml
```

CI does this in `.github/workflows/android-build.yml`.
