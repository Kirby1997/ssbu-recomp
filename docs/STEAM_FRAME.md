# Steam Frame

What is specific to the native aarch64 Linux build on the Valve Steam Frame (`setup.py --target linux-arm64`).
Everything general about Linux is in [BUILDING_LINUX.md](../BUILDING_LINUX.md).

**Status: in progress, the game does not boot on the Frame yet.**

## Building

From an x86-64 PC: `source toolchain/env.sh`, with the Frame's sysroot copied by `toolchain/sync_sysroot.sh`.
Natively on the Frame: `toolchain/build_deps_on_device.sh` builds SDL3 into `~/ssbu-port/deps/sdl3`, then
`setup.py --target linux-arm64` (`--target linux` on an aarch64 host means the same).

## Game data

The runtime finds the dump itself (`runtime/datapaths.c`), in this order: `--data`, `SSBU_DATA` / `SSBU_ROMFS`, `data=` in
`~/.config/ssbu-recomp/config`, then the build's own layout.

When the data are missing or unusable (no `data.arc` or `.nrr`, packages that still need `tools/extract/dump_folder.py`,
the other game build, missing system fonts) and a display is present, the game opens its own picker before the game
window. It names the problem and lets you choose the folder:

| Button | Does |
|---|---|
| A | opens the folder |
| B | goes up |
| X | uses the folder shown |
| Y | lists places such as the SD card and USB drives |

The keyboard and mouse work too. In Desktop Mode the system folder dialog is offered first (never under Game Mode's
gamescope). The folder is checked and saved as `data=`.

To change it later, start once with the picker: in Steam's launch options `%command% --pick-data` or
`SSBU_PICK_DATA=1 %command%`. Scripted runs, `SSBU_SHOW=0` and runs without a display print the reason and exit with code 9
instead.

## Crash reports

On a crash the game writes `<exe>_crash.txt` to the user data folder (`SSBU_REPORT_DIR` for another one): the signal,
host and guest call stacks as module+offset, registers and the stack. It is written without malloc or stdio, on a stack of
its own, so a stack overflow is reported too. The process then dies of the original signal (exit status and core dump as
usual); `SSBU_CRASH_EXIT=1` exits with code 4 instead, as on Windows.

## Waiting for a Frame run

Written and tested only on other machines (macOS arm64) so far. Run these on the device before relying on them:

| Script | Checks |
|---|---|
| `tests/port/threads/run_frame.sh` | stacks and `__thread` below 2^47, fibers, fiber-local storage, critical sections |
| `tests/port/crash/run_frame.sh` | crash reports (gcc, clang, fortify, PAC) |
| `tests/port/picker/run_frame.sh` | the picker's logic and its SDL window with a virtual gamepad |
| `tests/port/boot`, `tests/port/fs` | game data lookup and file layer after the upstream merge |
| `tests/arm_host/run_frame.sh` | lifter memory ordering, FP and NEON |

## Playing against upstream builds

This fork's lifter matches the Switch more closely in a few edge cases (CLREX, FRECPS / FRSQRTS of 0 and infinity, some
LSE / LDAPR encodings that now trap instead of being skipped), on every host, Windows included. A fork build and an upstream
build are therefore not guaranteed to simulate identically, which matters for a rollback match between them.

## Graphics (Adreno 750 / Turnip)

