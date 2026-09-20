# suyu on macOS (Apple Silicon) - what was wrong and how it got fixed

Notes from getting suyu running on a MacBook Pro (Apple M4 Pro, macOS Tahoe 26.6.2) on
2026-09-19, ending with *The Legend of Zelda: Breath of the Wild* booting, rendering and playing.

Four separate problems are documented here, and they are worth keeping apart:

1. **The prebuilt release in this folder does not work.** Five environment faults stack on top
   of each other, and after fixing all five it still crashes, because the binary was built from
   an incomplete source tree. Nothing recovers it.
2. **Built from source it works, but only after four code fixes.** Stock suyu cannot open a game
   on Apple Silicon.
3. **It then crashes at random.** Heap corruption from an ODR violation and a stack smash, both
   present in every build including the prebuilt one. Two more code fixes.
4. **Block-shaped texture corruption.** Solved: the game aliases render targets, which Metal does
   not allow (Fix 12). The fifty experiments it took are recorded below so nobody repeats them.

All code fixes are applied in this repository's source tree, and the same change set is kept as
`suyu-macos-fixes.patch` beside this file for anyone starting from upstream. How to apply it and
build is in Part 2 under "Applying the patch and installing".

## Outcome

| | |
| --- | --- |
| Prebuilt `suyu.app` from `suyu-macos-arm64.tar.gz` | Never runs. Crashes in its own UI code. |
| Built from source, unpatched | Builds and opens, crashes on every game launch. |
| Built from source, first four patches | BotW boots, renders and plays; dies at random minutes in. |
| Built from source, all six patches | Guard Malloc caught the writer; the crash-free retest at the reproducing setting is recorded in Part 4. |
| Built from source, all patches (Fix 12 included) | Plays with no block artifact. |
| Installed at | `/Applications/suyu.app` |
| Release | `v0.0.4a` on `github.com/kirbycope/suyu-v0.0.4`: a self-contained, ad-hoc signed `suyu.app` (Qt, FFmpeg, Boost, OpenSSL, SDL3, Vulkan loader, MoltenVK and its ICD manifest all inside the bundle) plus the patch |
| Source tree | `~/GitHub/suyu` |

## Part 1 - why the prebuilt release cannot work

Every symptom looked like Apple blocking the app, and for three of the five it partly was. Each
fix revealed a new symptom, which is why it felt like nothing was changing.

### 1. Broken code signature

The bundle shipped with a signature claiming to seal resources it did not have:

```
$ codesign --verify --deep --strict --verbose=2 suyu.app
suyu.app: code has no resources but signature indicates they must be present

$ codesign -dv suyu.app
flags=0x20002(adhoc,linker-signed)
Sealed Resources=none
```

There is no `Contents/_CodeSignature` directory. macOS reports an invalid seal as "damaged",
which looks like a Gatekeeper block but is not one. **This distinction matters:**

- **"Damaged / move to Trash"** is a signature validity failure. Gatekeeper rejects it outright,
  so **no "Open Anyway" button ever appears**. There is nothing to override.
- **"Cannot verify the developer"** is a policy decision, and *that* one does offer Open Anyway.

So if Settings appears to be missing the button, the signature is broken. Stop hunting for the
button and fix the seal. Sign inside-out; Apple discourages `--deep` for signing:

```bash
codesign --force --sign - suyu.app/Contents/Frameworks/libMoltenVK.dylib
codesign --force --sign - suyu.app
```

If that fails with `resource fork, Finder information, or similar detritus not allowed`, a
`com.apple.FinderInfo` attribute is in the way:

```bash
find suyu.app -exec xattr -d com.apple.FinderInfo {} \; 2>/dev/null
```

### 2. Fourteen missing shared libraries

The release is a developer build, not a redistributable app. It bundles only `libMoltenVK.dylib`
and links **24 libraries by absolute `/opt/homebrew` path**. A properly packaged Mac app copies
its dependencies into `Contents/Frameworks` and rewrites install names to `@rpath` - that is what
`macdeployqt` does. This one does not.

```bash
otool -L suyu.app/Contents/MacOS/suyu | grep -o '/opt/homebrew[^ ]*' | sort -u | \
  while read p; do [ -e "$p" ] || echo "MISSING $p"; done
```

```bash
brew install qtbase boost
brew upgrade ffmpeg
```

ffmpeg had to be an **upgrade**, not an install: the binary wants `libavcodec.63` (ffmpeg 9) and
the installed 8.1.1 only provides `.62`. Homebrew keeps `ffmpeg@8` as a separate formula, so
anything pinned to the older ABI keeps working.

### 3. Quarantine and App Translocation

With the libraries present the app launched, then died about seven seconds later with no crash
report and no window:

```
16:04:03  suyu starts, running-active-NotVisible
16:04:10  kernel (AppleSystemPolicy) ASP: Security policy would not allow process: 5977
16:04:10  termination reported by launchd (9, 8, 9)      <- SIGKILL
```

`AppleSystemPolicy` runs the notarization check *asynchronously after launch* and kills the
process when it fails. Because the launch itself was permitted, Settings never records a blocked
launch, so again there is no Open Anyway button. App Translocation was also in play: a quarantined
app that has not been moved by Finder runs from a randomized read-only mount, so nothing persists.

- **Drag the app to `/Applications` in Finder**, not with `mv`. Translocation only clears when
  Finder performs the move.
- Then `xattr -d com.apple.quarantine /Applications/suyu.app`.

### 4. Missing Vulkan loader

suyu's own log, the single most useful file in this whole investigation:

```
~/.local/share/suyu/log/suyu_log.txt
```

```
Render.Vulkan <Error> vulkan_instance.cpp:31:AreExtensionsSupported:
    Required instance extension VK_KHR_portability_enumeration is not available
```

`VK_KHR_portability_enumeration` comes from the **Khronos Vulkan loader**, not MoltenVK. The
binary looks for `Contents/Frameworks/libvulkan.1.dylib` and the release did not ship it.

```bash
brew install vulkan-loader molten-vk
mkdir -p ~/.config/vulkan/icd.d
cat > ~/.config/vulkan/icd.d/MoltenVK_icd.json <<'EOF'
{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib",
        "api_version" : "1.4.0",
        "is_portability_driver" : true
    }
}
EOF
cp /opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.dylib <bundle>/Contents/Frameworks/
```

Homebrew's own ICD manifest uses a relative `library_path`, so copy the content, not the file.

### 5. The release was cut from an incomplete tree

