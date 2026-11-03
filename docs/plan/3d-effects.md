# Plan: the race's effects as a 3D particle system

Status: implemented 2026-11-02 for the GL and GXM backends; the GXM side
is built but not run, and the Vita's new shaders are not compiled yet.

Differences from the plan below, as built:

- Particles are not made from `g_effects[]`. `gl3d.c` takes the faces of
  the effect meshes (`gl3d_set_particles`) that it already turns into
  screen boxes, so sizes and places are the 2D's exactly, and draws them
  into the layer instead of through the copy layers.
- There is no atlas. The shader reads the same index pages as before, then
  a 256 x 1 RGBA row table from `fx_blend_rows`.
- Corners are expanded on the CPU, as the 2D boxes were; sparks and stars
  are the same boxes, not streaks.
- Not done: the original effect size (phase 6), `--fx-test`, `PROF_FX3D`,
  `tools/shd_fit.py --counts` from a real race.

## Problem

The race's effects are the original's 2D effects. `src/race/fx.c`
writes them as tiny meshes, one for each of its 200 slots:

- screen-sized sprites (face type 7, type 8 for some);
- points of 1 or 2 pixels (types 1 and 2);
- lines 1 pixel wide (type 13, for rain);
- quads that go through a colour table (type 19, for splashes).

`gl3d.c` turns each one into a box in screen space. A sprite's texel
is a colour index, and it goes through the level's `.SHD` table over
the index already on the screen, `T[src << 8 | dst]`. So each sprite
must read the pixels under it.

On the Vita, each read ends a GXM scene (about 0.25 ms). `gl3d.c` puts
the faces into overlap layers, with one copy for each layer, up to
`MAX_LAYER` (5). Thick smoke still costs up to about 10 scenes a frame.
Smoke above layer 5 reads an old frame, so it is drawn wrong. The
effects are also drawn at 40% of the original's size (`FX_SIZE`) so
that fewer of them overlap.

## Goal

A new particle system that draws the effects as **3D objects**:

- particles placed in world space, with RGBA textures, sorted by depth;
- mixed by the GPU's fixed-function blending: premultiplied alpha for
  smoke, dust and spray, additive for fire, sparks and halos.

Nothing reads the target, so there are no copies and no layers.

A setting chooses between them, **EFFECTS: 2D / 3D**. 2D is the
default everywhere until 3D shows a measured gain on the Vita. The 3D
effects must look the same as the 2D ones, or nearer to the original:
the same frames, colours, timing, motion and size on the screen.

The effect logic does not change. `fx.c` keeps all of its state in
`g_effects[]`, the same as with 2D. The particle system only reads
that state. So the timing of the original game, the sounds and network
play stay the same.

## What changes and what does not

| Effect (`fx.c`) | 2D now | 3D |
|---|---|---|
| 1 turning sprite: smoke, dust, spray, exhaust, boost and skid smoke, chimneys | type 7 sprite through `.SHD` | a billboard in world space, turned by the effect's angles, its frame from `f38.frames`, alpha blend |
| 4 fireball, boost flames | type 7 sprite | a billboard, additive |
| 2 spark | a point of 1 or 2 pixels, its colour moving along a palette ramp (`f24`) | a short streak along its velocity, additive, the colour from the palette at `f24` |
| 8 star (four points turning) | 4 points | 4 small additive sprites in the same turning square |
| 9 splash | 2 faces through `.SHD` | the same quad in world space, its frames as an RGBA texture, alpha blend |
| 11 halo (a car put back) | type 7 sprite | an additive billboard on the same path |
| rain (`race_weather`) | lines through `.SHD` (colours 0x77 to 0x7b) | streaks in world space, alpha blend |
| snow | type 7 sprites | small billboards |
| 5 flying parts | solid triangles | unchanged: they are already 3D meshes |
| 6 lightning | a mesh, and the palette flashing | unchanged |
| 3 explosion, 7 sink, 10 fall | only start other effects | unchanged; what they start is drawn as above |
| shadows, skid marks, glass | faces through `.SHD` | unchanged, still with the palette (see "Later") |

## Design

### 1. Where the particles are drawn

