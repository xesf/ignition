# Android renderer performance

The normal Android build includes whole-pass DYN vertex streaming, conservative
screen culling, redundant GL state suppression and shared texture-page arrays.
Coherent framebuffer fetch is used only when the driver advertises
the exact coherent extension and the shader compiles and links. Unsupported
drivers or failed shaders retain the ordinary shader and dependency-preserving
copy path. Arena allocation failure retains the original per-face uploads.
Other platform builds keep their existing renderer paths.

These paths are built into the normal Android renderer; no extra compiler
flags or user settings are needed.

The changes preserve face order, texture/table boundaries, palette and depth
behavior. They do not change gameplay, physics, effects or device resolution.
Optional `IGN_PROF` renderer counters report copies, draws and uploaded bytes;
without `IGN_PROF`, the counters compile away.

Run the asset-free host tests from the repository root:

```sh
make -f tests/renderer.mk test-renderer
```

They are also part of `make test`. The standalone target needs GNU Make,
a native C compiler and a POSIX shell; on Windows use Git Bash or put Git's
`usr/bin` on the command's PATH, with `CC=gcc`. Windows-specific unwind and
switch-table options apply only to host fixtures. The tests include feature
and platform controls, coherent-extension matching, shader failures, dependency layers,
full uploaded vertex payloads and profiler reporting. They need no ROM,
display server, Android device or SDK.

Faces wholly outside the screen are conservatively removed before upload.
Intersecting and uncertain faces retain their stable draw order.

The renderer caches redundant GL state and uniform updates, with invalidation
on resource deletion, resize and reinitialization.

Compatible texture pages share an array allocation and a batch key. Original
per-texture admission limits, face order, page coordinates and fallback textures
remain intact. The headless tests cover pooled arena uploads, survivor payloads,
run offsets, GL cache invalidation, compile/link failures, allocation failure,
reinitialization and non-fetch rendering.
