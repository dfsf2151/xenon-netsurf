# OpenH264 benchmark (not linked into the browser)

`h264bsd`, the decoder originally used by NetSurf's image/video path, only
handles H.264 Baseline. YouTube's 360p stream and most modern MP4 files use Main
or High profile with CABAC and B-frames. OpenH264 2.7.0 was cross-built for
Xenon to measure the alternative.

## Port notes

- Decoder and common static libraries only, `USE_ASM=No`, `ARCH=ppc`, one
  thread, BSD-2-Clause.
- Built with `-D__linux__ -D__XSdk__`. xsdk has pthreads but no
  `semaphore.h`, so a small POSIX semaphore shim on pthread mutex/cond was
  added. `usleep` and two pthread attribute stubs are also supplied.
- `WelsQueryLogicalProcessInfo()` uses `sysconf(_SC_NPROCESSORS_ONLN)` under
  `__XSdk__`.
- `SetOption(DECODER_OPTION_NUM_OF_THREADS, …)` must run **before**
  `Initialize`; changing it afterwards crashes in `sem_wait`.
- Input is Annex-B NALs; `DecodeFrameNoDelay()` emits pictures.

## Xenia results

Decode only, 300-frame clips, one thread, zero errors:

| Clip | Profile | Decode |
|---|---|---|
| 640×360 | Main, CABAC, 3 B-frames | **157.5 fps** |
| 854×480 | Main, CABAC, 3 B-frames | **92.4 fps** |
| 1280×720 | High, CABAC, 3 B-frames | **42.7 fps** |

The picture-150 YUV dumps matched `ffmpeg`'s visible-area output byte for byte
for 360p and 720p.

These are emulator numbers. The earlier h264bsd console run was roughly half
the Xenia decode rate, so 360p/480p should still clear 30 fps on hardware, while
720p High is likely borderline. Console measurement of this port is pending.

## What is still missing in the browser

1. MP4 (ISO BMFF/avcC) demuxing.
2. Bounded HTTP download of a video file.
3. A fullscreen YUV→RGB presenter (GPU shader preferred).
4. AAC audio and A/V clock sync.
5. Input handling to stop playback and return to the page.

Direct-MP4 sites such as test-videos.co.uk, W3Schools and archive.org are the
nearest target. YouTube's public stream extraction is also blocked by bot checks
on many networks, so it is not the first integration target.
