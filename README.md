# FarCompare

Side-by-side file comparison plugin for [Far Manager](https://www.farmanager.com/) 3.x (x64).

![FarCompare](https://img.shields.io/badge/Far_Manager-plugin-blue)

## Features

- Side-by-side diff view in a single editor window
- Color-coded differences:
  - **Yellow** — modified lines (content differs)
  - **Red** — deleted lines (only in left/old file)
  - **Green** — added lines (only in right/new file)
  - **Gray** — empty placeholder on the opposite side
  - **Blue** — center separator column
- Uses the [ComparePlus](https://github.com/pnedev/comparePlus) Myers diff engine (Hirschberg linear space optimization, swap check, boundary shift, diff combine)
- Keyboard shortcut: `Ctrl+Alt+C` (via included Lua macro)
- Works with FarColorer syntax highlighting enabled

## Usage

1. Navigate to a file on the **left panel** and another on the **right panel**
   - Or select two files on the same panel
2. Press `Ctrl+Alt+C` (or F11 → FarCompare)
3. The diff opens in the editor with color-coded differences

Press `Esc` to close the comparison.

## Installation

Copy these files into `%FARHOME%\Plugins\FarCompare\`:
- `FarCompare.dll`
- `FarCompEng.lng`
- `FarCompHun.lng` (optional)

For the keyboard shortcut, copy `FarCompare_keys.lua` to:
`%FARPROFILE%\Macros\scripts\`

## Building

### Prerequisites
- Microsoft Visual Studio 2022 (BuildTools, Community, or Professional)
- Git (for submodules)

### Steps

```
git clone --recursive https://github.com/YOUR_USERNAME/FarCompare.git
cd FarCompare
build.bat
```

Output goes to `build\` directory.

## Project Structure

```
FarCompare/
├── src/
│   ├── FarCompare.cpp      # Main plugin source
│   ├── FarCompare.def      # DLL exports
│   ├── guid.hpp            # Plugin GUIDs
│   ├── version.hpp         # Version info
│   ├── FarCompareLng.hpp   # Language string IDs
│   ├── FarCompEng.lng      # English strings
│   └── FarCompHun.lng      # Hungarian strings
├── macros/
│   └── FarCompare_keys.lua # Ctrl+Alt+C shortcut
├── external/
│   ├── FarManager/         # Git submodule — Far Manager SDK
│   └── comparePlus/        # Git submodule — diff engine
├── build.bat               # Build script
└── README.md
```

## Credits

- Diff engine from [ComparePlus](https://github.com/pnedev/comparePlus) by Pavel Nedev (GPL-3.0)
- Myers diff algorithm: E. Myers, "An O(ND) Difference Algorithm and Its Variations", Algorithmica 1, 2 (1986)
- [Far Manager](https://github.com/FarGroup/FarManager) plugin SDK

## License

GPL-3.0 (due to ComparePlus diff engine dependency)