The 3D view is a target of colour indices (`RG8` with depth). GPU
blending needs RGB, so the particles get a layer of their own:

1. The view is drawn as it is now. The solid meshes go into the index
   target. The remaining table faces (shadows, skid marks, glass) are
   drawn with their layers. They overlap much less than smoke does.
2. **The particle layer** is an `RGBA8` target of the view's size. It
   uses **the view's depth buffer** and starts cleared to 0. The
   particles are drawn into it far to near:
   - depth is tested and never written;
   - smoke uses premultiplied alpha, `ONE, ONE_MINUS_SRC_ALPHA`;
   - fire uses additive blending, `ONE, ONE`, so alpha stays 0.
3. **At the present**, `gfx_draw_layers` shows the view through the
   palette, as now. It then puts the particle layer over it
   (`rgb + (1 - a) * view`), and then the 2D (`OVER`), as now.

Premultiplied "over" is associative. So building the layer apart and
putting it over the view at the end gives the same picture as drawing
every particle over the view. The palette only needs to be applied
once, at the present, as now.

On GXM that is one more scene for each view: the index scene, then
the particle scene. However many particles there are, it stays that
way. With split screen, each view draws its particles inside its own
columns (`gl3d_view_columns`).

Glass does not write depth. So smoke behind a car's window is drawn
in front of the window. Now the two are sorted together. The
difference is small, and it is listed under "Parity".

### 2. The particles (`src/fx3d.c`, new)

`fx3d_build()` runs once a frame, after the effects' tick and before
the 3D is drawn. It walks `g_effects[]` and the weather drops, and
makes a list of particles. Each particle has:

- a world position;
- a velocity, for streaks;
- the camera-facing axes, or its own axes for splashes and the
  turning type 1;
- a half width and a half height in world units, or in pixels for the
  sprites that keep a fixed size;
- a frame in the atlas (u0, v0, u1, v1);
- a colour multiplier;
- a blend: alpha or additive.

The list is sorted far to near by view depth (at most about 200
effects plus the weather drops). Each particle is 4 vertices: the
centre, a corner (-1 or 1, -1 or 1), the size, the UVs and the colour.
The vertex shader moves the corner out along the camera's right and up
axes, or the particle's own axes. GXM has no geometry shaders, so this
is the way to make the quads on the GPU. One stream holds all the
particles. There is one draw for each run of the same blend and atlas.
The effects' scene objects (`g_part_objs`) are left out of
`gl3d_draw`. With 3D, `is_dynamic` sends them to nobody.

**Size and place on the screen as with 2D.**
- A type 7 sprite is `w[7] * 4 / depth` scaled by its hot spot
  (`collect` in `gl3d.c`). That is a size in world units divided by
  depth, so it becomes a billboard with world size
  `hot * w[7] * 4 / 2^24`, with the same factors that `dyn_box` uses.
- Type 8 keeps its size in pixels, which the vertex shader works out
  in clip space.
- The near cut-off (depth <= 200) and the type 20 rule stay.

A scripted scene (see "Parity") checks that a sprite covers the same
pixels in both modes.

**The effects' size.** `FX_SIZE` (4) makes the effects smaller than
the original game. With 3D, use the original size (10), because
overlaps no longer cost anything. That is nearer to the original. It
is a separate commit, measured on the Vita.

### 3. The atlas: the original frames as RGBA

The textures come from the game's own frames, so they look the same.
At level load, and again when the palette changes, `fx3d` reads the
frames in the effects' texture slots (the pages that `texture_get(slot)`
loads for `f24`, with the rectangles of `g_fx_frames` and the halo and
snow rectangles). It turns them into one `RGBA8` atlas. For each texel
with colour index `s`, the colour comes from the table row that a 2D
sprite would use:

- **Colour 0** becomes transparent (`a = 0`).
- **A row that draws one colour** becomes that palette colour, opaque.
  This is exact.
- **A row that blends** becomes the least-squares fit of
  `T[s][d] ≈ C + k * palette[d]` over the 256 colours. The texel is
  the premultiplied `C`, with alpha `1 - k`. A row that makes colours
  brighter (`k > 1`) becomes additive.