With all of the above corrected, the prebuilt binary still segfaulted during main-window
construction. `strings` on it contains `suyu-cmd-static failed to link:`, and its log says
`LoadCompatibilityList: Unable to open game compatibility list` - that list is a git submodule
compiled in as a Qt resource, so the submodule was not checked out when the release was built.
In hindsight it was almost certainly also dying of the heap corruption in Part 4, which exists in
every build. Either way there is nothing to recover.

## Part 2 - building from source

Source lives in the release repo itself, `github.com/suyu-emu/suyu-v0.0.4`. The docs in the tree
are inherited from Eden (a fork of a fork) and say "Eden" throughout. `docs/Caveats.md` says
"macOS is largely untested. Expect crashes, significant Vulkan issues, and other fun stuff." It is
right.

```bash
git clone --recurse-submodules --shallow-submodules --depth 1 -j8 \
  https://github.com/suyu-emu/suyu-v0.0.4.git ~/GitHub/suyu

brew install qtbase qtcharts qtsvg boost ffmpeg sdl3 vulkan-loader molten-vk \
             cmake ninja ccache autoconf automake libtool glslang

export PATH=/opt/homebrew/opt/libtool/libexec/gnubin:/opt/homebrew/bin:$PATH   # libtoolize

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DYUZU_TESTS=OFF \
  -DVulkanHeaders_FORCE_BUNDLED=ON \
  -DVulkanUtilityLibraries_FORCE_BUNDLED=ON \
  -DUSE_SYSTEM_MOLTENVK=ON \
  -DENABLE_LIBUSB=OFF \
  -DYUZU_USE_QT_MULTIMEDIA=OFF \
  -DYUZU_USE_QT_WEB_ENGINE=OFF \
  -DENABLE_QT_TRANSLATION=OFF \
  -DQt6Charts_DIR=/opt/homebrew/opt/qtcharts/lib/cmake/Qt6Charts \
  -DQt6Svg_DIR=/opt/homebrew/opt/qtsvg/lib/cmake/Qt6Svg \
  -DCMAKE_PREFIX_PATH="/opt/homebrew/opt/qtbase;/opt/homebrew/opt/qtcharts;/opt/homebrew/opt/qtsvg;/opt/homebrew"

cmake --build build            # ~2.5 min on an M4 Pro, 1683 targets
```

Then copy `libvulkan.1.dylib` into `build/bin/suyu.app/Contents/Frameworks/`.

Why each non-obvious flag: `VulkanHeaders_FORCE_BUNDLED` avoids a system-headers mismatch that
`vulkan-loader` drags in, and `VulkanUtilityLibraries_FORCE_BUNDLED` must go with it or CPM stops
with "partial dependency installation detected"; `USE_SYSTEM_MOLTENVK` skips a call to an undefined CMake macro
(`download_moltenvk_external`, defined in a module that is never included) and gets MoltenVK 1.4.2
instead of the hardcoded 1.2.8; `ENABLE_LIBUSB=OFF` because the bundled libusb dereferences a
null function pointer in `usbi_create_event` on macOS 26 (costs GameCube-adapter support only).
The Qt components needed are `Core Widgets Charts Concurrent Gui Network Svg`; Homebrew puts each
module in its own prefix, hence the explicit `_DIR`s. GNU `libtoolize` lives in `gnubin` because
Homebrew prefixes it `g` to avoid Apple's `/usr/bin/libtool`.

### Applying the patch and installing

There are two starting points.

**From this fork.** `github.com/kirbycope/suyu-v0.0.4`, branch `main`, already has every fix
applied in the tree (one commit on top of upstream). Clone it, install the Homebrew packages
listed above, run the same `cmake` configure, and build. Nothing to apply.

**From upstream, with the patch file.** `suyu-macos-fixes.patch` is a plain `git diff` against
commit `c0dd2ff` ("FINAL"), the tip of `suyu-emu/suyu-v0.0.4` at the time and the parent of the
fork's commit. It carries every fix in this document plus the inert diagnostics and was checked
to apply cleanly to a fresh checkout of that commit. The original repository is archived (still
cloneable, read-only); the fork contains the same commit, so either source works. Whoever
receives the file needs that commit checked out, the patch, and `git`:

```bash
git clone --recurse-submodules --shallow-submodules https://github.com/kirbycope/suyu-v0.0.4.git suyu
cd suyu
git checkout c0dd2ff                    # upstream tip, the commit the patch was made against
git apply --check /path/to/suyu-macos-fixes.patch   # dry run: prints nothing when it fits
git apply /path/to/suyu-macos-fixes.patch
```

`git apply` works on any checkout and needs no git identity configured. The file is a plain
diff, not a mail-format patch, so `git am` does not apply to it; to record the change as a
commit, run `git add -A && git commit --no-verify` afterwards (upstream ships a pre-commit hook
that rejects trailing whitespace already present in its own sources, hence `--no-verify`).

Then, either way:

```bash
# Build (same cmake configure as above, then):
export PATH=/opt/homebrew/bin:$PATH
cmake --build build

# The bundle needs the Vulkan loader beside MoltenVK:
cp /opt/homebrew/lib/libvulkan.1.dylib build/bin/suyu.app/Contents/Frameworks/

# The loader finds MoltenVK through an ICD manifest. This one lives in the user config dir, so
# no system paths are touched:
mkdir -p ~/.config/vulkan/icd.d
cat > ~/.config/vulkan/icd.d/MoltenVK_icd.json <<'JSON'
{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib",
        "api_version" : "1.4.0",
        "is_portability_driver" : true
    }
}
JSON

rm -rf /Applications/suyu.app && cp -R build/bin/suyu.app /Applications/suyu.app
open /Applications/suyu.app
```

Nothing in the patch needs an environment variable: the MoltenVK-specific behaviour (Fix 12, the
depth image types, the shader-side compare) keys on the Vulkan driver id at runtime, so the same
binary behaves as upstream on any other driver.

Toolchain note. The Command Line Tools' linker (`ld-1267`) cannot read the `.tbd` stubs of the
macOS 27.0 SDK that a newer Command Line Tools install drops in; linking then fails with
`tapi error: malformed file ... unknown architecture`. Either point the developer directory at
Xcode 27 (`sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`, then
`sudo xcodebuild -license accept`), or keep the tools and pin the SDK for the build:
`SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk cmake --build build`.

## Part 3 - the four fixes needed to open a game

### Fix 1 - the MoltenVK feature waiver never fires
`src/video_core/vulkan_common/vulkan_device.cpp`

