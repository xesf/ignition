# PlayStation Vita port

The Vita build runs the same engine and the same renderer as macOS. The
renderer (`canvas.c`, `gl3d.c`, `gfx.c`) draws through a small set of GPU
operations, `src/rnd.h`, with one backend for each host:

- `src/rnd_gl.c`: OpenGL 3.2 core, on macOS.
- `src/rnd_gxm.c`: GXM, the Vita's own GPU API.

The host is `src/platform/vita.c` (buttons, clock, audio, log). The build
uses VitaSDK's system libraries only.

Status: playable on a Vita, menus and races, at up to the game's own 36
frames a second. The shaders are compiled by the console's
`libshacccg.suprx` at the first start and kept. Vita3K on macOS cannot run
it (its Metal backend has no triangle fans).

## Build

You need VitaSDK, at `$VITASDK` or `~/vitasdk`. No extra packages.

```
make vita                      # -> build-vita/ignition.vpk (title IGNT00001)
```

`make vita-clean` removes `build-vita/`.

`tools/vita_sce_sys.py` makes the LiveArea images: the bubble, the
backdrop and the gate. With an extracted disc under `data/`, it makes them
from the game's installer picture (`INSTALL.PIC`) and logo
(`BALTAZAR/DATA/IGN_LOGO.PIC`). To use another disc, configure with
`-DIGN_DISC=DIR`. Without a disc, the tool draws a road at dusk instead,
so CI's packages hold no art from the game.

The LiveArea keeps the images it had when the package was installed. To
see new ones, install the `.vpk` again with VitaShell, or use "Refresh
LiveArea".

## Install

```
make vita-deploy
```

This copies the package to the Vita (`tools/vita_deploy.sh`):

- `eboot.bin`, `sce_sys/` and `shaders/` to `ux0:app/IGNT00001/`
- the package itself to `ux0:data/ignition/ignition.vpk`
- the extracted disc to `ux0:data/ignition/disc/`, if it is not there yet

The Vita is found in one of two ways:

- **A mounted ux0**: the memory card in a reader, or VitaShell's USB mode.
  The script takes the first volume in `/Volumes` that has `app/`, `data/`
  and `id.dat`, or the folder in `VITA_UX0`.
- **VitaShell's FTP server**: press Select in VitaShell, then
  `VITA_IP=<address> make vita-deploy` (`VITA_PORT` defaults to 1337). The
  disc is not copied over FTP; copy it once by hand.

The first time, install `ux0:data/ignition/ignition.vpk` with VitaShell,
or use VitaShell's "Refresh LiveArea", so the game gets its bubble. After
that, `make vita-deploy` replaces the files in place.

For the CD music, set `VITA_MUSIC=1`. This also copies the image's `.cue`
and every `.bin` it names to `ux0:data/ignition/`, next to `disc/`. To
use another extracted disc, set `DATA` to its `disc/` folder (the default
is the one `make` runs).

Settings are saved in the `disc/` folder, as on the PC. The log goes to
`ux0:data/ignition/log.txt` and `err.txt`.

## Shaders

GXM runs only compiled programs (`.gxp`), and the Mac has no compiler
for them. The Cg source is in `rnd_gxm.c`. The game looks for each
program in this order:

1. `app0:shaders/`, which the package fills from `platforms/vita/shaders/`.
2. `ux0:data/ignition/shaders/`, compiled by an earlier run.
3. Compiled now by the system's Cg compiler, `libshacccg.suprx`, loaded
   from `ur0:data/` (where Vita homebrew keeps it) or
   `ux0:data/ignition/`. The result is saved to (2).

A file's name holds a hash of its source, for example
`canvas_f_55cdda29.gxp`, so a changed shader is compiled again.

The first run on a Vita with `libshacccg.suprx` compiles all 16 programs.
The next `make vita-deploy` copies them into `platforms/vita/shaders/`
before it builds, so the package carries them. Commit them; then no
compiler is needed, and Vita3K can run the game too. Without them, a Vita
without the module, or Vita3K, stops at
`gxm: shader programs failed` in `log.txt`.

## Controls

The game reads a keyboard, so each button presses a key. In a race, the
keys come from player 1's settings (`g_race_cfg + 0x17f`). If you remap
the keyboard in the menu, the buttons follow.

| Vita | Menus | Race | Race paused | Y/N question |
|---|---|---|---|---|
| D-pad left/right, left stick | arrows | steer | arrows | |
| D-pad up/down | arrows | gear up / gear down | arrows | |
| R / L (trigger or shoulder) | | accelerate / brake | | |
| Cross | Return | action (boost); on the results, Return | Return | Yes |
| Circle | Escape | | Escape | No |
| Triangle | | look back | | |
| Start | Return | Escape (pause) | Escape | Escape |
| Select | Escape | Return (go on) | Return | Return |

## Renderer

The screen is 960 x 544. The NATIVE resolution follows it, and the 2D menu
is drawn at 4:3 in the middle, as on macOS.

