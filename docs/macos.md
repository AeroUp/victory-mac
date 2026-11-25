# macOS

What a Mac user can do today, from one page. The build of the shaders and assets runs natively, the Vulkan test
tools build natively and run on MoltenVK, and the loader (`gta6rt`) is a Windows program that runs under Wine, which
on an Apple Silicon Mac itself runs under Rosetta 2. Nothing is playable on any platform yet (README, *Project
status*). The Linux page, [linux.md](linux.md), explains why there is no native loader; on a Mac there is one more
reason: the game's code is x86-64, and on Apple Silicon only Rosetta 2 runs x86-64 code.

Everything below was checked on an Apple M3 Pro, macOS 27.2.1, Rosetta 2 package `1.0.0.0.1793943009`, Homebrew
LLVM 23.1.6, MoltenVK 1.4.2 and WineHQ stable 11.0 (the `11.0_1` macOS build).

## What runs where

| Piece | On a Mac | How it is checked |
|---|---|---|
| `make_game.py`: check, shaders, assets, pipelines | Native | CI (`macos-python`); the tests below |
| `Build.sh`, `Mods.sh` | Native (`/bin/sh`); pick Homebrew's `python3.N` over Apple's 3.9 `python3` | `tests/test_shell_launchers.py` |
| Shader translator, Vulkan test tools (`vk_harness`, ...) | Native (arm64), on MoltenVK | CI (`macos` job); `ctest` on this Mac: 74 of 74 pass |
| `gta6rt`, its shims and their tests | Windows program, cross-built with clang-cl and xwin, run under Wine and Rosetta 2 | By hand on this Mac: 281 of 301 pass on `9002b420b` ([Under Wine](#under-wine)) |

## Install the tools

```sh
softwareupdate --install-rosetta --agree-to-license   # Rosetta 2, if `arch -x86_64 /usr/bin/true` fails
brew install python llvm lld spirv-tools cmake ninja molten-vk vulkan-headers vulkan-loader xwin
brew install --cask dotnet-sdk                        # the assets stage builds rpf9dump (.NET 8 or newer)
```

- **Python**: `/usr/bin/python3` is Apple's 3.9. `./Build.sh` and `./Mods.sh` try `python3.19` down to `python3.12`
  before `python3`, so Homebrew's newest Python is found without changing `PATH`.
- **LLVM and lld** are keg-only in Homebrew: nothing is linked onto `PATH`. `tools/mc_command.py` finds `llvm-mc` in
  `/opt/homebrew/opt/llvm/bin` (`/usr/local/opt` on an Intel Mac, and `llvm@<N>` after `llvm`), and `make_game.py`
  appends the `llvm` and `lld` keg folders to `PATH` for the cross-build (after everything already there, so Apple's
  clang stays the compiler of the native builds). `LLVM_MC` still wins when it is set.
- **The MSVC CRT and Windows SDK** for the cross-build come from Microsoft through xwin. Running it means accepting
  Microsoft's licence, so the build never does it for you:

  ```sh
  xwin --accept-license --sdk-version 10.0.26100 splat --use-winsysroot-style --preserve-ms-arch-notation \
       --output ~/.local/share/xwin-sdk
  ```

  It writes about 630 MB. On the default case-insensitive APFS volume xwin warns that it turns its symlinks off; the
  build works without them.
- **Wine**: Homebrew disabled its WineHQ casks (`wine-stable`, `wine@staging`, `wine@devel`) on 2026-09-01 because
  they fail Gatekeeper. The same builds are WineHQ's macOS packages on GitHub
  (`Gcenx/macOS_Wine_builds`, e.g. `wine-stable-11.0_1-osx64.tar.xz`, SHA-256
  `b50dc50ec7f41d58b115a6b685d4d1315ba3c797bd3aa0f49213f2703cb82388`, the value the disabled cask still lists).
  Unpacking that archive into `~/Applications` or `/Applications` gives `Wine Stable.app`; `gta6rt.sh` and the CMake
  toolchain look there when `wine` is not on `PATH`. Its `wine` is an x86-64 program and brings its own x86-64
  `libMoltenVK.dylib`: Homebrew's MoltenVK is arm64 and cannot load into it.

## Build the port

With your own copy of the game already readable on disk:

```sh
./Build.sh --find      # list the copies found: the home folder, Downloads, Documents, /Volumes, ...
./Build.sh --check     # check your copy, build nothing
./Build.sh             # build into ~/Documents/Leonida Legacy (~/Leonida Legacy when iCloud syncs Documents)
```

The default stages include `runtime` when the cross-build has what it needs (clang-cl, lld-link, llvm-lib, llvm-rc,
llvm-mt, CMake and the xwin splat); otherwise it is left out with one line saying which of those is missing. The
cross-build passes `-DLEONIDA_SDL3_FETCH=ON`, which downloads the pinned, hash-checked SDL3 source once: without the
GameInput shim the loader binds `GameInputCreate` to the host's `GameInput.dll`, Wine's is a stub that fails it, and
the retail boot stops there before its first command list ([gameinput.md](gameinput.md)).

The stage writes `<out>/gta6rt.sh`. On a Mac it uses `WINE` when set, else `wine` on `PATH`, else the first of
`Wine Stable`, `Wine Staging` and `Wine Devel` in `~/Applications` and `/Applications`, and it exports
`ROSETTA_ADVERTISE_AVX=1` unless you set it yourself (next section).

## Rosetta 2 and the game's instructions

The executable is built for the console's Zen 2 and uses its instructions without asking CPUID
([cpu_features.md](cpu_features.md)). Measured here with a probe that runs each instruction in its own process
under Rosetta and checks the result:

| Instructions | Under Rosetta 2 |
|---|---|
| AVX, AVX2, FMA3, F16C, BMI1, BMI2, MOVBE, AES-NI, PCLMULQDQ, POPCNT, LZCNT, CLFLUSHOPT, PREFETCHW, CMPXCHG16B, RDRAND, RDTSCP, XGETBV | Run, results right |
| **ADX** (`adcx`, `adox`, every form) | `#UD` (SIGILL) |
| SSE4a (`insertq`, `extrq`), CLZERO, MONITORX/MWAITX, CLWB | `#UD`; emulated by `runtime/src/cpu_emulation.h` |
| RDSEED, SHA, AVX-512, VAES, VPCLMULQDQ | `#UD`; the executable uses none of them |

CPUID leaves AVX, OSXSAVE, AVX2, FMA, F16C, MOVBE, BMI1/2 and LZCNT out unless `ROSETTA_ADVERTISE_AVX=1` is set,
although the instructions run either way. `gta6rt --init` and `--run` check CPUID for AVX (`runtime/src/host_cpu.h`) and
stop with exit code 4 without it, so `gta6rt.sh` sets the variable.

### ADX

ADX is on every desktop CPU since Broadwell and Zen, so it was counted but not flagged in
[cpu_features.md](cpu_features.md); Rosetta 2 does not have it, and neither does a Haswell. `cpu_emulation.h` now
decodes `ADCX` and `ADOX` (register and memory sources, `REX.W`, SIB, RIP-relative and 32-bit addressing) and does
them on the faulting thread's context, writing only CF or only OF. A memory source read through an FS or GS override
stays `#UD`. `cpu_emulation_adx_test` checks the decoder, the arithmetic and the addressing on every OS, and on an
x86-64 Linux or macOS host runs real interleaved ADCX/ADOX carry chains through a SIGILL handler that uses the same
code; on an Apple Silicon Mac CMake also builds it as x86-64 (`cpu_emulation_adx_rosetta`), so it runs under Rosetta:
544 instructions emulated, every result right.

### Rosetta 2 hands a SIGILL handler wrong flags

That test has to set its flags with `POPFQ`, because of what the measurement turned up: after an instruction Rosetta
cannot translate, the RFLAGS in the SIGILL context, and the flags the thread continues with after the handler returns,
are often not the ones the code computed. Each row ran `<instruction>; <trapping ADCX>` from a fresh executable buffer
(Rosetta's JIT path, as Wine's code is) and compared the context with a control run where a 6-byte NOP replaced the
ADCX:

| Instruction before the trap | Context vs the real flags |
|---|---|
| `xor`, `and`, `or`, `test` (clear CF) | CF stale (1) |
| `add`, `sub`, `neg`, `shr`, `bt`, `mul` | CF inverted or stale |
| `clc` (keeps OF) | OF lost |
| `stc`, `cmc`, `cmp`, `adc`, `popfq`, `sahf`, `pushfq; popfq` | Right |

16 of 23 cases were wrong. A memory fault is not affected: the same sequences ending in a load from a `PROT_NONE`
page give exact flags in the SIGSEGV context. So the #UD emulation alone avoids the crash, but an ADCX/ADOX whose
carry was set by the instruction just before it (a compiler clears CF and OF with `xor reg, reg` before an ADX chain)
can compute with the wrong carry. On a real CPU without ADX the kernel's context is exact, and so is the emulation.
The other emulated instructions write no flags, but the flags live across them come back the same way after the trap.

### ADX sites are rewritten before the game runs

So on a host without ADX the loader rewrites them before any of the image's code runs (`runtime/src/adx_rewrite.h`):
each ADCX or ADOX becomes a jump to a stub that does the same addition with `ADC` and keeps every other flag through
`PUSHFQ`/`POPFQ`, which Rosetta keeps right. Nothing traps any more, so the wrong trap flags never come into play. A
fault in a stub's memory read is moved back to the original instruction, with its RSP and flags, before the game's
own handlers see it.

The sites are found by walking each executable section one instruction at a time (`runtime/src/x86_length.h`),
restarting at every `.pdata` function start. Measured on 1.0.2.1:

| What | Count |
|---|---:|
| Instructions in the 299,257 `.pdata` ranges (the walk) | 22,435,491 (`dumpbin`: 22,435,554, [cpu_features.md](cpu_features.md)) |
| ADCX / ADOX | 1,409 / 1,269, all 64-bit register forms |
| Of those inside a `.pdata` range | 0: all are in leaf routines between functions |
| `[66\|F3] [REX] 0F 38 F6` byte patterns in the code sections | 2,678, each one a site the walk decodes |
| Sites rewritten by `gta6rt` at load | 2,678; none left to the `#UD` path |
| ADX in the four package DLLs | 0 |

The length decoder agrees with llvm-mc's disassembler on every one of 518,587 instructions it decodes in 1,000,000
generated cases (`runtime/test/check_x86_length.py`, CTest `x86_length`); the cases where they disagree on whether
bytes are an instruction at all are all encodings a Zen 2 raises `#UD` on. `adx_rewrite_test` runs rewritten ADX
chains (register, memory, SIB, RSP- and RIP-relative sources, the 32-bit form, every combination of the six
arithmetic flags, and the `stc; xor; adcx` case above): 1,088 cases exact natively under Rosetta
(`adx_rewrite_rosetta`) and under Wine, where it also faults a stub's read and checks what the caller's `__except`
sees. `LEONIDA_ADX_REWRITE=0` turns the rewrite off; `=1` turns it on on a CPU that has ADX, to compare the stubs with
the CPU.

A 4-minute boot to the frame loop reached none of the sites (upstream `main` had no ADX emulation then, and nothing
trapped), so which part of the game uses them is not known.

## Booting the retail executable

Measured on the M3 Pro above with the player's own 1.0.2.1 copy, `gta6rt` cross-built from `main` at `42537e7b6`,
`LEONIDA_D3D12=vulkan`, no shaders built yet, and the network blocked for the whole Wine process tree with
`sandbox-exec` (a profile that denies IP traffic and the DNS socket; the executable's `ws2_32.dll` imports bind to the
host's, [boot.md](boot.md)):

| Pass | Result |
|---|---|
| `--dry-run` | Exit 0: the executable and its four package DLLs mapped, 0 unimplemented imports |
| `--init` | Exit 0: the 15 trace events [runtime_imports.md](runtime_imports.md) lists for Windows |
| `--run` | The title's runtime services, the D3D12 device on MoltenVK, 585 command lists, then an `int3` 6 s in |

The stop is the title's assert on an `E_NOTIMPL` from `CreateCommandSignature`: one of the five signatures it creates
at startup is a `DISPATCH_MESH`, and the Vulkan layer refuses that without `VK_EXT_mesh_shader`, which MoltenVK does
not have. Any GPU without mesh shaders stops there. With that signature allowed (a local experiment, not a fix: mesh
work still has nowhere to run), the same boot ran its frame loop for the full 240 s: 10,387 `PresentX` in the title's
own window, 20,775 `ExecuteCommandLists`, no exception. Every pipeline creation failed, as it must with no shaders
built. A Mac picture needs the shaders built from the player's copy and the mesh-to-compute fallback for GPUs without
mesh shaders (`--mesh-compute` and the runtime side, claimed in the coordination issue).

### With the shaders built

The shaders stage on this Mac (6 jobs, `--wave64-emulate`, and `--f16-interp-fallback`, which it picks for MoltenVK)
carved, unpacked and disassembled all 26,503 shaders and translated them in about 22 minutes, then stopped at its
completeness check: 207 pixel shaders were refused, every one of them `computed I/J pair under --f16-interp-fallback`
(95 `PS_TrailmapDeformData` variants and the cascaded shadow-map filters `PS_ShadowRender_CSM_*` among them). The stage
then skips what comes after the check (`native_hashes.txt` and the programs in the upscaler, the BVH builder, the
pipeline library and the executable), and without those the loader finds no shader for 2,689 compute pipelines and
for the native graphics pipelines. The translator now rebuilds a computed pair's vertex values from screen-space
derivatives where it has no per-vertex inputs ([design.md](design.md), SHADER-177 row).

With those steps run by hand, a 5-minute boot showed only the package splash: 2,922 of about 2,925
`ExecuteCommandLists` were refused. MoltenVK cannot convert a vertex shader that reads its subgroup lane ("Subgroup
builtins are not available in this type of function": Metal vertex functions have no SIMD-lane index), 1,113 of the
2,236 vertex shaders read it, and one draw whose pipeline could not be built failed its whole command list. Two
changes address that:

- `--vs-single-lane` (picked by `make_game.py` on MoltenVK) translates each vertex shader invocation as a wave whose
  only running lane is lane 0, with no subgroup operation left ([design.md](design.md), "Vertex shaders without a
  lane index"). With it `vk_pipeline_check` creates 2,234 of the 2,236 vertex pipelines on this Mac; the other two
  write `CullDistance`, which Metal does not have.
- A draw whose pipeline cannot be built is dropped and the rest of its list runs, as a draw with an unresolvable
  resource already was (`LEONIDA_STRICT_REPLAY=1` restores the failure).

Still open on a Mac: no mesh shaders (9,282 of the corpus; `--mesh-compute` and its runtime side), no geometry
shaders (195, and the generated stage for rectangle lists), 20 compute shaders that declare 33 to 41 KB of workgroup
memory against Apple's 32 KB (`--max-shared-memory` is passed for the BVH builder's programs only), `CullDistance` in
two vertex shaders, and MoltenVK lacking `shaderDenormPreserveFloat16`, so `vk_pipeline_check` skips every module
translated with float controls (it creates them with `--no-float-controls`).

## Under Wine

Configure the cross-build and run its tests through Wine. The toolchain makes Wine CTest's emulator, wrapped in
`runtime/build/windows/wine-rosetta.sh`, which sets `ROSETTA_ADVERTISE_AVX=1`:

```sh
export PATH="$PATH:$(brew --prefix llvm)/bin:$(brew --prefix lld)/bin"
cmake -S runtime -B runtime/build/windows -G Ninja --toolchain "$PWD/runtime/cmake/clang-cl-xwin.cmake" \
      -DXWIN_DIR="$HOME/.local/share/xwin-sdk" -DLEONIDA_SDL3_FETCH=ON
cmake --build runtime/build/windows --parallel
export WINEPREFIX="$HOME/.wine-leonida" WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d"
"$HOME/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine" wineboot --init
ctest --test-dir runtime/build/windows -j 6 --timeout 180 --output-on-failure
```

`winedbg.exe=d` keeps Wine from starting its debugger for a crashed test, which otherwise waits until the time
limit ([testing.md](testing.md#known-problems)).

The cross-build on this Mac (clang-cl 23.1.6, MSVC 14.44 and SDK 10.0.26100 from xwin 0.10.0, SDL3 3.4.20) builds all
1,026 targets. Under WineHQ 11.0 and Rosetta 2, with MoltenVK 1.4.2 from the Wine bundle, on `main` at `9002b420b`, 281
of 301 tests pass and 2 skip (`device_proxy`, `device_proxy_trace`: no WARP adapter under Wine). `cpu_emulation` passes
there: all six emulated instruction kinds, ADX included, go through the runtime's vectored handler under Wine. On
`42537e7b6` only the loader, CPU and ADX tests were run again (15 tests): 14 pass, `loader_tls` among them, and `loader`
keeps its one OLD-8847 check. The 20 failures on `9002b420b`:

| Tests | What fails | Known elsewhere |
|---|---|---|
| `bcrypt_rng`, `bcrypt_rng_seeded`, `gameinput_host`, `root_sig`, `run_startup_cmdline`, `dstorage` | Wine's own answers differ from Windows' | Same under Linux Wine (coordination issue, TEST-WINE-RUNTIME-TESTS) |
| `loader_tls` | The `/GA` fixture's index-0 TLS reads are not rewritten (7 checks) | The fixture as clang-cl and lld-link build it; claimed in the coordination issue |
| `loader` | "preferred base is taken and the image has no relocations" (1 of 95 checks) | OLD-8847 in the coordination issue |
| `file_system` | One overlay case | A finding in the coordination issue |
| `xmem` | A mapped page's protection, its working-set charge, and write faults on read-only 4 KiB parts of pages (4 checks) | Not checked elsewhere. An Apple Silicon page is 16 KiB; Rosetta cannot give a 4 KiB part of one its own protection |
| `dxgi_swapchain` | MoltenVK's Metal validation rejects a texture description; Wine's `vkCreateImage` asserts | Not checked elsewhere |
| `d3d12_heap_properties`, `d3d12_residency` | The layer's answers differ from the host's (heap flags `0xc0` against `0x0`; `EnqueueMakeResident` flag 2 gives `E_INVALIDARG`) | Not checked elsewhere |
| `d3d12_uav_clear`, `d3d12_clear`, `d3d12_clear_formats`, `d3d12_descriptor`, `d3d12_existing_heap`, `d3d12_indirect_dispatch_count`, `d3d12_dxr_validation_khr` | Clears refused ("invalid clear"), a descriptor readback, a placed buffer's GPU address, synthetic buffer allocation | Not checked elsewhere; not looked into yet |

`vk_device_filter_tools` used to fail here too, for two reasons in the test, not the tools: it started the Windows
tools directly (Linux can, through Wine's binfmt_misc entry; macOS gives "Exec format error"), and it took MoltenVK's
own `[mvk-info] GPU device:` log line for a chosen device. It now gets CTest's emulator as `--runner` and looks for
the tools' `device:` line at a line start.

[testing.md](testing.md#the-windows-runtime-on-linux-under-wine-or-proton) has the Wine test runner and what fails
under Wine on any host.
