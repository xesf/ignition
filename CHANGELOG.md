# Changelog

All notable changes to this project are documented in this file.

## [Unreleased]

## [0.4.0] - 2026-11-03

The GOG and Steam releases run as they are, with their Ogg Vorbis music; the Vita's menu and races draw faster; and there is an option to draw the race's effects in 3D.

### Added
- The GOG and Steam releases work as they are: copy them into `data/` and the game finds the files there, with the CD music decoded from their Ogg Vorbis tracks (`game.ins` lists them; `game.gog` is not needed). Disc images are also found one folder down, such as `data/images-disks/<image>/disc`. Without `TEST.PFM`, which those releases lack, the menu plays the shorter film (`TEST2.PFM`).
- GFX OPTIONS has an EFFECTS item, 2D or 3D. With 3D, the race's effects (smoke, fire, sparks, splashes, rain and snow), shadows, skid marks and glass are drawn after the 3D into a layer of their own that the GPU blends over the view, instead of reading the pixels under each one. It is 2D by default; `--effects 2d|3d` sets it for one run, and the choice is kept in `ign_port.cfg`.
- On the Vita, L and R together save a screenshot to `ux0:data/ignition/shotNN.tga`.

### Changed
- On the Vita the race's 2D picture is shown from the game's own buffer without a copy, and rows that are empty are not compared, which took the frame's present step from 18 ms to about 7 ms.
- On the Vita the menu's background film (see-through faces drawn in at most 16 layers; one past that is left out) runs at about 45 frames a second through its crash scenes (30 at the worst) instead of 5 when each face copied the screen.
- The Vita reads the game from `ux0:data/ignition` when the game's files are there, and from `ux0:data/ignition/disc` as before. `make vita-deploy` and the LiveArea art look for a disc under `data/` or one folder down.
- The stats line the Vita logs has the parts of the frame's present step and the 2D picture's rows.

### Fixed
- On the Vita, smoke and explosions in the menu's film no longer draw as rectangles that erase the train and cars under them; a few small patches can still show in the biggest explosions.
- The Vita package has the game's own LiveArea art again when the disc is under `data/images-disks/`.

## [0.3.0] - 2026-10-25

Network play, a software 3D renderer for the original resolutions,
touch in the Vita's menus, and the Vita's races hold their frame rate
better, with every computer car racing.

### Changed
- The Vita draws the race's 3D at 720 x 408 (75%), stretched to the screen, to hold 36 frames a second more often; the HUD and menus stay at full resolution.
- The Vita holds its frame rate in thick smoke and sparks: see-through faces stack at most six layers deep, where a pile of them took up to 96 GPU scenes a frame, and the 2D's textures upload only the rows that changed.

### Fixed
- In NATIVE, the championship's podium, the retry offer and GAME OVER are drawn at the menus' size, as the results are, instead of at the screen's own pixels.
- The game no longer crashes after a championship race on the Vita: the sound mixer could still be mixing the race's voices while the race freed them.
- On the Vita, computer cars no longer stand at the start until bumped. Most tracks' racing lines give surfaces past the 30 in the grip table; the original reads the doubles around the table, and the port read past its own, which on the Vita held NaN and gave a throttle of 0.
- On macOS, Right Shift (the boost), Right Control and Right Option are told apart from the left keys.
- The championship's end shows the podium: the cup or statue, the league completed, the place and the score.
- A view wider than 4:3 no longer shows black where the track was not gathered: the port gathers a wider disc of cells about the camera.

### Added
- Touch in the Vita's menus: a tap on the front screen selects an item, a tap on the selected item activates it (or steps its value), and a tap on a flag picks the language. On macOS a click does the same, and `--tap MS:X:Y` scripts one.
- Network play: MULTI PLAYER, NETWORK GAME races up to six players over a local network, on UDP instead of the original's DirectPlay and IPX. `--host` and `--connect HOST` choose to host or join; see [docs/network.md](docs/network.md).
- `--soft3d` draws the 3D on the CPU with the original's own pipeline and rasteriser, ported: the race and the front end's cars and background, in 8 bit colour at the original resolutions (320 x 200, 640 x 480, 800 x 600; NATIVE races at 800 x 600), with no GPU 3D. GFX OPTIONS' RENDERER item selects it as SOFT 3D. With it, MIP MAPPING draws the original's blurred and magnified texture levels.
- `--fps` and Alt+F show the race's frame rate; a second Alt+F adds the original's other debug lines, which it never showed.
- SOUND OPTIONS has MUSIC VOL., 0 to 10, for the CD music apart from SFX VOLUME; it is kept in `ign_port.cfg`.
- GFX OPTIONS has a RENDERER item: HARDWARE draws the 2D on the GPU, SOFTWARE on the CPU as the original. Choosing a resolution sets its default, HARDWARE for NATIVE and SOFTWARE for the original sizes; the choice is kept in `ign_port.cfg`.
- Choosing one of the original resolutions sizes the window to it (320 x 200 shown at 640 x 480); NATIVE gives the window back its size.
- The game's joystick controller: with JOYSTICK chosen for a player in the options, the left stick steers with analog control and the triggers (or A and B) accelerate and brake, on the Vita, Windows, Linux and the web.

