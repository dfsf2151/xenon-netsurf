# Building Xenon NetSurf

**The build is not turnkey yet.** This is an experimental port. The tested
workspace is a Linux x86-64 host with the xsdk cross toolchain, a pre-populated
`deps/` directory and a `prefix/` full of static libraries. The checked-in
source and patch are enough to reproduce that setup, but you will have to wire
the paths yourself.

The prebuilt `release/ns360.zip` is the recommended way to try the browser.

## Tested versions

| Component | Version / commit |
|---|---|
| Host | Linux x86-64 |
| Compiler | `powerpc-xsdk-elf-gcc` from xsdk-gcc (GCC 15.3) |
| xsdk | `xenosworks/xsdk`, `libxsdk.a` built in its checkout |
| NetSurf | `39da3c3a40af4566d86500ff3052dfdc7f9a0378` |
| mbedTLS Xenon | `1500b8ac2ab62f3ce6c6bebafac777e55dbee93b` |
| libcss | `006ffc4096040da13690abc380e5f828ec08284d` |
| libdom | `f69781e1f062444b5af3f62d431d7d94018da53b` |
| libhubbub | `6651b8cf87a4aa87bcdb2ff024a02659cd3f9402` |
| libparserutils | `6b0cbf086ca8eb8fe74b69f0c9ecf274eb2397ca` |
| libwapcaplet | `c7c128d3eb3223b216c974471f82e9337fbcf4ba` |
| libnsutils | `0bd39060740b6163bd50875326654a722df97eb2` |
| libnspsl | `82815c2bc7fd70d1b6afccfa89a9a0f3fa73db8a` |
| libnsbmp | `3e5409942cc8a7ea7ef705b035394c6089cef4f8` |
| libnsgif | `22e99eb6818b1284d0f3ff1b7f46159e87221220` |
| libpng | `ed217e3e601d8e462f7fd1e04bed43ac42212429` |
| zlib | `51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf` |
| utf8proc | `f72a4023617e2d879f2ca93086fd02bc7e3149fe` |
| libjpeg | IJG jpeg-9e |
| buildsystem | `0005ae300283ff01c2e2b05e7376b3e55dea21f7` |

## Workspace layout

The frontend Makefile and `config/Makefile.config.example` assume this layout:

    xenon-netsurf/
      frontends/xenon/          this repository
      netsurf-src/              NetSurf checkout + copied frontend
      deps/                     dependency sources
      prefix/                   cross-built libraries, headers, pkg-config files
      third_party/mbedtls-xenon/
      third_party/xsdk/         xsdk checkout (or ../UCN_MVP_X360/third_party/xsdk)

`XSDK_ROOT`, `XENON_PREFIX`, `XENON_MBEDTLS`, `XENON_CC`, `XENON_CXX` and
`XENON_AR` can all be overridden. If your xsdk lives elsewhere, edit the copied
`Makefile.config` and/or export those variables.

## 1. Toolchain and xsdk

Build xsdk first. Its `build/libxsdk.a` and `src/xsdk.ld` are linked into the
XEX. The compiler must be the xsdk GCC with the 32-bit LR fix from
`xsdk/tools/toolchain/gcc-xsdk.patch`; without it, mbedTLS SHA-3 and other code
can fail intermittently.

The default toolchain path is `~/xsdk-gcc/bin/powerpc-xsdk-elf-*`.

## 2. NetSurf and the patch

```sh
git clone https://github.com/netsurf-browser/netsurf.git netsurf-src
git -C netsurf-src checkout 39da3c3a40af4566d86500ff3052dfdc7f9a0378
git -C netsurf-src apply ../patches/netsurf-xenon.patch
cp -r frontends/xenon netsurf-src/frontends/
cp config/Makefile.config.example netsurf-src/Makefile.config
```

Edit `netsurf-src/Makefile.config` and set `XSDK_ROOT` and `XENON_PREFIX`. The
frontend Makefile also reads `XENON_MBEDTLS`, which defaults to
`third_party/mbedtls-xenon` beside the repository.

## 3. Dependencies

Clone the revisions from the table above into `deps/`. `build_deps.sh` builds
the core NetSurf libraries (libwapcaplet, libparserutils, libhubbub, libcss,
libdom, libnsutils, libnspsl) and installs them into `prefix/`.

The tested `prefix/` also contains cross-built zlib, libpng, libjpeg, utf8proc,
libnsbmp and libnsgif. They were built by hand with the same xsdk compiler:

- zlib: `./configure --prefix=$PREFIX --static` with the xsdk compiler.
- libjpeg 9e: `./configure --host=powerpc-xsdk-elf --prefix=$PREFIX \
  --disable-shared --enable-static CC=$XENON_CC`.
- libpng: CMake with the xsdk toolchain file, static libraries only.
- utf8proc, libnsbmp and libnsgif: their upstream makefiles with
  `PREFIX=$PREFIX`.

This part is the least polished. Check `prefix/lib` for the expected `.a`
files before building NetSurf.

## 4. mbedTLS Xenon

```sh
cmake -S third_party/mbedtls-xenon -B third_party/mbedtls-xenon/build-xsdk \
  -GNinja \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/xsdk/cmake/xsdk.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF
cmake --build third_party/mbedtls-xenon/build-xsdk
```

The frontend compiles with
`third_party/mbedtls-xenon/configs/config-xenon-netsurf.h` and links the static
libraries from `build-xsdk/library`.

## 5. Build NetSurf

```sh
PKG_CONFIG_PATH=$PWD/prefix/lib/pkgconfig \
  make -C netsurf-src TARGET=xenon PREFIX=$PWD/prefix -j8
```

The result is `netsurf-src/nsxenon` (an ELF with debug symbols).

## 6. Convert to XEX

```sh
python3 /path/to/xsdk/tools/elf2xex.py netsurf-src/nsxenon ns360/default.xex
```

Place `ns360/` beside `res/`, `home.html`, `help.html`, `about.html`,
`cacert.pem`, `mime.types`, `hosts.txt`, `url.txt`, `demo.wav` and `demo.mp3`.
The prebuilt zip shows the expected layout.

## 7. Package

```sh
cd dist && rm -f ns360.zip && zip -r -9 ns360.zip ns360
sha256sum ns360.zip ns360/default.xex
```

## Known build gaps

- Dependency acquisition and the image libraries are manual.
- The frontend Makefile still expects a few Unix-ish paths and the xsdk
  linker script.
- There is no CI. The binary in `release/` was built and checked on Xenia.