suyu requires `geometryShader`, `logicOp`, `shaderCullDistance` and `wideLines`, which Metal
lacks, and already contains a waiver for MoltenVK. It cannot trigger: `IsMoltenVK()` reads
`properties.driver.driverID`, which `GetSuitability()` only populates *after* the feature check.
The patch computes the flag from the platform. BotW renders fine without those four features.

### Fixes 2 and 3 - an Objective-C exception escaping every C++ catch
`src/video_core/video_core.cpp`

`CreateGPU()` acquires a scope guard on the graphics context, moves ownership into the renderer,
and cancels the guard only in a `catch (const std::runtime_error&)`. The exception that actually
arrives is an **NSException**, which no C++ handler matches, so `Cancel()` never runs and
`~Scoped()` dereferences freed memory during unwinding. Every real error therefore presented as a
corrupted-pointer segfault. Adding `catch (...)` turned that into a readable
`Failed to initialize GPU`, which is what made Fix 4 findable.

### Fix 4 - the actual blocker: an NSView layer handed to MoltenVK as a CAMetalLayer
`src/suyu/qt_common.cpp`

Breaking on `__cxa_throw` in lldb:

```
frame #2  CoreFoundation`-[NSObject doesNotRecognizeSelector:]
frame #5  libMoltenVK.dylib`MVKSurface::getNaturalExtent()
frame #10 suyu`Vulkan::Swapchain::Create                       <vk_swapchain.cpp:156>
```

The Qt frontend took `[NSView layer]` and `vulkan_surface.cpp` cast it straight to
`CAMetalLayer*`. Under Qt 6 that layer is a `QContainerLayer`. **The correct code already existed**
in `src/qt_common/qt_common.cpp`, which walks the sublayers for a real `CAMetalLayer` and even
has a comment describing this bug - but `src/suyu/CMakeLists.txt` builds `src/suyu/qt_common.cpp`,
a second copy of the same function that never received the fix. The patch ports it across.

## Part 4 - the random crashes: heap corruption

After Part 3 the game played for 24 minutes and then, on later runs, died at 3 s, 40 s and 50 s
with the same signature on three unrelated threads:

```
EXC_BREAKPOINT  libsystem_malloc.dylib`_xzm_xzone_malloc_freelist_outlined
                <- operator new  <- (Dynarmic JIT | RomFS builder | Foundation XPC decode)
```

A trap *inside the allocator* means heap corruption detected after the fact; the thread that dies
is the victim, not the culprit. Two writers were found.

### Fix 5 - `aes_util.cpp` smashes the stack ~1,400 times per run
`src/core/crypto/aes_util.cpp`

The constructor deliberately leaves a cipher uninitialised when its key is all zeros (key file
absent), on the theory that it is never used. It is used. In `Transcode()`, `EVP_CipherInit_ex`
then fails, `EVP_CIPHER_CTX_get_block_size` returns 0, and because AArch64 integer division by
zero yields 0 without trapping, `size % 0` evaluates to `size`, `whole_block_bytes` becomes 0,
`tail` becomes `size`, and

```cpp
std::array<u8, 16> tail_buffer{};
std::memcpy(tail_buffer.data(), src + whole_block_bytes, tail);   // tail == size
```

copies the entire input into a 16-byte stack array. The `ASSERT`s in that function log and
continue; each run logged about 1,400 of them and every one was a stack smash. The patch returns
early with zeroed output when the context has no cipher or the block size is invalid. The log
spam is gone with it.

**This had been dismissed as harmless noise. It was not.** An assert that fires 1,400 times and
"seems fine" is a memory-safety bug until proven otherwise.

### Fix 6 - two different classes named `PlayTime::PlayTimeManager` (ODR violation)
`src/suyu/` - eleven files

This is the writer that Guard Malloc caught red-handed:

```
thread 'CPUCore_3'  SIGSEGV
  0  PlayTime::PlayTimeManager::PlayTimeManager() + 24
  1  Service::NS::IQueryService::IQueryService(Core::System&)
  2  allocate_shared<Service::NS::IQueryService>
```

A guard-page fault at `+24` *inside a constructor* means the object is being written past the end
of its own allocation. Two headers define the class:

| | members |
| --- | --- |
| `src/frontend_common/play_time_manager.h` | `database`, `running_program_id`, `play_time_thread` |
| `src/suyu/play_time_manager.h` | the same three **plus `ProfileManager* manager`** - 8 bytes bigger |

Core's `IQueryService` includes the `frontend_common` header and `make_unique`s an object sized by
that layout. But both `.cpp` files are linked into one executable with identical mangled symbols,
so the linker silently keeps one constructor for every caller - and when it keeps the GUI's, it
initialises a member that lies past the end of core's smaller allocation. The overflow lands on
whatever the heap put next, which is why the victim and the timing changed from run to run, and
why it *looked* correlated with an unrelated graphics setting: changing any setting perturbs the
heap layout.

The fix renames the GUI copy's namespace to `SuyuPlayTime` (22 lines across 11 files). `src/yuzu`
also declares the class but is not built.

Verification: before the rename, the game died under Guard Malloc in about 30 s with the writer
caught at `PlayTimeManager::PlayTimeManager() + 24`. After the rename, two 100 s Guard Malloc runs
survived and were killed by hand, banner confirmed both times. Every one of those runs was at
default settings - the "config=2 reproducer" narrative that existed for a while was wrong, see the
`\default` note in Debugging notes - which makes the comparison cleaner, not weaker. The heap fix
holds, and the apparent correlation between crashes and a graphics setting was heap layout luck
across identical configurations.

### How the writer was found, and two traps on the way

Chasing crash reports was useless - three reports, three victims. The tool that answered the
question in one run was **Guard Malloc**, which puts every allocation against a guard page so the
first out-of-bounds write faults at the culprit's own instruction:

```bash
DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib MALLOC_FILL_SPACE=1 /path/to/suyu game.xci
```

Trap one: **do not wrap the command in `caffeinate`, `env`, or any other Apple binary.** SIP purges
every `DYLD_*` variable from a protected process's environment at load, and the child inherits the
purged environment. The first attempt ran with the normal allocator and nobody noticed until the
missing `GuardMalloc[...]` banner was checked for. Set the variable directly on the target.

Trap two: ReportCrash rate-limits repeated crashes of the same binary, so after a few in quick
succession no `.ips` appears at all. Run under lldb with crash hooks instead, which do not depend
on ReportCrash:

```bash
lldb -b -o "settings set target.env-vars DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib" \
     -o run -k "thread backtrace" -k "quit" -- /path/to/suyu game.xci
```