The game draws with the Vulkan renderer (`runtime/nvn_vk*.c`, the default). The Frame's GPU is an Adreno 750 on Mesa's
Turnip driver. Everything below was checked against one real Steam Frame report: vulkancapsviewer 4.13 on SteamOS 0.3.0,
Turnip `Mesa 26.3.0-devel (git-8aa73b4b19)`, Vulkan 1.4.359, driver ID 18. It is attached to
[vulkan.gpuinfo.org issue #139](https://github.com/SaschaWillems/vulkan.gpuinfo.org/issues/139). Nothing here has run on
a Frame yet.

### Expected to work

The renderer needs a Vulkan 1.3 device with `dynamicRendering`, `synchronization2`, `VK_KHR_swapchain` and BC textures,
and the translated shaders target SPIR-V 1.6. Turnip has all of these. It also has every optional extension the renderer
uses:

- `VK_EXT_graphics_pipeline_library`, with fast linking
- all seven extended-dynamic-state-3 states
- `VK_EXT_vertex_input_dynamic_state`
- `VK_KHR_push_descriptor` (32 descriptors)
- custom border colour, provoking vertex, 8-bit indices, list restart, vertex attribute divisor

So the fastest path is used: "STAGE LIBRARIES" (a draw links its pipeline, nothing is compiled during a fight).

These are present too:

- ASTC: no CPU decode is needed
- the 24-bit depth formats `X8_D24` and `D24S8`, plus `D32S8`
- all 16-bit colour formats
- `A2B10G10R10_SNORM` as a vertex format
- logic op, dual-source blending, depth clamp, wide lines, `shaderInt64`, clip distances, gather offsets, layer and
  viewport output, draw parameters, sampler min/max

The translator rejects geometry and tessellation stages on every GPU, and the renderer uses no transform feedback.
Turnip's support for those is therefore not needed.

### Fallbacks (none expected to be taken on the Frame)

Each one is chosen from the feature the driver reports, never from the vendor:

| Missing | Falls back to |
|---|---|
| graphics pipeline library | whole pipelines, made by the workers |
| extended dynamic state 3 | the state goes back into the pipeline key |
| dynamic vertex input | the vertex layout goes into the key (`pipes_novi.bin`) |
| push descriptors | an allocated set per draw |
| ASTC | CPU decode to RGBA8 |
| 24-bit depth | 32-bit float depth |
| `A2B10G10R10_SNORM` vertex fetch | decoded in the vertex shader |

The fork adds two checks for limits where Turnip is smaller than desktop GPUs:

- A storage-buffer range above `maxStorageBufferRange` is cut to it. Turnip's limit is 128 MB; desktop drivers have
  1 GB or more.
- A compute program beyond the compute limits is named in the log. Shared memory is 32 KB on Turnip, against up to
  48 KB on Maxwell.

On Linux, an early device loss on a GPU that is not NVIDIA no longer switches `graphics.ini` to OpenGL. That renderer
needs NVIDIA's OpenGL.

### Open risks, most likely first

1. **Feedback loops on a tiler.** Stage effects such as fog and soft particles sample the depth target, and sometimes a
   colour target, while it is bound. The renderer does this in `GENERAL` layout, which desktop GPUs tolerate. When
   Turnip renders a pass in tile memory (GMEM), such a read sees memory as it was when the pass began. Reads are correct,
   but a draw that writes the target it samples will look different.
   - Signs: `nvn_vk_fb:` lines in the log, then wrong fog or particles.
   - A/B tests: `TU_DEBUG=sysmem` (no tiling), `SSBU_VK_DEPTHCOPY=1` (sample a copy, as on AMD).
2. **A new driver path.** Stage libraries, extended dynamic state 3, dynamic vertex input and push descriptors are used
   together on Mesa 26.3-devel, a combination not tested by upstream.
   - Signs: `no_stage_libraries.txt`, `vkCreateGraphicsPipelines` failures, a lost device.
   - A/B tests: `SSBU_VK_GPL=0`, `SSBU_VK_EDS3=0`, `SSBU_VK_PUSH=0`, one at a time.
3. **Speed.** A tiler pays for patterns that cost little on desktop; see "Performance notes" below.
   - Signs: `nvn_vk_gpu:` with `SSBU_VK_GPUTIME=1`, and `nvn_vk_pass:` with `SSBU_VK_PASSTIME=1`.
4. **Thin device-loss reports.** Turnip has neither `VK_EXT_device_fault` nor `VK_NV_device_diagnostic_checkpoints`, so
   a lost device names only the last operation the renderer recorded.
   - Help: `SSBU_VK_VALIDATE=1` (needs the validation layer), `TU_DEBUG=flushall` or `syncdraw` to locate a hang.
5. **Compute shared memory.**
   - Sign: `compute program #n is beyond this device's limits`.
6. **Presenting under gamescope.** The desktop report offered mailbox, FIFO and immediate on Wayland, but Game Mode's
   gamescope may offer other modes.
   - Sign: the `nvn_vk: swapchain` line names the mode used.

### What to look at in the first log

Expected values come from the report above.

| Line | Expected |
|---|---|
| `nvn_vk: device 0:` | `Turnip Adreno (TM) 750 (integrated GPU, vendor 0x5143 device 0x43051401), Vulkan 1.4.x` |
| `nvn_vk: using device` | every extension yes except `external_memory_host`, `full_screen_exclusive` and `blend_operation_advanced` (none of the three is enabled by the renderer) |
| `nvn_vk: driver:` | `turnip Mesa driver, Mesa 26.x ... (driver ID 18)`; the Mesa version is the fact to quote in a bug report |
| `nvn_vk: extended dynamic state 3` | `yes [blend enable 1, ... depth clamp 1]` |
| `nvn_vk_path:` | `STAGE LIBRARIES + preparation + loading barrier` |
| `nvn_vk: features:` | every value 1 |
| `nvn_vk: limits:` | push constants 256, push descriptors 32, storage buffer range 134217728, offset alignment 4, shared memory 32768, invocations 2048, subgroup 128 (or 64), GPL fast linking yes |
| `nvn_vk: memory:` | one heap of about 14,683 MB, device-local; types `0x7 0xf 0xb 0x11` (unified memory) |
| `nvn_vk: vertex format A2B10G10R10_SNORM` | `a vertex format on this GPU` |
| `nvn_vk: depth targets sampled while bound:` | `sampled in place (GENERAL layout)` (risk 1) |
| `nvn_vk: swapchain` | format 44 (`B8G8R8A8_UNORM`), present mode mailbox or another mode gamescope offers |
| `nvn_vk: descriptors:` | `pushed (VK_KHR_push_descriptor)` |
| `nvn_vk: device loss report:` | `fault information no` |

At exit:

- `nvn_vk_m4: holds of the render thread`: pipelines made during play.
- `nvn_vk: storage buffer ranges cut`: printed only if a range was cut.
- `nvn_vk_fb:`: the feedback passes.

### Performance notes (not acted on yet)

Some of these are inferred from how Turnip works, not measured.

- **Every pass loads and stores every attachment.** Passes always use `LOAD_OP_LOAD` / `STORE_OP_STORE`, and a clear
  inside a pass is `vkCmdClearAttachments`. On a tiler, this means a full read and write of each target per pass.
  `CLEAR` / `DONT_CARE` where the game's first command is a clear, or where depth is not read later, is the first win.
- **Many small passes.** Passes end at every copy, layout change, compute dispatch and feedback split. Turnip's autotune
  sends small passes to system memory, so few run in GMEM. `SSBU_VK_PASSTIME=1` shows the passes.
- **Colour images are created `MUTABLE_FORMAT`** without a format list. Turnip then likely disables UBWC (bandwidth
  compression) on them (inferred). `VkImageFormatListCreateInfo` would keep it.
- **Constant buffers are storage buffers** in the translated shaders. Adreno's compiler can promote uniform-buffer
  loads into its constant registers, but not storage-buffer loads (inferred).
- **Shader cost.** Large translated shaders with many temporaries, `int64` (lowered in software on Adreno) and private
  arrays (scratch memory) mean high register pressure at wave sizes of 64 and 128.
- **Every supported Vulkan 1.0-1.3 feature is enabled**, including robust buffer and image access. Enabling only the
  features in use would let the driver skip bounds checks.
- **The 60 Hz pacing is `display.c`'s.** The Frame's panel refresh and gamescope's own pacing still have to be measured.
