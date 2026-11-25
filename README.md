# victory-mac

Runs **Grand Theft Auto VI (Xbox Series X|S, retail 1.0.2.1)** on **macOS (Apple Silicon)** from files the user supplies, using Rosetta 2, Wine, and Vulkan over MoltenVK.

The title's x86-64 code runs inside the Windows PE loader under Wine + Rosetta 2 (with missing Zen 2 instructions emulated or rewritten at load time), the console's RDNA2 shaders are translated ahead of time to SPIR-V (`--wave64-emulate --f16-interp-fallback --vs-single-lane`), and D3D12 / Xbox ABI calls are translated to Vulkan on MoltenVK.

> [!IMPORTANT]
> **No game data, ever.** This repository contains **no game files, executables, archives, keys, extracted assets, or translated shaders**. You must supply your own legally obtained Xbox Series 1.0.2.1 copy on disk. Nothing here decrypts or bypasses any protection, and the runtime blocks all network access (`ws2_32` / offline GDK shims).

---

## Status (2026-11-25)

**Not playable yet.** On Apple Silicon, the title boots offline, passes all `ExecuteCommandLists` / `PresentX` calls, dismisses the package splash into the **live first-run controller UI** (`ACCOUNT PICKER (Y)` / `SCREEN NARRATION (OFF) (X)` / `PLAYER (A)`), accepts gamepad/keyboard input to advance past it, and begins decoding/uploading the 4K YUV intro video streams. 3D world rendering is not working yet on Mac because MoltenVK lacks hardware mesh shaders (`VK_EXT_mesh_shader`) and geometry shaders.

| Stage | State |
|---|---|
| Package check & shader build (`make_game.py`) | ✅ Works natively on macOS (`26,503 / 26,503` main shaders, `2,683 / 2,684` library compute, `52 / 52` BVH, `27 / 29` FSPX, `12 / 12` exe programs translated & `spirv-val` clean) |
| PE loader & Rosetta 2 CPU emulation (`gta6rt.exe`) | ✅ Boots retail `1.0.2.1` offline under Wine + Rosetta 2 (`2,678` ADX sites + `10` CLZERO loops rewritten at load, `0` unimplemented imports) |
| Splash → First-run controller UI | ✅ Works on MoltenVK via `--vs-single-lane`, computed I/J (`--f16-interp-fallback`), and geometry-shader-free `RECT_LIST` expansion (`rect_list_vertex()`) |
| Controller / keyboard input | ✅ Works (`SDL3` GameInput shim + `LEONIDA_INPUT_SCRIPT` advances past first-run UI) |
| Intro video stream | ⚠️ Decodes and uploads 4K YUV (`3840×2160` Y + `1920×1080` U/V), currently slow (~400–530 ms/pass) with an Apple AGX `bytes_per_row` warning |
| 3D World & menus past intro | ❌ Blocked on `--mesh-compute` runtime `DispatchMesh` support (`14,685` mesh PSOs refused without `VK_EXT_mesh_shader`) |

### Tested hardware

- **Mac:** Apple M3 Pro (**12-core CPU**, **18-core GPU**)
- **OS & translation stack:** macOS 27.2.1, Rosetta 2 (`1.0.0.0.1793943009`), WineHQ Stable 11.0 (`11.0_1`), MoltenVK 1.4.1 / 1.4.2, Homebrew LLVM 23.1.6

---

## Subsystem breakdown

| Subsystem | Today (macOS / Apple Silicon) | Needs |
|---|---|---|
| **Loader & CPU** | Maps `game_scarlett_master.exe` + 4 package DLLs under Wine + Rosetta 2; rewrites `2,678` `ADCX`/`ADOX` sites and `10` `CLZERO` loops at load (`adx_rewrite.h`, `x86_length.h`) and emulates SSE4a/CLWB/MONITORX | – |
| **Runtime services** | Offline user/profile, packages, `xtaskqueue`, DirectStorage (`zlib` + `BCPack`) | Speech synthesis is silent by default |
| **Network** | Offline by design (`ws2_32` / sandboxed) | – |
| **Input** | SDL 3.4.20 virtual gamepad (keyboard + mouse stand-in pad, `LEONIDA_INPUT_SCRIPT`) | Trigger rumble |
| **GPU: UI & 2D** | D3D12 → Vulkan on MoltenVK; geometry-shader-free `RECT_LIST` (`rect_list_vertex()`, `4096/4096` pixels exact in `d3d12_rect_list_test`); live FPS + frame pacing (`p50`/`p95`/target ms) in the window title | Speed up 4K video upload & resolve AGX `bytes_per_row` assertion |
| **GPU: 3D world** | `vk_pipeline_check` creates `2,234 / 2,236` VS pipelines on MoltenVK (`--vs-single-lane`); non-buildable draws drop cleanly instead of failing the submission | Runtime `--mesh-compute` (`DispatchMesh` → compute + `vkCmdDraw`), geometry-shader-free fallback for `195` GS, depth-bounds (`203` PSOs) |
| **Shaders** | RDNA2 → SPIR-V with `--wave64-emulate --f16-interp-fallback --vs-single-lane`; computed I/J pairs rebuilt from derivatives; `26,503 / 26,503` main shaders pass `spirv-val` | `20` compute shaders > 32 KiB Apple shared memory limit; `2` VS using `CullDistance` |
| **Textures** | Console XG detiling & staged/host uploads; `700` slot-70 views active in boot | Faster 4K linear plane upload path on MoltenVK |

