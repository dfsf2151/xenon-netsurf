# Xenon NetSurf — quick guide

**Experimental build. Tested in Xenia only. Not complete.**

## Install

1. Extract `ns360.zip`.
2. Copy the `ns360` folder to a FAT32 USB drive, for example
   `Games/ns360/`.
3. Launch `default.xex` from Aurora, F3 or XeLL.
4. Keep `res/`, `cacert.pem`, the HTML files and the demo audio beside the XEX.

No Xbox LIVE account is needed. The browser never calls LIVE interfaces. Do not
sign in on a BadUpdate console.

## Controls

| Input | Action |
|---|---|
| A | Select / open |
| B | Back |
| X | Options menu |
| Y | Address / search bar |
| Left stick | Move pointer |
| LB / RB | Scroll page |
| BACK | Home |
| START | Quit |

The Options menu contains: New Tab, Reload Page, Home, Close Tab, Play
demo.wav, Play demo.mp3, Sound Test, Help, Quit.

## Search and address bar

Press Y, type a URL or a search phrase, and press A. Plain text goes to
DuckDuckGo Lite (`https://lite.duckduckgo.com/lite/?q=...`), which works without
JavaScript. HTTPS is checked against the bundled Mozilla CA list.

## Tabs

Up to six tabs. Each tab keeps its own scroll position. Use the tab strip or
the Options menu to switch and close tabs.

## Audio

Local WAV (PCM 16-bit) and MP3 files play through the Xbox 360 XAudio output at
48 kHz. Stereo is placed on the front pair.

- Use **Options → Play demo.wav / Play demo.mp3 / Sound Test**, or
- type a direct local address such as `file:///game:/demo.wav`.

Limits: 32 MiB per file; no AAC, no HTTP audio streams, no automatic sounds
from web pages.

## What is missing

- Video playback and YouTube.
- AAC audio.
- JavaScript.
- SVG.
- Arabic and other non-ASCII scripts (the font is ASCII; unknown characters
  appear as `?`).
- Robust HTTP timeouts and cancellation.

## Troubleshooting

- **Page stays blank:** the network may still be loading images. Wikipedia can
  take 20–25 seconds on a slow link.
- **Certificate error:** check that `cacert.pem` is beside the XEX and that the
  console clock is plausible; the browser takes time from the first HTTP Date
  header but needs a sane starting point.
- **Audio menu does nothing:** make sure `demo.wav`/`demo.mp3` are beside the
  XEX and that the console's audio output is configured.
- **Emulator log:** `~/.local/share/Xenia/cache/default.log`.