An AddressSanitizer build (`-DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -O1"`
plus the same linker flag, in a separate build directory) is the second opinion for anything Guard
Malloc's 16-byte granularity misses. `src/common/fiber.cpp` carries no sanitizer annotations, so
expect noise around fiber switches.

## Part 5 - the block-shaped texture corruption (solved)

Symptom: uniform, grid-aligned square blocks of displaced texture content on grass, terrain and
character skin, worst on high-detail surfaces and mid-distance ground, absent on glow effects and
UI. **It is texture-space, not screen-space** - a block on Link's hair moves with Link - so it is
wrong texture data, not a render-pass or tile-memory problem. It varies over time on the same
texture, which points at something mip- or streaming-dependent.

What has been established, in order, because several plausible theories were wrong:

- **It is not the GPU ASTC compute decoder.** That decoder only runs when
  `IsOptimalAstcSupported()` is false. The check looks strict - it asks every ASTC format for
  `SAMPLED | BLIT_SRC | BLIT_DST | TRANSFER_SRC | TRANSFER_DST` - but it tests
  `(features & wanted) == 0`, i.e. *any* bit present passes. A direct Vulkan probe of MoltenVK
  shows ASTC formats with `sampled=1 blit_src=1 blit_dst=0 xfer=1`, so the check passes, native
  hardware ASTC is in use, and the `accelerate_astc` setting is inert. Do not spend time on it.
- **It is not MoltenVK's `bufferRowLength` conversion.** Zeroing `bufferRowLength` and
  `bufferImageHeight` on compressed uploads (byte-for-byte identical layout per the Vulkan spec for
  the tiled path) made no difference. That patch was reverted.
- **It is not caused by the heap corruption.** The blocks were present before and after Fix 6.
- The `Unimplemented format=0` asserts from `video_core/surface.cpp` are noise:
  `RenderTargetFormat::NONE == 0`, they are queries on disabled render-target slots.

- **Format changes the density, not the presence.** With the tree's diagnostic switch
  `SUYU_FORCE_ASTC_DECODE=1` (makes `IsOptimalAstcSupported()` return false on Apple) and
  `accelerate_astc=0`, ASTC is decoded on the CPU and uploaded as RGBA8 through the uncompressed
  copy path. Same spot, same view: the native-ASTC frame had dense blocks across grass, foliage
  and rock; the RGBA8 frame had a handful, in one patch of grass and along one rock edge. An
  order-of-magnitude drop from changing only the texture format.

Two readings survive that result. A **CPU/GPU timing race** - a staging buffer reused or a copy
recorded before its data is final - explains it cleanly: CPU decoding makes uploads slower and
later, which narrows a race window without closing it, and Apple's unified memory plus MoltenVK's
semaphore emulation would expose such a race where a discrete PC GPU hides it. Alternatively BotW
also ships **BC-compressed** textures, which the switch does not touch, so the residue could be
those still taking a broken compressed path.

### The knob experiments, and what they ruled in and out

