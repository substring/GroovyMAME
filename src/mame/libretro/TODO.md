# libretro driver - TODO

Priorities: P1 first, P3 last.

## P1

- **Multi-disc content (.m3u)**: cores parse the `.m3u` themselves; the
  driver has to implement the disk control interface
  (`RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE` / `_EXT_INTERFACE`: eject,
  set index, add image, labels), add a disc control menu, and keep the
  selected disc across save states.
- **Rotation and aspect ratio**: `RETRO_ENVIRONMENT_SET_ROTATION` (needed by
  FBNeo vertical games) and the aspect ratio reported by the core (the
  screen is fixed at 4:3 today).
- **Per-core and per-game input configuration**: the system is always
  `libretro`, so all cores share one input mapping. Also show the core's
  button names (`RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS`) in the input menu.
- **Refresh rate changes don't reach switchres**: with Flycast the mode sent
  to the display stayed at 60.0000 Hz while the core reported 59.945 then
  59.827 Hz (seen with `-video mister`), so with `-syncrefresh` the game
  would run about 0.3% too fast. Check how a new frame period from
  `RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO` goes through the screen
  reconfiguration and the switchres resolution change check.
- **Hardware rendering**
  - Validate on Windows (WGL context, `d3d11` and `opengl` renderers): builds
    with MinGW, never run yet.
  - Vulkan cores (`vkCmdCopyImageToBuffer` into cached host memory, read
    back in the same frame).
  - macOS.

## P2

- **Reuse RetroArch per-core and per-game configuration**: mirror
  RetroArch's layout under `cfg/libretro/<core name>/` so its `config/<core
  name>/` folders can be copied as is: `<core>.opt`, `<content dir>.opt`,
  `<game>.opt` (priority game > directory > core), `*.switchres.ini`
  overrides (native switchres format), and the CRT keys of the `.cfg`
  overrides (`crt_switch_resolution` -> monitor, `crt_switch_resolution_super`
  -> super_width, `crt_switch_porch_adjust` -> h_size = 1 + porch / 100,
  `crt_switch_center_adjust` -> h_shift, `crt_switch_vertical_adjust` ->
  v_shift). The current `cfg/libretro/<core name>.opt` moves to
  `cfg/libretro/<core name>/<core name>.opt`. Validate with real RetroArch
  files, then document.
- **Downscale high internal resolutions to native (SSAA)** for CRT: cores
  render upscaled through their own options (Flycast, Beetle PSX,
  PCSX-ReARMed) and the screen follows the upscaled size. Downscale on the
  GPU before the readback for hardware rendered cores (also less to read
  back), by block averaging for software cores; setting per core in a
  "GroovyMAME" category of the Core Options menu. Also needed for MiSTer,
  whose renderer is limited to 768x576.
- **Mode switches at startup**: the display switches mode three times when
  starting (default 320x240, then the base geometry reported by the core,
  then the size of the first frame; Sonic 2: 320x240, 256x192, 320x224).
  Wait for the real geometry before reconfiguring the screen.
- **Speed below 100% with cores running two frames per `retro_run`**:
  Flycast games rendering at 30 fps (Demolish Fist) run at 95-97%, in a
  window as well as to MiSTer, while Virtua Tennis 2 and Genesis Plus GX
  reach 99.7-100%. Each run emulates two frames within one frame slot, and
  the next slot skips `retro_run`; check whether the heavy frames overrun.
- **Controller types** (`retro_set_controller_port_device`): right analog
  stick, keyboard, mouse, lightgun, multitap, rumble.
- **Audio**: integrate with GroovyMAME's audio synchronization instead of
  the simple queue, validate on a CRT.
- **Rewind**: check that MAME's `-rewind` works with the core states.

## P3

- **Expose the core memory as MAME address spaces**
  (`retro_get_memory_data`, `RETRO_ENVIRONMENT_SET_MEMORY_MAPS`) through a
  device with `device_memory_interface`: debugger memory viewer and
  watchpoints, Lua access, base for cheats and RetroAchievements. The
  debugger can't step into the core's CPUs; debug cores with gdb.
- Run-ahead (replaying frames through the core states).
- Core cheats (`retro_cheat_set`: Game Genie, Action Replay codes).
- RetroAchievements.
- Subsystems (`retro_load_game_special`: Super Game Boy, Sufami Turbo...).
- Study RetroArch's netplay.

## Not prioritized

- Validate fullscreen on a CRT with switchres and frame delay.
- Automated test of the 16 buttons of the 4 RetroPads.
- Window title still "libretro core [libretro]" (built by the OSD before the
  core is loaded).

## Out of scope

Jobs for an external frontend: downloading and updating cores, playlists
with thumbnails, BIOS checks from `.info` files. Niche libretro interfaces:
microphone, camera, sensors, location, MIDI.

## Done

- Software rendered cores: video, audio, 4 RetroPads, MAME save states,
  battery save RAM.
- Command line: `-L` (path, file name or core name in `libretropath`),
  `-cart`.
- Core Options menu, options saved per core (RetroArch format).
- OpenGL hardware rendered cores on Linux (EGL) and Windows (WGL), read back
  in the same frame, working with every video backend.
- Content in zip and 7z archives.
- Frame pacing on the emulated time for cores running more than one frame
  per `retro_run`.
- Checked with MiSTer output (`-video mister`) against a fake GroovyMiSTer
  server.
- Minimal footprint in MAME: new files, plus small additions to
  `mame.lst`, `emuopts`, `mainmenu.cpp`, `info.cpp` and `frontend.lua`.