## [0.2.0] - 2026-10-24

The Vita becomes playable, championships play through, and NATIVE's
menus, results and messages are laid out at the menus' size.

### Changed
- The Vita renders much faster: a copy for the colour tables is a scene break instead of two scenes, see-through faces are drawn in overlap layers and batched, render targets take 8 scenes each in a ring, and draws set only the state that changed.
- Vita race controls: the right and left triggers accelerate and brake, Cross is the action (boost), the d-pad's up and down change gear, and the d-pad or left stick steers.
- The race effects (smoke, sparks, explosions, nitro, rain) are drawn at 40% of the original's size.
- The game finds its data in the first `data/*/disc` folder by name, whatever the disc image's folder is called.
- The sources are grouped in folders by module: `src/race/`, `src/menu/`, `src/frontend/` and `src/platform/`.

### Fixed
- On the Vita, Cross goes on from the results.
- The first frame of a new track no longer shows the last track's view.
- `--view` shows a picture's colours right: the `.PIC` palette is at 0x48.
- The Vita deploy copies the CD music from a disc image that has audio tracks, and leaves no macOS `._` files on the card.
- CD music plays on the Vita: tracks stream from the disc image instead of being read whole into memory.
- The menu's corner vignette reaches the screen's edges when NATIVE's margins are not whole menu pixels (the Vita).
- On the Vita, the next race in a championship no longer shows the last track's textures and colours.
- NATIVE's race messages (LAP RECORD, TRACK RECORD and the like) are drawn at the menus' size, as its HUD.
- On the Vita, a re-created sprite no longer shows its old picture: the language page's selected flag now follows the selection.
- A championship no longer quits after its second race: the end of a race frees what the race loaded.
- NATIVE's track results, track score and totals are drawn at the menus' size.
- NATIVE's menus sit closer under the logo, with their rows spread out, and the last options (RESET GHOST CAR and its YES / NO) draw in full.

### Added
- `docs/formats.md`, a reference of the game's file formats, and `docs/engine.md`, how the engine and its tracks work.
- The Vita's LiveArea bubble, backdrop and gate are made from the game's installer picture and logo when the build finds an extracted disc.

## [0.1.0] - 2026-10-23

The first version: the front end and single-player races are playable from
start to finish, from the files of your own copy of the game.

### Added
- Game data from the disc image: the game reads the `.cue` and `.bin` files of your own disc. `make extract` (or `cd_extract`) copies the files and the audio tracks out of an image, and the packaged builds do it by themselves at the first start when they find an image next to the program.
- Front end: the intro film, the language page, the main menu and every page under it, the options, the best times, the cheat codes, and the 3D cars on the car select page over the animated track.
- Races: the tracks, the cars on the grid, the car physics and ground contact, the computer drivers, the player controls, the race view and the HUD, sound effects, the race effects and animated textures, the results, and two players on a split screen.
- CD music, played from the disc image, and the game's sound banks and voices.
- Graphics on the GPU at the display's resolution. The **NATIVE** choice in the resolution option uses the window's own size, with the menus, the loading screen and the race HUD laid out for it. `--software` (or Alt+R) draws the 2D on the CPU, as the original does.
- Platforms: macOS, Linux (also as an AppImage for the Steam Deck), Windows x86_64, the web (WebGL 2), Android, iOS and iPadOS, and the PlayStation Vita. Each host uses only its system's own APIs. On every platform but macOS, a gamepad (or the Vita's buttons) presses the game's keys, and touch screens get an on-screen pad.
- `--autopilot`, where the computer drives the player's car, and `--press`, `--shot` and `--background` for scripted test runs.
- `--version`, and the port's version in the log after the original game's version line.
- Release packages for every platform from GitHub Actions, published with the changelog when a version is tagged.