| run | change | result |
| --- | --- | --- |
| baseline | native ASTC, defaults | dense blocks on grass, foliage, rock, character |
| test A | `SUYU_FORCE_ASTC_DECODE=1`: ASTC decoded to RGBA8 by suyu's GPU compute decoder (the CPU mode setting never applied, see the `\default` note) | sparse blocks - an order of magnitude fewer, same spot and view as baseline |
| E1 / E3 | `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=1` | sparse blocks, near grass only; **26 FPS, no measurable cost** |
| E2 | suyu's fence-based sync instead of timeline semaphores | **worse** - dense everywhere; reverted |
| E3b | intended `use_asynchronous_gpu_emulation=false` - **never applied** (`\default` flag), so this was a plain baseline run | sparse at that spot |
| E4 | `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` | unchanged - roughly baseline density |
| E5 | `MVK_CONFIG_USE_MTLHEAP=0` + argument buffers forced on | crashed 15 s in: `std::terminate` out of `TextureCache::JoinImages`; not pursued |
| E6 | Fix 7 (barriers widened to `ALL_COMMANDS`), no knobs | **unchanged** - the barrier was wrong but is not the cause |
| E7 | render-pass event mask hack | **invalid** - the automated menu presses never left the title screen; no frame was captured, and the hack was reverted |
| E8 | Fix 8 (`MarkUsage` after reorderable uploads) | **unchanged** - correct, kept, not the cause |
| E9 | `use_asynchronous_gpu_emulation=false`, this time verified in the log dump | **unchanged** - the GPU thread is not the cause |
| E10 | `cpu_accuracy=Debugging` + `cpuopt_fastmem=false`, verified in the log dump | **unchanged** - guest writes tracked by the slow path, squares identical |
| E11 | baseline frame with Fixes 9 and 10, saved as `before-feedback-loop-fix.png` | squares dense; the grid is uniform in **screen** space, the same size at the horizon as at Link's feet, and one tile sits on Link's leg showing grass |
| E12 | `CheckFeedbackLoop` restored to upstream yuzu's rule (any sampled view of a bound colour or depth image ends the render pass) | **unchanged**; the split fires about 1,000 times a second in BotW, so it is exercised |
| E13a | `SUYU_SPLIT_EVERY_DRAW=1`: every draw in its own render pass | **unchanged** and not markedly worse, so pass boundaries (load/store through memory) are not the mechanism |
| E13b | `SUYU_WAIT_IDLE_EVERY_SUBMIT=1`: `vkDeviceWaitIdle` after every queue submit | **unchanged** - with the CPU fully serialised against the GPU, no race of any kind is left; the GPU is drawing wrong content deterministically |
| E14 | `MVK_CONFIG_FAST_MATH_ENABLED=0` | **unchanged** - the per-tile pass is not a fast-math casualty |
| E15 / E16 | `MVK_CONFIG_SHADER_DUMP_DIR`, then again with `~/.cache/suyu/shader` moved aside so every shader recompiles and is dumped | the MSL shows which kernels do what: eight compute kernels and seven fragment shaders call `sample_compare` |
| E17 | `VK_LAYER_KHRONOS_validation`, core + best practices, `MVK_CONFIG_LOG_LEVEL=3` | **the lead**: `VUID-vkCmdDispatch-None-06479` - a compute shader samples an `R32_SFLOAT` view through a comparison sampler, and a vertex shader does the same on `R16_UNORM`; MoltenVK reports neither format as supporting depth comparison. Also: blending enabled on an `R16G16B16A16_UINT` attachment (`VK_ERROR_FEATURE_NOT_PRESENT` from MoltenVK), `VK_KHR_portability_subset` never enabled, `primitiveRestartEnable` forced on for list topologies, required subgroup size requested for a stage MoltenVK does not allow it on |
| E18 | `SUYU_KEEP_COMPARE=1`: keep the comparison sampler bound on the `R32_SFLOAT` shadow map instead of swapping in a non-comparison one | **unchanged** - Metal's sampler compare gives garbage on a colour format even when asked properly, so the comparison has to move into the shader |
| E19 | depth compare emulated in the shader (Fix 11), compare bits indexed by the instruction's array-element index by mistake | **lighting collapsed**: the whole field went dark and the checkerboard became dense. Wrong, but decisive: the compare result drives the per-tile lighting term |
| E20 | same, compare bits indexed by texture descriptor | lighting back to normal, **tiles unchanged**: the compare feeds the term but is not what corrupts it |
| E21 | counter on depth-format images bound as plain (non-compare) textures | about 1,000 bindings a second of `D32_FLOAT` and `D16_UNORM` views read through `texture2d<float>` |
| E22 | `SUYU_SKIP_COMPUTE=1`: every guest compute dispatch dropped | fog and lighting unchanged, **tiles unchanged**: the per-tile term is computed in a fragment pass, not in the compute kernels |
| E23 | image types follow the bound texture's format on MoltenVK (depth formats declared `depth2d`, native compare on them; colour formats compared in the shader) | **unchanged** |
| E24 | one log line per unique render target set | 669 sets; the scene is `R1 1600x900` (colour + depth) followed by colour-only `C1 1600x900` post passes, half-res `800x450` sets, `Z 896x896` shadow depth maps, and no small per-tile target anywhere |
| E25 | live draw bisection: a file-driven filter skips a range of each frame's ~3,800 guest draws while the game keeps running | skipping the first half removed Link from the scene, **and his silhouette stayed behind inside the bad tiles**. The tiles are stale pixels from earlier frames, not a lighting term; with a static camera only sun, fog and grass sway had changed, which is why they read as lighting differences |
| E26 | live switches: full-image `vkCmdClearDepthStencilImage` at every guest depth clear; clears in their own render pass; stencil test forced off | none removes the tiles; the latter two visibly thin them |
| E27 | every guest colour clear also clears its whole image to magenta | the entire scene below the sky turns magenta (a cleared intermediate feeds every material, the exposure chain), and inside it the stale tiles stand out as non-magenta blocks: texels nothing rewrote this frame. Texture-cache image creation is not churning (20 large images in total) |
| E28 | magenta on one colour clear at a time (24 per frame: 1920x1080, two 1600x900, the 800x450 set, the 200x112 / 100x56 / 1x1 exposure chain, UI targets) | only clear 2, a 1600x900 buffer, tints the terrain, and inside the tint the stale tiles still show **old, valid** values. A whole-image `vkCmdClearColorImage` cannot miss tiles, so the sampler reads a different copy of that buffer than the one being cleared and drawn |
| E29 | `MVK_CONFIG_USE_MTLHEAP=0` with the heap-only image flag stripped | not viable: BotW renders into slices of 3D textures and views 3D textures as 2D arrays, both of which MoltenVK can only do with heaps (Metal assertion in `newTextureViewWithPixelFormat`). Also moot: the mask buffer is created once and lives, so one-time heap aliasing cannot produce tiles that change over time |
| E30 | detector: every sampled image of at least 1600x900 checked against the set of images that have been render targets; image-to-image copies on large images counted | **no alias**: zero large copies, and every sampled 1600x900 image is a render target (the only non-render-target hits are large regular textures). The shader samples the very `VkImage` that is cleared and drawn |
| E31 | the clear-2 buffer (an `A8B8G8R8_UNORM` 1600x900 mask) copied to the host before and after the magenta clear | memory is right: after the clear, 100 percent magenta; one frame later, before the next clear, no magenta left, a mostly zero mask with a few painted marks, and **no tile pattern in memory**. The sampler nevertheless saw the pre-clear state over most of the screen (E28). The writes land; the *reads* in the terrain pass see the buffer as it was before the pass that wrote it, except in some tiles |
| E32 to E34 | live switch: submit a new command buffer after every render pass (`pass_sync 1`), or also `vkDeviceWaitIdle` after every render pass (`pass_sync 2`); two placements crashed (flushing inside `EndRenderPass`, and between index-buffer binding and the draw) before it was moved to the draw and clear entry points | **unchanged even with the GPU idle between every render pass**: GPU ordering within a command buffer is not the mechanism. The stale tiles turned lighter under the switches, so their *content* is timing-dependent while their existence is not |
| E35 | display route counter; the final 1920x1080 `A2B10G10R10` buffer (clear 0) dumped from memory | the display is always fed from the texture-cache image, never from guest RAM; and **the tiles are already in the memory of the final buffer**, so they exist before presentation |
| E36 / E37 | `dump_draw N`: the colour target copied to disk just before draw N of the frame (first placement crashed the same way as the pass flush; moved before `Configure`) | the 1600x900 `B10G11R11_FLOAT` HDR scene buffer already shows the tiles mid-frame; the 1600x900 `A8B8G8R8` target that draws 200 to 2500 render into is a near-empty auxiliary buffer. Draw indices drift between frames (3,777 to 4,264 draws), so a per-format draw counter was added |
| E38 / E39 | dump keyed to the k-th draw into a target of a given format and width | the 1600x900 HDR buffer is **black (cleared) at its first and third draw of the frame and carries the complete, tiled scene by its tenth**: the scene is not drawn into it primitive by primitive. The census confirms why: BotW's scene sets are `R5`, `R6` and `R7` at 1600x900, a G-buffer. Draws 200 to 2500 fill the G-buffer, a few full-screen draws light it into the HDR buffer, and the tiles arrive with that lighting pass |
| E40 | the G-buffer targets dumped near the end of the G-buffer pass (draw 2000) | the normals target **already has the tile holes in memory**: squares of the previous frame's normals in the rock and background area while the near grass is continuous |
| E41 | the scene depth dumped at the first and a late G-buffer draw | depth is cleared to 1.0 across the whole buffer at the first draw and is **perfectly clean at draw 1800**: terrain, grass and Link all present, no tiles. The fragments in the hole tiles are rasterised and write depth; only their colour outputs fail to land |
| E42 | `Invariant` on the vertex position output (the recompiler never emitted it), in case a depth pre-pass and the G-buffer pass disagreed under an EQUAL depth test | **unchanged** |
| E43 | `SUYU_DEDICATED_IMAGES=1`: every image in its own `VkDeviceMemory`, so no two images share a MoltenVK heap | **unchanged** |
| E44 | the normals target dumped at the **first** draw of the G-buffer pass | the previous frame's normals with its own holes, **plus scattered pure-black 32x32 squares**: something zeroes tiles of a live render target between frames. The game never clears this target itself (only two of the 24 colour clears are 1600x900, the HDR buffer and one RGBA8 target); the holes are therefore not unwritten pixels but content written *into* the image by another path |
| E45 | counters on texture-cache re-uploads from guest memory and downloads to it, for render-target sized images | one re-upload at startup (the 1080p buffer), zero downloads: the texture cache is not writing old data into these targets |
| E46 | memory dumped right after the game's clear 2 with the pass forced to end; then the same clear done with `vkCmdClearColorImage` outside the pass (`clear_via_image`) | the attachment clear is complete in memory when stored immediately (all zeros); doing the clear outside the pass **does not remove the tiles**. Whatever loses tile data happens after the clear, inside the long G-buffer pass |
| E47 | `no_queries`: the ZPass pixel counter (occlusion query) never enabled, removing MoltenVK's per-draw `MTLFence` waits and updates inside the render encoder | **unchanged** |
| E47b | G-buffer draw state logged; albedo, normals and a third attachment dumped at the same draw | the G-buffer draws use depth test LEQUAL with depth writes on (no EQUAL pre-pass), six colour targets; the **holes coincide across attachments**: the whole tile's fragment output is missing, not one attachment's store |
| E48 | per-frame largest draws logged; `skip_big 15000` skipped the 132 largest draws of each frame | the largest draw in the frame is only 26,685 vertices x instances (BotW draws its grass in many small draws), and skipping the big ones leaves the tiles **unchanged**. Together with E13a (one draw per pass), parameter-buffer overflow is ruled out |
| E49 | Metal's own API validation (`MTL_DEBUG_LAYER=1`, log mode) | **`RenderPass Descriptor Validation: Attachment 1 overlaps with attachment 2` and `Attachment 3 overlaps with attachment 4`**, 318,000 times each in a few minutes: BotW binds one surface at two G-buffer slots twice over (the pairs with formats 214/214 and 213/213, whose dumps were byte-identical). Desktop Vulkan drivers tolerate it; Metal declares it undefined, and on a tile-based GPU the aliased attachments overwrite each other's tiles when the pass is stored. Also: uniform buffer bindings that run past the end of their buffer (`has space for N bytes, but argument has a length(65536)`), noted for later |
| E50 | **Fix 12**: on MoltenVK a colour target whose address equals an earlier bound target's is dropped from the pipeline key, the framebuffer and clears | **the squares are gone** |

