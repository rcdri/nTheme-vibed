# nTheme

A modern theme manager for TI-Nspire calculators running ndless

Alpha release, see below for specific model support information.

| Themed Home            | Settings               | Dark Mode               |
| ---------------------- | ---------------------- | ---------------------- |
| ![home](screenshots/home.png) | ![settings](screenshots/settings.png) | ![dark](screenshots/dark_scratchpad.png) |


## Supported Calculators
Note: OS 6.4.0.74/ndless r2022
- TI-Nspire CX II(-T)
- TI-Nspire CX II(-T/-C) CAS

## Features

- Dark mode (experimental, not perfect)
- Accent colors
- Icons follow both: OS icons are recolored for dark mode, and their blue details take the accent color
- Home screen wallpaper from PNG or JPEG, with crop, fit and fill scaling
- Glass (translucent) home screen text background
- Custom or blank home screen title (via `ndless/theme.cfg.tns`)
- Native settings dialog under Home > 5 > Style

## Installation and use

1. Copy `nTheme.tns` anywhere on your calculator
2. Run the program, the settings dialog will open automatically
3. To edit settings again, look under Home > 5 > Style

Note: Transfer `nTheme.tns` to `ndless/startup` if you'd like it to install itself alongside ndless automatically. Started that way, the wallpaper is decoded the first time the home screen is drawn after the install (memory is too constrained to decode it any earlier); if that fails, the reason is shown the next time you open Home > 5 > Style.

### Wallpaper support

Put PNG or JPEG files into `ndless/wallpapers/`. The file name must end in `.tns`, for example `wallpaper.png.tns`. Pictures up to 2 megapixels are guaranteed to be decoded, with mixed results between 2-4 MP. The home screen area is 320x217 pixels.

Tips: Try to keep images small and crop to ~3:2 for best results

## Known limitations

Dark mode isn't perfect. With a light accent (yellow), the light dark-mode icons are hard to see on the highlighted row. There may be bugs, including unreadable UI elements or issues involving Python and Lua apps. Open documents will retain some old colors until reopened.

### Hard-coded colors in dark mode

Some OS screens draw with colors of their own instead of the theme table. In dark mode nTheme rewrites those at
draw time: light grays get the table's lightness flip (Graphs lined grid, Data & Statistics variable bars, Vernier
DataQuest panels), and colors too dark to read on the dark background are lightened to 3:1 contrast. Colors the
theme sets are never touched.

3D graphing draws its own pixels, so nTheme recolors the 3D view right after the OS copies it to the screen: in
dark mode its white background turns dark and its black box, axes and labels turn light (CX II-T; the other
models need the `view_3d` address from `utils/hookfinder.py`).

### Graph colors

With an accent other than blue, the default function colors (f1-f6: blue, red, black, magenta, green, orange)
become a palette built from the accent: f1 is the accent itself, the others are nearby hues (±30°, ±60°) and
lighter or deeper shades of it, so the graphs look related to the accent and stay apart from each other. This
applies to 2D and 3D, in light and dark mode, and the graph color picker swatches change the same way. Colors picked
from the rest of the palette (brown, light blue, yellow, grays) keep their hue. Graph colors are kept readable on
the background: lifted to 3:1 contrast in dark mode, darkened to 3:1 on white in light mode when the accent made
them lighter. With the blue accent, graphs keep TI's colors.

### Icons

The OS draws its color icons straight from the `iconsColor.res` data it keeps in a heap registry, so nTheme
rewrites those pixel planes from a stock copy (about 1 MB) whenever the theme changes: about half a second.
Started from `ndless/startup`, this happens at the first home screen paint. Gray parts get the dark mode
lightness curve, gray drawn on color (app tiles, the "!" on warning signs) and the white title bar glyphs
stay, and TI's UI blue moves to the accent. Plot color swatches change exactly as the colors they stand for
are drawn. Light mode with the blue
accent is the stock icons, byte for byte. `utils/iconsim.c` runs the same code on a PC against `.res` files.

## Please add support for [insert CX variant here] on [insert OS here]!!!

Take a snapshot (RAM state) of your variant and OS version under firebird with ndless installed while on the home screen, then run `utils/hookfinder.py` against it.
After that, open a GitHub issue with the .txt file it spits out, and I'll see what I can do from there.

## Build
1. install ndless SDK (ndless-sdk/bin on PATH)
2. run `make` at the project root

## Roadmap

- Theme bundles (cfg, wallpapers, icons, and colors all in a single file for easy sharing)
- Custom home screen icons (I have a working PoC for this)
- Custom status bar backgrounds
- Advanced theme editor (manual editing of the color table)
- Theme gallery webpage
