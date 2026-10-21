# S5LWinBox

**S5LWinBox** is a high-performance desktop emulator for **iPhone OS 3.1.3 (iPhone 2G / 3G, S5L8900 SoC)** on Windows and Linux, featuring hardware-accelerated OpenGL ES via **HyperHLE**, real-time dyld/symbol tracking, stereo audio playback, and an integrated real-time Web Inspector.

---

## Features

- **SoC Emulation:** Full S5L8900 architecture (ARMv6 CPU, MMU, VIC, Timers, CLCD, WM8991 audio, MTZ2 touch, Power management).
- **Graphics Acceleration:** Standalone Rust-based **HyperHLE** OpenGL ES 1.1/2.0 core (`hyperhle_gles.dll` on Windows, `libhyperhle_gles.so` on Linux) with HLE dispatch.
- **Audio Output:** Real-time 48 kHz stereo PCM audio via SDL2.
- **Input:** Pixel-perfect touchscreen mouse mapping and hardware button emulation.
- **Drag & Drop:**
  - Drag the supported iPhone 3G IPSW (`iPhone1,2_3.1.3_7E18_Restore.ipsw`) onto the window to import firmware. The iPhone 2G 3.1.3 IPSW is currently rejected by the known-good firmware verifier.
  - Drag an **IPA** package onto the window to inspect and install apps/games directly into the guest system.
- **Web Inspector:** Built-in web inspector server (`http://localhost:8080`) providing live streaming logs with category filtering (`kernel`, `panic`, `hle`, `mbx`, `dyld`, `app`), telemetry (FPS, MIPS, retired instructions, TTBR0), and log export.

---

## Keybindings & Controls

| Input | Action |
|---|---|
| **Left Mouse Click & Drag** | Touchscreen touch / gestures |
| **Esc** / **Home** | iPhone Home Button |
| **F1** | Power / Sleep Button |
| **F2** / **F3** | Volume Down / Volume Up |
| **F5** | Silent Switch Toggle |
| **F11** | Fullscreen Toggle |
| **Space** | Pause / Resume Emulation |

---

## Live Web Inspector

While S5LWinBox is running, open any web browser and navigate to:
```
http://localhost:8080
```
The inspector offers:
- Live Server-Sent Events (SSE) log stream
- Category tabs: `Kernel`, `Panic`, `HLE`, `MBX`, `dyld`
- Real-time performance indicators: FPS, MIPS, retired instructions, MMU translation base (`TTBR0`), and machine state
- One-click log export to `s5lwinbox_log.txt`

---

## Building from Source

### Windows prerequisites
- CMake 3.22+
- Visual Studio 2022 (MSVC with C/C++ support) or Clang
- Rust & Cargo (for HyperHLE GLES core)

### Windows build steps

1. **Build HyperHLE GLES:**
   ```cmd
   cd rust/hyperhle-gles
   cargo build --release
   cd ../..
   ```

2. **Configure & Build S5LWinBox:**
   ```cmd
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --config Release
   ```

3. **Run:**
   The output binaries `s5lwinbox.exe`, `SDL2.dll`, and `hyperhle_gles.dll` will be placed in `build/Release/`.
   ```cmd
   build\Release\s5lwinbox.exe
   ```

### Linux (Debian/Ubuntu)

Prerequisites: CMake 3.22+, GCC or Clang, Rust stable/Cargo, and the SDL2 backend development packages below. CMake fetches SDL2; these packages enable X11/Wayland, EGL/OpenGL ES, and audio. Linux release builds target the Ubuntu 22.04 / glibc 2.35 baseline and statically link the GNU C++ runtime, making them compatible with Debian 12 and newer; SDL2, glibc, and display/audio system libraries remain runtime dependencies.

```sh
sudo apt-get install -y \
  build-essential cmake pkg-config \
  libasound2-dev libdbus-1-dev libdrm-dev libegl1-mesa-dev \
  libgbm-dev libgl-dev libgles2-mesa-dev libpulse-dev libudev-dev \
  libwayland-dev libwayland-egl-backend-dev wayland-protocols \
  libx11-dev libxext-dev libxcursor-dev libxfixes-dev libxi-dev \
  libxkbcommon-dev libxrandr-dev libxrender-dev libxss-dev libdecor-0-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target s5lwinbox --parallel
./build/s5lwinbox
```

When Cargo is available, CMake builds and places `libhyperhle_gles.so` beside the Linux executable. Without Cargo, HyperHLE acceleration is unavailable, but the emulator still runs. New Linux machines default to MBX off and QuartzCore's CPU renderer on. After the first firmware boot reaches hibernation, the Linux desktop sends one Power wake so the iPhone lock screen is visible; later iOS auto-lock behavior is unchanged, and F1 wakes the screen again. The graphics profile is written when a work image is created, so an existing image keeps its old settings; create a fresh machine/work image to apply the Linux defaults.

To install firmware, drag `firmware/iPhone1,2_3.1.3_7E18_Restore.ipsw` onto the emulator window. This emulator models the S5L8900 used by iPhone 2G/3G; the iPhone 3GS (`iPhone2,1`) IPSW uses a different SoC and is rejected by the importer. IPSW files are intentionally excluded from Git.

---

## License

Licensed under the MIT License. See [LICENSE](LICENSE) for details.
HyperHLE GLES components licensed under MPL-2.0.