Everything is colour indices until the present. Render targets hold an
index in R (`U8_R`), or an index and a "drawn" flag in R and G (`U8U8_GR`)
for the 3D. `gfx.c` shows them through the palette. How `rnd_gxm.c` does
what OpenGL does:

- **Scenes.** Each draw goes into the scene of the bound target. Drawing
  into another target ends that scene and begins one there. Colour stays
  in the target's memory between scenes. Depth is stored at the end of a
  scene and loaded again at the start of the next one.
- **Reading the pixel under a face.** The colour tables need the pixel a
  face covers, and the Vita's runtime Cg compiler cannot read the target
  in a shader (`FRAGCOLOR` and `__regformat` are not accepted). The GPU
  draws a scene in tile memory and writes it out at the scene's end, so a
  scene that samples its own target sees the target as the scene began.
  `rnd_copy` therefore copies nothing: it ends the scene, and the "copy"
  texture reads the target itself (`alias` in `struct texture`). That is
  one scene a copy.
- **Fewer copies.** Faces through a table go in layers (`layer_of` in
  `gl3d.c`): a face through a table goes one layer above the earlier faces
  whose triangles share area with it, any other face into the highest of
  their layers. Drawn layer by layer, with a copy before each, the frame is
  the same as with a copy before every face. Within a layer, faces in a
  row with the same texture and table go in one draw. On macOS each face
  copies its own rectangle, as before.
- **Render targets.** A render target holds the driver's memory for 8
  scenes in flight; a scene past that waits for the GPU. Each target size
  has a ring of 8 render targets, taken in turn.
- **Draw state.** A draw sets only the programs, uniforms, textures and
  depth state that changed since the last one in the scene.
- **Clears.** GXM has no clear. Colour is cleared with a quad. Depth is
  cleared by beginning a scene that does not load it.
- **Scissor.** GXM's region clip works on whole tiles, so the scissor is a
  test in the fragment programs (`u_clip`).
- **Shaders.** Cg has no `texelFetch` or `textureSize`. The shaders get
  texture sizes as uniforms and sample at texel centres; all textures are
  nearest. `gl_FragCoord` is `WPOS`. An index is written as
  `(i + 0.25) / 255`, so GXM's pack to 8 bits gives `i` whether it rounds
  or truncates.
- **No texture arrays.** A slot's 256 x 256 pages are tiles of one
  texture, 16 to a row (4096 texels, the widest GXM allows). The shaders
  find a page's tile from `u_grid`.
- **Which way up.** Targets are drawn the way their memory is laid out
  (row 0 first), as OpenGL's framebuffer objects are, so OpenGL's windings
  hold. GXM names windings as seen with y down, so a face OpenGL calls
  counter-clockwise is `SCE_GXM_CULL_CW`. The screen is drawn the other
  way up, as a window is.
- **Memory.** Two heaps from memblocks mapped for the GPU: CDRAM for
  textures, targets and the mesh store, and uncached main memory for
  textures filled each frame and buffers. The GPU runs up to two frames
  behind. So vertices go to a per-frame arena, a texture filled again gets
  new memory, freed memory waits three frames, and the mesh store has a
  copy for each frame in flight. A frame's last scene writes a
  notification that is waited on before its slot is used again.
- **Display.** Three buffers in CDRAM, shown by the display queue from
  `plat_present`, at the vertical blank.

- **Log.** Every two seconds the log has the frame rate and a frame's
  draws, scenes, copies and the time spent in the game, starting scenes,
  waiting for the GPU or the display, and filling textures and vertex
  memory. The Vita build also defines `IGN_PROF`, which adds the race
  frame's parts (`src/prof.h`).

## Effects

With EFFECTS 3D (not the default: on the Vita it has not been faster than 2D), the faces of the effects'
meshes (`gl3d_set_particles`) skip the layers and copies above. They are
drawn after the view into an RGBA layer (`gl3d_particle_texture`) that
shares the view's depth buffer, with the GPU's blend, `src + (1 - a)
dst`, and the layer is put over the view at the present. Each colour
index goes through a 256 x 1 table of premultiplied colour and alpha, made
from the colour table by `fx_blend_rows` (`src/fx_blend.c`). A frame has
one more scene for each view, however many effects there are.

The `particle` and `layer` programs are new: compile them once on a Vita
with `libshacccg.suprx` and commit the `.gxp` files (see "Shaders").

## Not done

- Races drop below 36 frames a second where many see-through faces
  (smoke, shadows, glass) overlap, as each overlap needs a scene of its
  own (see "Renderer").
- The race's 2D is drawn on the CPU. Its HUD triangles go through the GPU
  and are read back each frame, which waits for the GPU twice a frame.
- The game's joystick (`g_joysticks`, analog steering) is not fed from
  the stick; the stick presses the steering keys.
- No touch input, and no way to type names in the menus.