See [docs/macos.md](docs/macos.md) for full measurements, Rosetta 2 RFLAGS findings, and Wine test results (`281 / 301` CTest targets passing under Wine + Rosetta 2).

---

## Getting started (macOS)

### Prerequisites

```sh
softwareupdate --install-rosetta --agree-to-license   # Rosetta 2
brew install python llvm lld spirv-tools cmake ninja molten-vk vulkan-headers vulkan-loader xwin
brew install --cask dotnet-sdk                        # .NET 8+ for rpf9dump
```

1. **MSVC CRT & Windows SDK** (accepting Microsoft's license):
   ```sh
   xwin --accept-license --sdk-version 10.0.26100 splat --use-winsysroot-style --preserve-ms-arch-notation \
        --output ~/.local/share/xwin-sdk
   ```
2. **WineHQ 11.0** (`Wine Stable.app` in `~/Applications` or `/Applications` from `Gcenx/macOS_Wine_builds`, e.g. `wine-stable-11.0_1-osx64.tar.xz`).

### Build & run from your own copy

```sh
git clone --recursive https://github.com/AeroUp/victory-mac.git
cd victory-mac
./Build.sh --find      # locate your Xbox Series 1.0.2.1 copy
./Build.sh --check     # verify package files (builds nothing)
./Build.sh             # translate shaders & cross-build runtime into ~/Leonida (or ~/Documents/Leonida)
```

Run offline with the launcher script generated in your output directory:

```sh
./gta6rt.sh --dry-run
./gta6rt.sh --init
./gta6rt.sh --run
```

### Useful environment variables

| Variable | Effect |
|---|---|
| `LEONIDA_TITLE_FPS=0` | Disable live FPS & frame pacing (`p50`/`p95`/target ms) in the window title (enabled by default) |
| `LEONIDA_SHADERS=<path>` | Override translated SPIR-V directory |
| `LEONIDA_KEYBOARD_PAD=always` | Use keyboard/mouse as Xbox controller (`Enter`=A, `Backspace`=B, `WASD`/arrows, mouse=right stick) |
| `LEONIDA_INPUT_SCRIPT="a@65,a@80"` | Timed scripted button presses (`button@seconds`) |
| `LEONIDA_CAPTURE=<dir>` | Dump presented swapchain frames as `.ppm` (`LEONIDA_CAPTURE_EVERY`, `LEONIDA_CAPTURE_LIMIT`) |
| `LEONIDA_COMPUTE_WARMUP=0` | Disable background compute-pipeline warm-up thread |
| `LEONIDA_STRICT_REPLAY=1` | Fail command lists on unbuildable pipelines instead of dropping those draws |
| `LEONIDA_ADX_REWRITE=0` | Disable load-time ADX stub rewrite (falls back to `#UD` emulation) |

---

## Sources & Credits

This repository builds on and references work from across the community:

- **[ubiquitarianism/leonida](https://github.com/ubiquitarianism/leonida)** — upstream Leonida continuation (build system, RDNA2 → SPIR-V translator, PE loader, D3D12 → Vulkan runtime, and docs)
- **[Leonida (original)](https://github.com/ItsNotRexed/Leonida)** — original project by NotRexed and contributors
- **[VIctory](https://github.com/Marvisak/VIctory)** & **[fxceless/victory-fork](https://github.com/fxceless/victory-fork)** — Xbox Series X|S → Windows compatibility layer & D3D12 progress reference
- **[ScarlettPort](https://github.com/ZigZagBoi1/ScarlettPort)** — community VIctory/XCL stack & rendering status reference
- **[VIlkan](https://github.com/SebSilent/vilkan)** — Linux/Vulkan & wave64 exchange reference
- **[rpf9-explorer](https://github.com/breadbbbb/rpf9-explorer)** — RPF9 / texture-pool / AWC decoders (`third_party/rpf9-explorer`)
- **[Mesa](https://gitlab.freedesktop.org/mesa/mesa)** (MIT) — GFX10 descriptor layouts & ACO/RADV reference
- **[libHttpClient](https://github.com/microsoft/libHttpClient)** (MIT) — `third_party/xtaskqueue`
- **[MoltenVK](https://github.com/KhronosGroup/MoltenVK)**, **[Wine](https://www.winehq.org/)**, **[LLVM](https://llvm.org/)**, **[SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools)**, and **[xwin](https://github.com/Jake-Shadle/xwin)**

Grand Theft Auto and all related names are trademarks of their owners. This project is not affiliated with or endorsed by them.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE). Third-party components keep their own licenses.
