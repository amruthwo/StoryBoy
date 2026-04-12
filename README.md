<div align="center">
  <img src="images/StoryBoy_Icon.png" width="120" alt="StoryBoy icon" />
  <h1>StoryBoy</h1>
  <p>Audiobook player for SpruceOS handheld devices</p>
</div>

---

![StoryBoy screenshot](images/sb_full1.png)

**StoryBoy** is a native audiobook player for SpruceOS (and now OnionOS) devices. It has a three-level media browser (series → audiobooks → files), embedded cover art or online cover art fetching, listening history with automatic position saving/resume, a clean fullscreen playback UI with OSD, variable playback speed, and sleep timer. It's written in C around FFmpeg and SDL2.

---

## Supported Devices

| Device | Notes |
|---|---|
| Miyoo A30 | 640×480, ARMv7. Works great. |
| Miyoo Flip V1/V2 | 640×480, AArch64. Works great. |
| Miyoo Mini Flip (V4) | 752×560, ARMv7. Audio via SigmaStar bridge. |
| Miyoo Mini V2/V3 | 640×480, ARMv7. Video works; audio broken, currently. |
| TrimUI Brick / Hammer | 1024×768, AArch64. Works great. |
| TrimUI Smart Pro / Pro S | 1280x720, AArch64. Works great (I think). |

---

## Installation

1. Download the latest release zip from the [Releases](../../releases) page.
2. Extract the zip to your SD card — make sure you have a `/mnt/SDCARD/App/StoryBoy/` folder.
3. Launch from the SpruceOS app menu.

On first launch, StoryBoy scans your media folders and builds its library. Make sure your audiobook files are in `/mnt/SDCARD/Media/Audiobooks/`.  StoryBoy uses folders to define audiobooks, so each book will need its own folder, but they can be nested by series.  This was a compromise in order to support audiobooks that are split up between multiple .mp3 files.   

EXAMPLE: 
```
Audiobooks/  
│
└───Dungeon Crawler Carl/
│   │   cover.png
│   │
│   └───[Book 1] Dungeon Crawler Carl/
│       │   Dungeon Crawler Carl [Book 1].m4b
│       │   cover.png
|
│   └───[Book 2] Carl's Doomsday Scenario/
│       │   Carl's Doomsday Scenario [Book 2].m4b
│       │   cover.png
|
│   └───[Book 3] The Dungeon Anarchist's Cookbook/
│       │   The Dungeon Anarchist's Cookbook [Book 3].m4b
│       │   cover.png
│   
└───Animal Farm/
|   │   Animal Farm - George Orwell.mp3
|   │   cover.jpg
|
└───Legion - The Many Lives of Stephen Leeds/
|   │   Brandon.Sanderson-Legion_Track1.mp3
|   │   Brandon.Sanderson-Legion_Track2.mp3
|   │   Brandon.Sanderson-Legion_Track3.mp3
|   │   Brandon.Sanderson-Legion_Track4.mp3
|   │   cover.png
```

---

## Features

- **File browser** — three-level hierarchy (series → audiobooks → files) with folder grid and cover art
- **Cover art** — automatic use of embedded art, or by downloading from Open Library
- **Automatic mosaic** — For series, a mosaic is made by tiling book covers
- **Playback** — Chapter indicators, playback speed (1x, 1.25x, 1.5x, 2x)
- **Sleep timer** — Sleep timer (10m, 30m, 1h, 2h)
- **Screensaver** — Black screen and button lock
- **Seek** — ±10s / ±60s / or by chapter
- **Listen history** — remembers where you left off across all audiobooks
- **Themes** — ten color themes, cycle with R1 in the browser
- **OSD** — progress bar, current time, title, volume
- **Status bar** — clock, title, WiFi signal, battery level

---

## Basic Usage

### Navigation

- **D-pad** — navigate the file browser
- **A** — open folder / play audiobook
- **B** — back
- **Hold D-pad up/down** — fast scroll through long lists
- **Hold MENU** — show Controls Reference

### Playback controls

| Button | Action |
|---|---|
| D-pad left/right | Seek ±10 seconds |
| D-pad up/down | Brightness ± |
| A | Play / Pause |
| B | Back to browser |
| X | Cycle sleep timer  |
| Y | Double-press for screensaver & button lock |
| L1 | Seek -60 seconds |
| R1 | Seek +60 seconds |
| L2 | Previous chapter in current book |
| R2 | Next chapter in current book |
| START | Toggle playback speed |
| SELECT | Reset playback speed to 1x |
| Volume up/down | Adjust volume |

### Browser controls

| Button | Action |
|---|---|
| D-pad | Navigate |
| A | Open folder / play audiobook |
| B | Back (press twice at top level to exit) |
| X | Open listening history |
| Y | Manage cover art for selected audiobook |
| SELECT | Cycle view layout |
| R1 | Cycle color theme |

### Cover art

Press **Y** on any audiobook in the browser folder grid to manage cover art & scrape cover art from Open Library. 

(You can also add covers manually by placing a `cover.jpg` or `cover.png` in the folder with your audiobook .m4b or .mp3 files.)

---

## Supported Formats

Audio: M4B, MP3

All decoding is software.

---

Enjoy! :)
