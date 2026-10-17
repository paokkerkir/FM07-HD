# FM07HD — display mod for Football Manager 2007 (7.0.2)

Window size options, freely resizable/maximisable window (no more 1600x1200 cap), borderless
fullscreen, and UI scaling zoom up to 4x with text, boxes and images drawn sharply at full resolution.

Requires fm.exe **7.0.2** (the latest official patch). Any other version is detected and left
untouched.

## Install

Copy **`dsound.dll`** and **`fm07mod.ini`** into the Football Manager 2007 folder (next to
`fm.exe`). Start the game as usual — shortcut, double-click, anything. No launcher needed.

To uninstall, delete `dsound.dll` (and `fm07mod.ini`).

## Optional: match banner fix for large windows

FM2007 sizes the foul/card/offside banner over the 2D pitch as 3/5 of the pitch width; on wider
windows its graphics tile (the banner) appears 2-3 times. `data\panels\match notification.xml`
here gives it its designed fixed width (486) centred. Copy it over the game's
`data\panels\match notification.xml` (keep a backup of the original).


## Use

- **Ctrl+Alt+=** / **Ctrl+Alt+-**: zoom in / out, **Ctrl+Alt+0**: 100%
- Resize, move or maximise the window as you like. Window size, position, maximised state,
  windowed/fullscreen mode and zoom are saved to `fm07mod.ini` automatically and restored on
  the next start.
