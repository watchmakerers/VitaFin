# VitaFin — a Jellyfin client for PS Vita

A native homebrew Jellyfin client styled after the PS Vita **Settings** app (firmware 3.x):

- the Settings green gradient background (colours sampled from a real screenshot), with a faint page emblem
- centred page titles, big white list text, etched row separators, the glossy white press/focus bar
- **page-stack animation**: opening a page swings the current one back in 3D into a faint ghost on the
  left while the new page slides in; going back swings it forward again
- rows cascade in as content loads, focus glides, rows and buttons animate on press, overscroll bounce
- the round, partly off-screen **back** (↩) and **options** (…) corner buttons
- the system font rasterised natively at every size (sharp — vita2d's own path scales one bitmap size)
- 4× MSAA on all vector chrome
- the **real** system keyboard (SceImeDialog) and message dialogs (SceMsgDialog)
- colour themes: Green (Settings), Blue, Purple, Orange, Graphite — options menu on the home page

All chrome is drawn procedurally. No Sony assets are bundled.

## Features

- Sign in with server address, user name and password (token stored in `ux0:data/VitaFin/config.txt`)
- Home: Continue Watching, Next Up, and all your libraries
- Movie / TV libraries as poster grids; music as album grids; folders as lists
- Series → Seasons → Episodes, with watched checkmarks, unwatched counts and resume bars
- Detail page with backdrop, poster, metadata, overview, Resume / From Beginning
- Sorting (name, date added, release date, rating, ascending/descending); mark watched / unwatched
- Playback on the hardware decoders — no SceAvPlayer:
  - Jellyfin streams **MPEG-TS** (H.264 ≤ 720p + AAC stereo). It copies the H.264 untouched when the
    source already fits, otherwise transcodes.
  - VitaFin demuxes the TS itself, decodes video with **sceAvcdec** straight into textures and audio
    with **sceAudiodec**; audio is the master clock.
  - Seeking restarts the stream at the new position on the server.
  - Music streams as raw ADTS AAC through the same engine.
- **Audio tracks** (△ or … in the player → Audio): every track on the file, e.g. English 5.1 / Japanese stereo /
  commentary. The default follows your Jellyfin audio-language preference. Switching restarts the stream at the
  current position; the server downmixes to stereo AAC as needed.
- **Subtitles** (△ or … → Subtitles):
  - text tracks (SRT, ASS/SSA, VTT, embedded text) are fetched from Jellyfin as SRT and drawn by VitaFin:
    sharp, outlined, moved above the controls when they show; the stream stays stream-copied
  - picture tracks (PGS, VobSub), marked "(image)", are burned in by the server (switching restarts the stream)
  - a forced track, or the server's default (your Jellyfin subtitle preferences), is picked automatically
- Progress is reported to the server (start / every 10 s / stop), so resume works across devices
- A log is written to `ux0:data/VitaFin/log.txt` (stream format, decoder errors with codes)

## Controls

| Button | Lists / grids | Player |
| --- | --- | --- |
| D-pad / left stick | move focus | ← → seek 10 s |
| ✕ (○ on Japanese units) | open | play / pause |
| ○ | back | exit player |
| △ | options menu | — |
| L / R | page up / down | seek 60 s |
| START | sign in (login screen) | play / pause |
| Touch | drag to scroll (with inertia), tap to open | tap to show controls, drag the seek bar |

## Building

You need [VitaSDK](https://vitasdk.org). Install it once:

```sh
git clone https://github.com/vitasdk/vdpm ~/src/vdpm
cd ~/src/vdpm && ./bootstrap-vitasdk.sh --install-dir ~/vitasdk
export VITASDK=~/vitasdk PATH=$VITASDK/bin:$PATH     # fish: set -x VITASDK ~/vitasdk; fish_add_path $VITASDK/bin
vdpm install libvita2d curl openssl zlib libpng libjpeg-turbo freetype
```

Then build:

```sh
cd ~/Projects/VitaFin
cmake -S . -B build && cmake --build build -j
# -> build/VitaFin.vpk
```

Install the VPK with VitaShell. Requires a Vita on HENkaku/h-encore/Enso (3.60–3.74).

`tools/make_assets.py` regenerates the bubble icon and LiveArea art (8-bit indexed PNGs, as the Vita requires).

## Previewing the UI on a PC

`tools/preview/run.sh [theme]` compiles the real UI code for Linux with a software stand-in for vita2d and
a fake Jellyfin server, scripts some button presses, and writes screenshots (home, focus, push/pop
transitions, poster grid, detail page, options menu) to `tools/preview/shots/`. Needs libcurl, libjpeg,
FreeType, Pillow and Noto Sans; text uses Noto Sans there instead of the Vita system font.

## Layout

| File | What it does |
| --- | --- |
| `src/ui.c` | Drawing kit: 3D page transform, gradients, rounded rects, glow, vector icons, status bar, Settings rows, corner buttons, scroller with inertia + cascade, options menu, themes |
| `src/font.c` | System font (ScePvf) rasterised per pixel size into a glyph atlas; Japanese fallback |
| `src/screens.c` | Login, Home, Browse (list/poster/album/track) and Detail pages; page stack + swing transitions; async loading |
| `src/media.c` | Playback engine: TS/ADTS demux, sceAvcdec video thread, sceAudiodec audio thread, A/V clock |
| `src/player.c` | Player UI: overlay, seek bar, transport buttons, subtitle menu, progress reporting |
| `src/subs.c` | SRT parser and outlined subtitle rendering |
| `src/sfx.c` | UI sounds: the console's own (decoded from vs0 at runtime), user WAVs, or synthesised |
| `src/http.c` | libcurl wrapper and streaming reader (ranged block cache / ring buffer) |
| `src/jellyfin.c` | Jellyfin REST API: auth, views, items, images, stream URLs, progress |
| `src/images.c` | Async thumbnail loader with LRU eviction and fade-in |
| `src/ime.c` | System keyboard and message dialogs |
| `src/json.c` | Small JSON parser |

## Known limits

- HTTPS certificates aren't verified (homebrew has no CA store). LAN `http://` is recommended.
- Sources above 720p are always transcoded by the server (the decoder is set up for 1280×720 max).

## License

VitaFin is released under the [MIT License](LICENSE).

No Sony assets are included. The system UI sounds and fonts are read from the console's own firmware at runtime.
