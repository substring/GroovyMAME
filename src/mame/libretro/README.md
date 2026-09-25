# libretro driver

GroovyMAME can run libretro cores as an emulated system named `libretro`.
The core runs one `retro_run` per emulated frame, so switchres, frame delay
and vsync apply to it like to any other system. Both software rendered and
OpenGL hardware rendered cores are supported.

Tested on Linux only so far.

## Building

The driver is part of a regular build. For a quick build containing only
this driver and the emulator core (SDL2 OSD):

```
make SOURCES=src/mame/libretro/libretro.cpp SUBTARGET=gmlibretro -j$(nproc)
```

This produces `./gmlibretro`. It needs the usual GroovyMAME SDL2 build
dependencies (SDL2, SDL2_ttf, fontconfig, X11/Xrandr, libdrm development
packages).

## Options

| Option | Default | Purpose |
|---|---|---|
| `-L`, `-libretro_core` | | The core: a full path, a file name, or a core name (`genesis_plus_gx`, the platform suffix `_libretro.so`/`.dll`/`.dylib` is added), the last two searched in `libretropath` |
| `-libretropath` | `libretro` | Search path for the cores (several directories separated by `;`) |
| `-cart`, `-cartridge` | | The content (ROM, GDI, CHD...), with the extensions the core accepts |
| `-libretro_system_directory` | `libretro/system` | System directory given to the core: BIOS files, some cores also write there |
| `-libretro_gl_device` | default GPU | GPU rendering hardware rendered cores, as a DRM render node, e.g. `/dev/dri/renderD129` |
| `-libretro_gl_readback` | `auto` | Readback of hardware rendered frames: `persistent`, `pbo` or `direct`, to compare them |

`-verbose` logs the core, its geometry and timing, the GPU used and the
readback timings of hardware rendered cores.

## Files

| What | Where |
|---|---|
| Core options (Tab menu, **Core Options**) | `<cfg_directory>/libretro/<core name>.opt`, RetroArch format (`key = "value"`), written when changed |
| Core options, older location, read first | `<libretro_system_directory>/<core name>.opt` |
| Battery save RAM | `<nvram_directory>/libretro/<content name>.srm` |
| Save directory given to the core | `<nvram_directory>/libretro/` |

`<core name>` is the name the core reports, e.g. `Genesis Plus GX`, like
RetroArch's per-core `.opt` files, which can be copied as is.

## Running

Software rendered core:

```
gmlibretro libretro -L genesis_plus_gx -cart "Sonic The Hedgehog 2 (World).md"
```

Hardware rendered core:

```
gmlibretro libretro -L flycast -cart "Game (Europe).gdi"
```

Cores are looked up in `libretropath` (`libretro/` by default); add
`-libretropath /path/to/cores` or give the full path to `-L`.

## BIOS files

BIOS files go in the system directory (`libretro/system/` by default), in
the layout each core expects: see the core's page on docs.libretro.com, or
the `firmwareN_path` lines of its `.info` file. For instance Flycast wants
`libretro/system/dc/dc_boot.bin` (and `dc_flash.bin`, `naomi.zip`,
`awbios.zip`... for NAOMI and Atomiswave), and writes its VMU files there,
so the system directory must be writable.

## Hardware rendering

OpenGL cores (compatibility, core profile, OpenGL ES 2/3) get an OpenGL
context owned by the driver, created with EGL on a DRM render node, so it
works under X11, Wayland and the KMS console. The frame is read back into a
bitmap within the same frame (no added frame of latency), so hardware
rendered cores work with every video backend, including `kmsraw` and
MiSTer. The GPU rendering the core can differ from the one driving the
display (`-libretro_gl_device`).

Not supported yet: Vulkan and Direct3D cores, hardware rendering on
Windows and macOS.

## Notes

- Switchres has no Wayland backend: for CRT mode switching, run fullscreen
  under X11 or on the KMS console. Use `-window` for tests on a Wayland
  desktop.
- `-noreadconfig` ignores `mame.ini`, handy to test in isolation.
