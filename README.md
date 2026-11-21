# FilmRender

A cross-platform **C++17 + wxWidgets** desktop application that replaces [render.sh](render.sh) with a full graphical interface—configurable render options, an interactive video queue, built-in `yt-dlp` MP3 queue downloading, automatic `tools/` binary management, and a live side-by-side output log.

---

## Features

### 1. Folders & Paths
- **Film Folder**: Select or drag-and-drop a folder containing `.mp4` clips.
- **MP3 Audio Folder**: Select or drag-and-drop a folder containing `.mp3` background tracks (auto-detects `./ncs` if present on first launch).
- **Output File**: Defaults to `final_output.mp4` inside the Film Folder, or choose any custom destination path.
- **FFmpeg Binary**: Uses `ffmpeg` from `tools/` or `PATH`, or lets you browse to a custom executable.
- Settings are automatically persisted across sessions using `wxConfig`.

### 2. Render Options
- **Speed Multiplier**: Default `3.70x` with live timestamp multiplier preview (`TS Mult = 1 / speed`, formatted using C locale for FFmpeg `setts`).
- **FFmpeg Log Level**: `error`, `warning` (default), `info`, or `verbose`.
- **Sanitize filenames (`ASCII a-zA-Z0-9`)**: Strips non-ASCII/special characters from `.mp4` clips with `_1`, `_2` collision suffixes while skipping the target output file.
- **Include MP3 audio**: Toggle background audio muxing (`-an` when disabled).
- **Shuffle MP3s**: Randomizes the merged `.mp3` playlist order before concatenation.
- **Loop MP3 until video ends**: Passes `-stream_loop -1` before the merged audio input so shorter MP3 collections loop seamlessly for the entire sped-up video duration.
- **Cut to shortest (`-shortest`)** and **Overwrite existing (`-y`)**.

### 3. Video Queue (Order & Selection)
- Lists all `.mp4` files in the Film Folder (excluding the output file) with human-readable sizes and totals.
- Check/uncheck clips to include or exclude them.
- **Move Up** / **Move Down** to rearrange clip order manually.
- **Natural Sort** (`sort -V` style, e.g., `Tp2` before `Tp10`) and **A-Z Sort**.
- **Sanitize Names** button to rename clips immediately without starting a render.

### 4. Tools Status & Binary Installer (`tools/`)
- Displays live availability for **`ffmpeg`**, **`yt-dlp`**, and **`bun`** (JavaScript runtime for `yt-dlp`) across `tools/` and your environment `PATH`.
- **Check Tools**: Re-scans `tools/` and `PATH` and logs resolved paths.
- **Download Missing -> tools/**: Downloads and extracts missing binaries from their official releases directly into `tools/` (Linux `x64`/`arm64`, Windows `x64`/`arm64`, macOS `x64`/`arm64`).

### 5. MP3 Downloader Queue (`yt-dlp`)
Click **Download MP3s (yt-dlp)...** to open the downloader queue dialog:
- Paste YouTube video or playlist URLs and click **Add URL to Queue** (or press **Enter**).
- Fetches metadata via `yt-dlp --flat-playlist --dump-json` and appends items to the queue while skipping duplicates (allowing you to merge multiple URLs and playlists).
- Each queue card displays:
  - Thumbnail (loaded asynchronously)
  - Video title, channel name, and video ID
  - Editable output filename textbox (`.mp3`)
  - Individual progress bar and status label
- Supports **Expand playlist (`--yes-playlist`)**, **Skip existing (`--no-overwrites`)**, **Clear Completed**, **Clear All**, **Download Queue**, **Stop**, and updating/downloading `yt-dlp`, `bun`, and `ffmpeg` into `tools/`.

### 6. Two-Column Layout & Live Output Log
- Left column holds configuration, options, tools status, and the video queue; the full-height right column holds the live progress gauge, FFmpeg stats line, and monospace **Output Log**.
- Includes **Preview Command**, **Open Output Folder**, **Clear Log**, and **Save Log...**, plus safe cancellation via **Stop**.

---

## Render Pipeline

When you click **Render Video**, FilmRender runs the same stream-copy workflow as `render.sh` asynchronously:
1. *(Optional)* Sanitizes `.mp4` filenames to ASCII `a-zA-Z0-9` while preserving your chosen queue order.
2. Writes a temporary FFmpeg concat list from the checked & ordered video clips.
3. *(If audio is enabled)* Scans `.mp3` files from the MP3 Audio Folder, optionally shuffles them, and merges them via `ffmpeg -f concat -safe 0 -i ... -vn -c:a copy`.
4. Concatenates, speeds up video timestamps (`-bsf:v setts=dts=DTS*<TS_MULT>:pts=PTS*<TS_MULT>`), optionally loops the merged audio (`-stream_loop -1`), and muxes with `-c:v copy -c:a copy` (zero video re-encoding).

---

## Requirements

- C++17 compiler (GCC, Clang, or MSVC)
- CMake 3.16+
- wxWidgets 3.0+ or 3.2+ (`core` and `base` components)
- Runtime tools (either on `PATH` or downloaded into `tools/` from inside the app):
  - `ffmpeg` (required for rendering and MP3 extraction)
  - `yt-dlp` and `bun` (optional; used by the MP3 Downloader dialog)

---

## Building from Source

### Linux (Ubuntu / Debian)
```bash
sudo apt install build-essential cmake libwxgtk3.0-gtk3-dev ffmpeg
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/FilmRender
```

### macOS (Homebrew)
```bash
brew install cmake wxwidgets ffmpeg
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
open build/FilmRender.app
```

### Windows (MSVC + vcpkg)
```powershell
vcpkg install wxwidgets:x64-windows
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\FilmRender.exe
```

### Windows (MSYS2 / UCRT64)
```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-wxwidgets3.3-msw
WX_CFG=$(ls /ucrt64/bin/wx-config* | sort -V | tail -n 1)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DwxWidgets_CONFIG_EXECUTABLE="$WX_CFG"
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/FilmRender.exe
```

---

## GitHub Actions (Windows x86_64)

The workflow at [.github/workflows/build-windows.yml](.github/workflows/build-windows.yml) builds `FilmRender.exe` on `windows-latest` using MSYS2 (`UCRT64` + wxWidgets MSW), runs `ctest`, bundles required UCRT64/wxWidgets DLLs via `objdump`, and uploads the `FilmRender-windows-x86_64` artifact.

---

## Project Structure

```text
.
├── .github/workflows/
│   └── build-windows.yml       # Windows x86_64 CI build & portable artifact
├── src/
│   ├── main.cpp                # wxApp entry point & image handler init
│   ├── MainFrame.h/.cpp        # Main two-column GUI window
│   ├── Mp3DownloaderDialog.h/.cpp  # yt-dlp queue dialog + tool downloader
│   ├── MediaUtils.h/.cpp       # Filename sanitization, natural sort, concat helpers
│   ├── ProcessRunner.h/.cpp    # Cross-platform async subprocess runner
│   ├── RenderEngine.h/.cpp     # FFmpeg merge/speedup/mux pipeline
│   ├── ToolManager.h/.cpp      # PATH & tools/ detection + binary downloader
│   └── YtDlpUtils.h/.cpp       # yt-dlp JSON metadata & progress parsing
├── tests/
│   └── test_core.cpp           # Unit & FFmpeg integration tests (CTest)
├── CMakeLists.txt
├── render.sh                   # Original bash script
├── yt.sh                       # Original yt-dlp helper script
└── README.md
```
