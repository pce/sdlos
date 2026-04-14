# clima — required fonts

Place the following font files in this directory before building.
The CMake build (`DATA_DIR`) copies this entire `data/` tree next to the
binary, where `loadAppFonts` picks them up at runtime.

---

## 1. Ubuntu (primary typeface)

Download from the official Ubuntu font page:

    https://design.ubuntu.com/font

Direct zip link (Ubuntu Font Family 0.83):

    https://assets.ubuntu.com/v1/0cef8205-ubuntu-font-family-0.83.zip

Extract and copy into this directory:

    Ubuntu-Regular.ttf
    Ubuntu-Bold.ttf      ← optional; used for font-weight:bold if present
    Ubuntu-Italic.ttf    ← optional

Or, if you prefer Ubuntu Sans (the 2023 variable-font redesign on Google Fonts):

    https://fonts.google.com/specimen/Ubuntu+Sans

Save as `UbuntuSans-Regular.ttf` (the loader tries this name as a fallback
if `Ubuntu-Regular.ttf` is not found).

---

## 2. Twemoji Mozilla (emoji fallback)

Provides colour emoji (☀️  🌧️  ❄️  🌨️  ⛅) via COLR/CPAL tables.
SDL3_ttf chains it behind the primary font through TTF_AddFallbackFont so
any missing glyph falls through automatically.

Download the latest release from:

    https://github.com/mozilla/twemoji-colr/releases

Look for `TwemojiMozilla.ttf` in the release assets and copy it here.

---

## Expected directory listing after setup

    data/fonts/
        Ubuntu-Regular.ttf       ← required (primary)
        Ubuntu-Bold.ttf          ← optional
        TwemojiMozilla.ttf       ← required (emoji fallback)
        README.md                ← this file

---

## How fonts are loaded

1. `loadAppFonts()` in `src/jade_host.cc` scans `<binary>/data/fonts/` for
   the primary font candidates in priority order:
   Ubuntu-Regular → UbuntuSans-Regular → InterVariable → Inter-Regular →
   Roboto-Regular → LiberationSans-Regular.

2. If a primary font is loaded and `TwemojiMozilla.ttf` is present in the
   same directory, it is registered as a fallback via `TTF_AddFallbackFont`.

3. Apps can also declare fonts declaratively on the jade root node:

       col#my-root(_font="data/fonts/Ubuntu-Regular.ttf"
                   _font_size="16"
                   _emoji_font="data/fonts/TwemojiMozilla.ttf")

   These `_font` / `_emoji_font` attributes are processed in step 8 of
   `loadScene()` and override whatever `loadAppFonts` chose.