The fit was measured on `USA.SHD`, `JAPAN.SHD` and `BRAZIL.SHD`:

| Rows | Count |
|---|---|
| Draw one colour (exact) | about 92 |
| Fit within RMS 8 of 255 | about 55 to 88 |
| Do not fit any linear blend (RMS 40 to 62) | about 40 to 45 |

The rows that do not fit move a colour along the palette's ramps. For
those, use the mean of the result as the colour, with an alpha from
how much the result changes with the colour under it. Phase 0 finds
out how many of these rows the effects really use.

Palette changes: lightning (`race_palette_tint`) and fades. The atlas
is small (the effect frames only), so it is made again when the
palette changes. If that is too slow on the Vita during lightning,
apply the tint in the shader instead. It is a linear move toward one
colour.

Filtering: nearest, as the 2D. A later option can use bilinear
filtering and mipmaps for softer smoke at NATIVE.

### 4. Additions to `rnd.h`

Both backends need these. macOS is where the parity is checked.

- A texture format `RND_RGBA8`. In GL: `GL_RGBA8`. In GXM: texture
  `A8B8G8R8` and colour surface `A8B8G8R8`.
- `rnd_target_shared(color, other)`: a target that uses the depth
  buffer of `other`. In GL: the same depth renderbuffer in a second
  FBO. In GXM: the same depth surface. A scene already stores depth at
  its end and loads it at the start of the next scene.
- `rnd_blend(RND_BLEND_OFF | RND_BLEND_PREMUL | RND_BLEND_ADD)`. In GL:
  `glBlendFunc`. In GXM, blending is part of the fragment program
  (`blendInfo` in `sceGxmShaderPatcherCreateFragmentProgram`). So
  there is one patched program for each pair of program and blend,
  made when first used and kept.
- New programs:
  - `RND_PARTICLE` (centre, corner, size, uv, colour; the atlas);
  - `RND_LAYERS`: the present of the view, with the particle layer
    over it. It can also be a second quad that uses `RND_BLEND_PREMUL`
    onto the screen.

  Their Cg goes in `rnd_gxm.c`. The new `.gxp` files must be compiled
  once on a Vita that has `libshacccg.suprx`, then committed in
  `platforms/vita/shaders/`.

### 5. The setting

- `SET_EFFECTS = 0x5a0` in `settings.h`. It is a third word in
  `ign_port.cfg`. `port_settings_load` reads 1, 2 or 3 words and gives
  a missing word its default: 3D where `__vita__` is defined, 2D on
  the other hosts.
- In GFX OPTIONS, an item **EFFECTS** with **2D** and **3D**, under
  RENDERER (`add_port_options`). Check with `--shot` that 320 x 200
  still shows the whole page.
- It is read when a race starts. It is used with HARDWARE and SOFTWARE,
  which draw the 3D on the GPU. SOFT 3D always draws 2D effects.
- A command-line flag, `--effects 2d|3d`, for scripted comparisons.

## Phases

Each phase is one commit, measured on its own (the frame-budget and
pixel-parity skills).

0. **Measure.** Count, for each table face of the effects, its texels
   by colour index, weighted by the pixels covered. Do this in a race
   with smoke, a crash, boost and rain. Add `tools/shd_fit.py`. It
   gives the weighted error of the atlas colours for each level, and
   shows which effect frames fall on rows that do not fit. Choose how
   to handle those frames before Phase 3.
1. **`rnd.h` additions** in both backends: `RGBA8`, the shared depth,
   blending, and the particle and layer programs. No change to what
   the game draws.
2. **The atlas** (`src/fx3d_atlas.c`) and `tests/test_fx3d.c`. The test
   checks that a synthetic alpha row gives back its alpha, that a row
   of one colour is exact, that colour 0 is transparent, and that the
   frames have the right rectangles.
3. **Sprite effects in 3D:** types 1, 4 and 11, and the snow. The
   particle layer, and the present that puts it over the view. Behind
   `--effects 3d` only.
4. **The rest:** sparks and stars as streaks, splashes as quads, and
   rain as streaks.
5. **The setting**, the menu item and the Vita default. Update
   `docs/vita.md`, `docs/engine.md` (the effects) and the changelog.