E6 is the important negative. The barrier fix stays in because it is correct, but the artifact is
not a copy-to-sample visibility gap. What finally identified its nature was a nearest-neighbour
zoom into a corrupted patch of grass: the bad tiles form a screen-aligned checkerboard, each bad
tile shows the bare ground **with the grass layer missing** and the ground itself correctly
positioned, and a few tiles are black. Nothing is displaced; those tiles are missing *later*
geometry. That is a render target whose tiles come back wrong after a render pass is split and
resumed - most plausibly the **depth buffer**, since a bad depth tile makes later draws fail the
depth test there (no grass) or rejects everything (black). It is also why an early observation
misled two people: the "grass-green block on Link's hair" was not a displaced hair texel, it was a
tile that had not been redrawn since the frame showed grass at that spot. Texture-format, upload
and synchronisation theories all followed from that misreading.

The square size settles the granularity. BotW runs docked at 1600x900, the window shows it at
1.44 device pixels per game pixel, and the dark tiles measure 36 to 42 device pixels with grass
blades eating their edges: 46 device pixels, **32x32 game pixels, the Apple GPU tile size**.
MoltenVK allocates its heaps with `MTLHazardTrackingModeTracked` (`MVKDeviceMemory.mm:226` at
v1.4.2), so a GPU-to-GPU race on those images is something Metal itself prevents; what Metal does
not track is memory the CPU writes while the GPU still reads it.

A native-resolution look at the bad tiles (E13b) settles what they contain: the geometry is all
there. A dark grass tile has every blade, rendered dark, with only the brightest tips showing
through; the tile crossing Link's thigh shows the thigh, lit brighter, and the grass beside it
brighter by the same amount. Each bad tile is the correct scene with one wrong lighting or fog
term applied uniformly across the 32x32 block. That is the signature of a pass that computes
something per screen tile (tiled light culling, per-tile fog or exposure) producing a wrong value
for some tiles.

E9 and E10 close two more branches. With the GPU thread off, the CPU and GPU emulation run in
lock-step, so a race between them is ruled out. With fastmem off and the interpreter-accurate
CPU, every guest memory write goes through the slow, tracked path, so a missed invalidation of a
CPU-streamed terrain tile is ruled out as well: if the texture cache were serving stale guest
memory, E10 is the configuration that would have fixed it. Both runs were verified from the
settings dump at the top of `suyu_log.txt`, unlike E3b.

### The cause, and Fix 12

BotW's scene is a deferred G-buffer pass: about 2,000 draws into six colour targets plus depth at
1600x900. The game binds the same surface at two of those slots, twice over (RT1 and RT2, RT3 and
RT4; the pairs with matching formats whose memory dumps were byte-identical). Vulkan forbids
aliasing attachments within a render pass, but every desktop driver writes attachments pixel by
pixel in draw order, so the alias is harmless there and upstream yuzu never had to care. Metal
declares it undefined, and its API validation layer says so on every pass:
`RenderPass Descriptor Validation: Attachment 1 overlaps with attachment 2`. On a tile-based GPU
each attachment's tile lives in tile memory for the whole pass and is written out separately when
the pass ends, so the two aliased attachments store over each other, tile by tile, in whatever
order the hardware picks. A tile whose last store came from the *unwritten* alias keeps the
previous frame's contents (the ghost of Link, the "old lighting"), and a tile whose alias was
never loaded stores zeros (the black squares of E44). Depth is unaffected because it has one
attachment.

That is why nothing else moved it: it is not a race (E13b, E34), not the clear (E46), not the
texture cache (E30, E45), not heap sharing (E43), not a heavy draw (E48), and the memory copies
were always right because the *final* store of a tile is a real store. It also explains why the
early MoltenVK submit knob and the stencil and clear splits only thinned it: they change which
alias stores last.

