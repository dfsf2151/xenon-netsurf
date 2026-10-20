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
| A | Select / open; on a text field, open the keyboard |
| B | Back |
| X | Menu |
| Y | Address / search bar |
| Left stick / D-pad | Move pointer |
| Right stick | Scroll smoothly |
| LB / RB | Scroll a screen |
| BACK | Speed Dial |
| START | Quit |

The menu contains: New tab, Reload page, Home, Close tab, Play demo.wav, Play
demo.mp3, Sound test, Help, Quit.

## Typing into pages

Press A on a text field: the keyboard opens with the field's current text.
Type with A, delete with X, and press Done (or Y) to put the text in the field;
the form is not sent. Password fields start empty and show dots. Send the form
with the page's own button. Cookies are kept in `Cookies` beside the XEX, so
sites stay signed in between runs.

## Search and address bar

Press Y, type a URL or a search phrase, and press A. Plain text goes to
DuckDuckGo Lite (`https://lite.duckduckgo.com/lite/?q=...`). HTTPS is checked against the bundled Mozilla CA list.

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
- Modern web apps (YouTube, the Instagram feed, ES2015+ frameworks).
  JavaScript is ES5 only, through Duktape. A script gets 5 seconds.
- SVG.
- Arabic and other scripts outside Latin, Greek and Cyrillic.
- Instagram sign-in: Instagram ignores the plain login form. Public post and
  reel links work without an account.
- Robust HTTP timeouts and cancellation.

## Troubleshooting

- **Page stays blank:** the network may still be loading images. Wikipedia can
  take 20–25 seconds on a slow link.
- **Certificate error:** check that `cacert.pem` is beside the XEX and that the
  console clock is plausible; the browser takes time from the first HTTP Date
  header but needs a sane starting point.
- **Audio menu does nothing:** make sure `demo.wav`/`demo.mp3` are beside the
  XEX and that the console's audio output is configured.
- **Emulator log:** `~/.local/share/Xenia/cache/default.log`. Put an empty
  `debug.txt` beside the XEX for NetSurf's verbose log.
- **A site is slow or broken with scripts:** list its domain in `nojs.txt`
  beside the XEX (one per line) to load it with scripts off.