6. **The original size** (`FX_SIZE` 10) with 3D, if the Vita still
   holds 36 frames a second.

## Parity

Add a debug scene, `--fx-test N`. It holds the camera fixed on a
track and starts effect type N (or every type, one after the other)
at a fixed place, with a fixed random seed. Write shots of the same
frames with each setting on macOS:

```
ignition --fx-test 1 --effects 2d --shot MS:2d.tga
ignition --fx-test 1 --effects 3d --shot MS:3d.tga
```

Compare them with the pixel-parity skill:

- **Coverage:** the pixels each effect covers, as masks. They must
  match within a pixel at the edges.
- **Colour:** the RGB difference inside the masks, as a mean and a
  99th percentile.

Do this for each effect type on a light road and a dark road, and in
the rain and the snow. Save the shot pairs with the commit. A frame
that looks clearly different (for example, a ramp row that has become
grey) is a defect. Fix it in the atlas; do not just accept it.

Known differences, which are allowed:

- Where 2D smoke goes above `MAX_LAYER`, 3D is right and 2D is wrong.
  For that comparison, set `MAX_LAYER` high on macOS.
- Smoke behind glass (see "Design", section 1).
- Sparks are streaks instead of single points. They are closer to how
  they look at 320 x 200 than a point is at 960 x 544.

## Performance

On the Vita, read the stats line in `log.txt` (frame rate, scenes,
copies, time spent ending scenes) and `IGN_PROF`'s `PROF_DYNS`,
`PROF_LAYERS` and `PROF_SORT`. Add a `PROF_FX3D` for building the
particles. Measure each setting in the same places: the grid at the
start, a pile-up, boost and rain. Do this before Phase 3 and after it.

Targets with 3D:

- at most 2 scenes for one view, plus what the shadows need;
- no copies made for effects;
- 36 frames a second in the places that now fall below it.

The extra cost:

- the particle layer, `720 x 408 x 4`, about 1.2 MB of CDRAM in each
  slot of the target ring;
- a cleared layer in each frame;
- one more texture fetch for each pixel at the present.

## Risks

- **Rows that do not fit** the blend. Phase 0 is the gate.
- **Palette timing.** The atlas is made with the palette of the moment.
  Check that lightning and fades set the palette before the frame's 3D
  is drawn. If not, the effects lag by one frame.
- **Shadows and skid marks** still use layers. If they still cost too
  much on the Vita, move them later (see "Later").
- **GXM memory** for the layer targets in the ring.
- **Menu room** at 320 x 200.
- **Vita3K** cannot test this. Its Metal backend has no triangle fans,
  and new shaders need a real Vita to compile them first.

## Later

- Shadows, skid marks and glass as RGBA blended faces too, with the
  same atlas fit applied to flat colours. Then the view needs no
  copies at all.
- Bilinear filtering, mipmaps and soft particles (fading where smoke
  meets the ground, using the depth buffer) at NATIVE.

## Files

| File | Change |
|---|---|
| `src/rnd.h`, `src/rnd_gl.c`, `src/rnd_gxm.c` | `RGBA8`, the shared depth, blending, `PARTICLE`, `LAYERS` |
| `src/fx3d.c`, `src/fx3d.h`, `src/fx3d_atlas.c` (new) | particles from `g_effects[]` and the weather; the RGBA atlas |
| `src/gl3d.c`, `src/gl3d.h` | leave the effect objects out with 3D; the particle layer target; draw the particles |
| `src/gfx.c`, `src/video.c` | put the particle layer over the view at the present |
| `src/race/fx.c` | export the frame tables and the weather drops that `fx3d` reads; no change to the logic |
| `src/settings.h`, `src/frontend/frontend.c`, `src/main.c` | `SET_EFFECTS`, the menu item, `ign_port.cfg`, `--effects`, `--fx-test` |
| `tests/test_fx3d.c`, `tools/shd_fit.py` (new) | the atlas test, the measurement of rows |
| `platforms/vita/shaders/` | the new compiled programs |
| `docs/vita.md`, `docs/engine.md`, `CHANGELOG.md` | documentation |