`src/video_core/gpu_workarounds.h` (new), `renderer_vulkan/fixed_pipeline_state.cpp`,
`texture_cache/texture_cache.h` (`FindColorBuffer`), `renderer_vulkan/vk_rasterizer.cpp`

On MoltenVK, a colour target whose address equals an earlier bound target's is treated as
`RenderTargetFormat::NONE` when the pipeline key is built, is given no image view when the
framebuffer is built, and a clear issued through the dropped slot is redirected to the first slot
with that address. The three agree, so the render pass, the framebuffer and the pipeline all see
one attachment; the fragment shader's output for the dropped slot is simply not consumed. It is
enabled from the rasterizer constructor when the device is MoltenVK and is a no-op elsewhere.

Metal validation also reported uniform buffer bindings that extend past the end of their buffer
(`argument has a length(65536)` where fewer bytes remain). That is a separate defect, not fixed
here.

suyu uses classic render passes (no dynamic-rendering suspend/resume), every attachment is
`LOAD`/`STORE` with `GENERAL` layouts, and it ends and restarts the render pass around every
texture upload.

### What the validation layer and a submit trace established

`brew install vulkan-validationlayers`, `debug\default=false` + `debug=true` in `[Renderer]`, and
a layer settings file with `khronos_validation.validate_sync = true`. suyu routes the layer's
messages into its own log and the layer also writes `/tmp/validation_layer.log`.

The layer reported, among others, `vkBeginCommandBuffer` on a pending command buffer (00049), a
descriptor set updated while in use (03047), a command buffer submitted while pending (03875) and
timeline signal values below the semaphore's current value (03882, "signal 3, current 21"). Those
four would explain everything - and they are **false positives**. An instrumented build logged,
at every submit, the host tick, the raw driver counter and the thread: one submitting thread,
ticks strictly increasing, and the driver counter exactly `host_tick - 1` every time. Nothing is
ever pending when the next submission goes out. The fork's own comment in `MasterSemaphore` says
the layer mis-tracks resource usage with timeline semaphores; that is what this is. The
`03882` offset of exactly 18 is the layer's model, not the driver. Two hours went into chasing
those four messages - a counter-clamping guard, a fence-based sync path that made things worse
and was reverted - before the trace settled it. **Trace first, theorise second.**

Also from that trace: since the GPU is idle at every submission boundary, the artifact cannot be
a race *between* submissions. It has to be ordering *inside* one.

The messages that are real are the synchronization hazards, which the layer derives from the
command stream and which do not depend on timeline tracking:

- `READ-AFTER-WRITE` at `vkCmdBeginRenderPass` (attachment `LOAD` after a layout transition) and
  `WRITE-AFTER-WRITE` at `vkCmdClearAttachments` - eight in total, unchanged after widening the
  `blit_image.cpp` barrier that looked responsible. Their source is still unidentified.
- **`WRITE-AFTER-WRITE` at `vkCmdCopyBuffer`: a copy into a buffer range previously written by
  another `vkCmdCopyBuffer`, no barrier between them.** This one has a cause.

### Fix 8 - uploads never mark their destination as used
`src/video_core/buffer_cache/buffer_cache.h`, both `runtime.CopyBuffer(..., can_reorder)` sites

The buffer cache has a fast path: when a staging upload's destination range has not been used
this tick (`CanReorderUpload` → `!buffer.IsRegionUsed(...)`), the copy is moved into the
*upload* command buffer, which executes before the main one, with no barrier. The idea is sound
for a range nobody else touches. But after the copy, **the destination range is never marked as
used** - `MarkUsage` is called for buffer-to-buffer copies and downloads, not for uploads. So a
second upload into the same range in the same tick also passes the gate, and both copies sit in
the upload command buffer back to back: two transfer writes to one range with no ordering
between them. Whichever finishes last wins, and on Apple hardware that is frequently the older
one. Per-tile terrain and grass data streamed every frame is precisely the kind of range that
gets written more than once per tick - and a checkerboard of tiles with stale data is precisely
what the screen showed, complete with perspective (the near tiles are larger), which is what
finally distinguished world-space terrain tiles from screen-space GPU tiles.

The fix marks each copied destination range used after the upload so the second write takes the
ordered, barrier-protected path. A setting, `disable_buffer_reorder`, turns the fast path off
entirely and is the fallback if anything else turns up in this area. The result of the run that
tests this fix is recorded below.

**A caveat that invalidates most of the table.** Only two comparisons in it were taken at the
same spot with the same view: test A against baseline (a real, large effect) and E6 against E3b
(no effect). The dense baseline frame was captured at the forest edge right after loading, while
E1, E3, E4 and E6 were captured on an open hillside after the player had moved. Density varies
with scene and streaming state at least as much as with any knob, so "synchronous submits helped"
- a conclusion this document carried for a while - is withdrawn: it compared two different places.
The knob rows are kept as a record of what was tried, not as evidence. The blocks are uniform ~32-48 px on every
surface, which is the size of a tiled GPU's internal texture tile: sampling a texture before a
blit's writes to it are visible shows exactly tile-sized regions of old content.

The MoltenVK knobs are read from the environment. The shipped README documents only two, but
`strings libMoltenVK.dylib | grep -o 'MVK_CONFIG_[A-Z_]*'` lists all 44.

### Fix 7 - the upload barrier omits the shader stages
`src/video_core/renderer_vulkan/vk_texture_cache.cpp`, `CopyBufferToImage` and the image copy

Both barriers around the upload copy name only

```
VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
```

as the source stages before the copy and the destination stages after it. **`FRAGMENT_SHADER` and
`VERTEX_SHADER` - the stages that actually sample textures - are missing.** Under the Vulkan
memory model a barrier makes writes visible only to the stages it names. Desktop drivers tend to
implement image barriers as full pipeline drains and hide the omission; Metal honours the scope,
so a draw in the same submission can sample the texture before the copy's writes are visible.
The whole file contains no `FRAGMENT_SHADER_BIT` and no `ALL_COMMANDS_BIT`; upstream yuzu used
`ALL_COMMANDS` here and this fork narrowed it. The patch restores `ALL_COMMANDS` on both barriers
and on the image-to-image copy site that used the same trio. Widening a barrier can only
over-synchronise, so the change is safe by construction; whether it is the *whole* cause is
answered by the run recorded below.

## Part 6 - two more crashes found while iterating

