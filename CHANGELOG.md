# Changelog

All notable changes to this project are documented in this file.

## [Unreleased]

### Added
- The default Android APK includes 32-bit ARM (`armeabi-v7a`), including TVs and other devices with a 32-bit Android userspace.

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
