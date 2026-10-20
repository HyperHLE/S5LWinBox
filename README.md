# S5LWinBox

**S5LWinBox** is a high-performance desktop emulator for **iPhone OS 3.1.3 (iPhone 2G / 3G, S5L8900 SoC)** for Windows and PC platforms, featuring hardware-accelerated OpenGL ES via **HyperHLE**, real-time dyld/symbol tracking, stereo audio playback, and an integrated real-time Web Inspector.

---

## Features

- **SoC Emulation:** Full S5L8900 architecture (ARMv6 CPU, MMU, VIC, Timers, CLCD, WM8991 audio, MTZ2 touch, Power management).
- **Graphics Acceleration:** Standalone Rust-based **HyperHLE** OpenGL ES 1.1/2.0 core (`hyperhle_gles.dll`) with HLE dispatch.
- **Audio Output:** Real-time 48 kHz stereo PCM audio via SDL2.
- **Input:** Pixel-perfect touchscreen mouse mapping and hardware button emulation.
- **Drag & Drop:**
  - Drag an **IPSW** (`iPhone1,1_3.1.3_7E18_Restore.ipsw` or `iPhone1,2_3.1.3_7E18_Restore.ipsw`) onto the window to auto-import rootfs, NOR, and NAND.
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

### Prerequisites
- CMake 3.22+
- Visual Studio 2022 (MSVC with C/C++ support) or Clang
- Rust & Cargo (for HyperHLE GLES core)

### Steps

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

---

## License

Licensed under the MIT License. See [LICENSE](LICENSE) for details.
HyperHLE GLES components licensed under MPL-2.0.
