# arcade-root-sync

`arcade-root-sync.sh` is a standalone post-`update_all` utility for MiSTer.
It is separate from `splashd`.

Deploy it to:

```sh
/media/fat/Scripts/arcade-root-sync.sh
```

Preview changes:

```sh
/media/fat/Scripts/arcade-root-sync.sh --dry-run --verbose
```

Apply changes:

```sh
/media/fat/Scripts/arcade-root-sync.sh
```

The script reconciles root `/media/fat/*.mra` and `/media/fat/*.mgl` entries:

- Creates root symlinks for top-level `/media/fat/_Arcade/*.mra` files.
- Uses a local 2-player alternative for 3+ player arcade games when one exists.
- Generates Neo Geo `.mgl` files from `/media/fat/games/NEOGEO/*.neo`.
- Uses full Neo Geo titles from `/media/fat/games/NEOGEO/romsets.xml` when available.
- Removes root `.mra` and `.mgl` entries that are no longer part of the desired set.

It downloads MiSTer's `ArcadeDatabase.csv` into
`/media/fat/Scripts/.config/arcade-root-sync/` and falls back to the cached copy
if the network is unavailable.
