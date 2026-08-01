# splashd for MiSTer FPGA
`splashd` is a standalone MiSTer companion daemon. It watches MiSTer's menu
selection files and updates the menu wallpaper from PNG/JPEG artwork in:

```text
/media/fat/splashd/wallpapers
```

It does not modify the MiSTer binary, its functionality, or interfere with any cores.

Demo:

https://github.com/user-attachments/assets/5e66e3cc-ceab-4ea4-b6d8-e23756805b90

## Installation
Place the `splashd` files under:

```text
/media/fat/splashd
```

Then run:

```sh
/media/fat/splashd/install.sh
```

The installer:

- Enables `log_file_entry=1` in the `/media/fat/MiSTer.ini` configuration file.
- Creates `/media/fat/splashd/wallpapers`.
- Adds a managed startup block to `/media/fat/linux/user-startup.sh`.
- Places the startup block after Pixelcade if Pixelcade is detected.
- Starts or restarts `splashd`.

## Recommended MiSTer Setup
- Keep a default menu image at `/media/fat/menu.png`, `.jpg`, or `.jpeg`.
- Use F1 in MiSTer until the menu is set to display the custom menu
  image/wallpaper mode.
- Set `osd_timeout` in the `/media/fat/MiSTer.ini` configuration file to a
  large value, such as `180` or higher, to avoid frequent idle repaint
  transitions while browsing.

## Matching
For `.mra` and `.mgl` menu files, `splashd` first resolves the ROM identifier:

- `.mra` files use `<setname>`.
- `.mgl` files use the referenced ROM filename stem. (`.neo` files are
  preferred for Neo Geo MVS/AES.)
- For normal menu entries, it checks `/tmp/FULLPATH` first, then
`/tmp/CURRENTPATH`.
- Root menu entries with an empty `/tmp/FULLPATH` are resolved by checking
  `/media/fat/<visible name>.mra` and `/media/fat/<visible name>.mgl`.

`.mra` example:
`Alien vs. Predator (Euro 940520).mra` with
`<setname>avsp</setname>` checks for:

```text
/media/fat/splashd/wallpapers/avsp.png
/media/fat/splashd/wallpapers/avsp.jpg
/media/fat/splashd/wallpapers/avsp.jpeg
```

`.mgl` example:
`Chrono Trigger.mgl` with
`<file delay="2" type="f" index="0" path="Chrono Trigger (USA).sfc"/>`
checks for:

```text
/media/fat/splashd/wallpapers/Chrono Trigger (USA).png
/media/fat/splashd/wallpapers/Chrono Trigger (USA).jpg
/media/fat/splashd/wallpapers/Chrono Trigger (USA).jpeg
```

If no ROM-named image exists, `splashd` falls back to menu-entry basenames.
The above examples would respectively look for:

```text
/media/fat/splashd/wallpapers/Alien vs. Predator (Euro 940520).png
/media/fat/splashd/wallpapers/Alien vs. Predator (Euro 940520).jpg
/media/fat/splashd/wallpapers/Alien vs. Predator (Euro 940520).jpeg
/media/fat/splashd/wallpapers/Alien vs. Predator.png
/media/fat/splashd/wallpapers/Alien vs. Predator.jpg
/media/fat/splashd/wallpapers/Alien vs. Predator.jpeg
```

and

```text
/media/fat/splashd/wallpapers/Chrono Trigger.png
/media/fat/splashd/wallpapers/Chrono Trigger.jpg
/media/fat/splashd/wallpapers/Chrono Trigger.jpeg
```

Directory menu entries are matched by visible basename too. MiSTer menu
directories usually start with `_`, so an active `_Arcade` entry first matches
`_Arcade.*`, then falls back to `Arcade.*`.

Before fuzzy matching, `splashd` also tries a normalized title with parentheses
or bracket suffixes removed. After exact filename checks fail, matching ignores
punctuation and spacing differences. For example, `_D.D. Crew` or
`D.D. Crew (Japan, 2 Players)` can match `D. D. Crew.png`.

If no matching artwork exists, `splashd` displays the default menu image. If no
default image can be decoded, it clears the framebuffer to black.

## Manual Run
For foreground testing:

```sh
/media/fat/splashd/splashd --foreground
```

Process the current MiSTer menu state once:

```sh
/media/fat/splashd/splashd --foreground --once
```

Default runtime paths:

```text
--wallpaper-dir /media/fat/splashd/wallpapers
--default-image /media/fat/menu.png, then /media/fat/menu.jpg, then /media/fat/menu.jpeg
--menu-root /media/fat
--mode-file /sys/module/MiSTer_fb/parameters/mode
--watch-dir /tmp
```

## Note
This application was partially developed using OpenAI's Codex tools.