### Fix 9 - an unknown system archive aborts the emulator
`src/core/file_sys/system_archive/system_archive.cpp`

`GetSystemArchive(title_id)` is a `switch` whose `default` returns `{nullptr, nullptr}`, and the
next line is `LOG_INFO(..., "Synthesizing system archive '{}' ...", desc.name, title_id)`. fmt
throws `format_error("string pointer is null")` for a null `const char*`, the exception escapes
the FS service's `std::thread`, and `std::terminate` aborts the process: `SIGABRT` on thread
`FS`, `FmtLogMessageImpl` <- `SynthesizeSystemArchive` <- `OpenDataStorageByDataId`. Upstream
yuzu bounds-checked an array before logging; this fork's rewrite lost the guard. The patch logs
the unknown title id as a warning and returns `nullptr`, which the caller already turns into
`ResultTargetNotFound` for the guest. BotW asked for such an archive about 50 s in.

### Fix 10 - the game list rescans on every save write
`src/suyu/game_list.{h,cpp}`

`QFileSystemWatcher::directoryChanged` was wired straight to `RefreshGameDirectory`, and the
watched directories include the emulated SDMC and both NANDs, which a running game writes into
constantly. On macOS that fired **2,203 times in 48 seconds**, each time launching a worker that
rescans every game directory and re-reads the XCI the game is running from. This is the "cover
art flashing" seen on the first launch, and a steady I/O load on top of the emulator. The patch
routes the signal through a 2 s single-shot `QTimer`, so a burst becomes one reload, and logs the
changed path at `Debug` so the next log names the offender.

## Everything installed on this machine

```
qtbase 6.11.2      qtcharts 6.11.2    qtsvg 6.11.2       boost
ffmpeg 8.1.1 -> 9.0.2                 vulkan-loader 1.4.357.0
molten-vk 1.4.2    glslang 16.6.0     cmake 4.4.3        ninja 1.13.2
ccache 4.14        autoconf 2.73      automake 1.19      libtool
```

Plus `~/.config/vulkan/icd.d/MoltenVK_icd.json`, written by hand as above.

## Layout

| | |
| --- | --- |
| Games | `~/Games/Switch/`, `~/Games/PSX/` |
| Keys | `~/.local/share/suyu/keys/prod.keys` |
| Firmware 22.5.0 | `~/.local/share/suyu/nand/system/Contents/registered/` (229 files, 322 MB) |
| Config | `~/.config/suyu/qt-config.ini` - note this build reads `Paths\romsPath`, not `Paths\gamedirs` |
| Log | `~/.local/share/suyu/log/suyu_log.txt` |
| Source, build | `~/GitHub/suyu-v0.0.4` (the fork clone), Release build in `build/` |

**Do not put ROMs in `~/Documents` on this Mac.** It is an iCloud Drive folder; iCloud
continuously touches files there, suyu rescans its game directory on every change
(`RefreshGameDirectory` every ~20 ms, visible as flickering cover art), and iCloud will try to
upload a 16 GB ROM. `~/Games` is outside sync.

## Debugging notes

### The live switch file

The build keeps the instrumentation that found the cause, all inert unless
`SUYU_DRAW_SKIP_FILE=<path>` is set at launch. The file is re-read every 30 frames, so every
switch can be flipped while the game runs. Line one is `from to`: guest draws whose per-frame
index is in that range are skipped (`4294967295 0` skips nothing). Every following line is
`key value`; keys are sticky, so reset them explicitly:

| key | effect |
| --- | --- |
| `depth_clear_image 1` | full `vkCmdClearDepthStencilImage` at every guest depth clear |
| `color_clear_image 1` | every guest colour clear also clears its whole image to magenta; `clear_index k` limits it to the k-th colour clear of the frame |
| `clear_via_image 1` | full colour clears done with `vkCmdClearColorImage` outside the pass |
| `split_clears 1`, `no_stencil 1`, `no_queries 1` | clears in their own pass; stencil test off; occlusion counter off |
| `pass_sync 1` / `2` | new command buffer after every render pass / plus `vkDeviceWaitIdle` |
| `skip_big N` | skip draws with more than N vertices x instances |
| `dump_clear k` | copy the k-th colour clear's target to `/tmp/rtdump_*.raw` before and after the clear |
| `dump_draw k` (+ `dump_rt n`, `dump_fmt f`, `dump_w w`, `dump_depth 1`) | copy colour target n (or the depth aspect) to `/tmp` just before draw k; with `dump_fmt`/`dump_w`, k counts only draws whose target n has that format and width |

`/tmp/decode_dumps.py` (kept beside the dumps during the session) turned the raw files into PNGs by
format. Environment switches from the same build: `SUYU_SPLIT_EVERY_DRAW`, `SUYU_SPLIT_CLEARS`,
`SUYU_WAIT_IDLE_EVERY_SUBMIT`, `SUYU_SKIP_COMPUTE`, `SUYU_KEEP_COMPARE`, `SUYU_NO_HEAP_FLAGS`,
`SUYU_DEDICATED_IMAGES`, `SUYU_INVARIANT`, `SUYU_FORCE_ASTC_DECODE`. Metal's own validation is
`MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=nslog MTL_DEBUG_LAYER_WARNING_MODE=nslog`, and it
is what named the cause.


Read suyu's own log first. Its logger runs on a background thread, so **the tail is always lost on
a crash**; absence of an expected message proves nothing. Run the binary directly to capture
stderr, which Finder swallows. Do not trust a backtrace whose frame offsets are enormous
(`ApplyAppMode +1915996` was the nearest export, not the crash site) - build `RelWithDebInfo` or
`-O0` for real frames. To find where an exception originates, break on `__cxa_throw`. lldb's
shell expansion chokes on filenames with brackets; symlink the ROM to a simple path. lldb batch
mode aborts its remaining `-o` commands on a signal stop; use `-k` for anything that must run
after a crash. For heap corruption, Guard Malloc first, ASan second - see Part 4.

Bisect one variable at a time. Changing a config value and a patch together cost an hour here.

**Editing `qt-config.ini` by hand: clear the `\default` flag.** Every key is stored as a pair:

```
accelerate_astc\default=true
accelerate_astc=1
```

When `\default=true`, suyu ignores the explicit value and uses the built-in default. Changing only
the value line does nothing, and suyu's own settings dump at the top of `suyu_log.txt`
(`-- Renderer.accelerate_astc: Gpu`) is the only place that tells you. Set `\default=false` as
well, or change the setting through the UI. Several experiments in this document were run with
edits that had silently not applied; each is marked.
