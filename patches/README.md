NetSurf patch
=============

`netsurf-xenon.patch` contains the changes needed in the NetSurf tree for the
Xenon target. It was generated against NetSurf commit:

    39da3c3a40af4566d86500ff3052dfdc7f9a0378

Apply it inside a clean NetSurf checkout:

    git clone https://github.com/netsurf-browser/netsurf.git netsurf-src
    git -C netsurf-src checkout 39da3c3a40af4566d86500ff3052dfdc7f9a0378
    git -C netsurf-src apply ../patches/netsurf-xenon.patch

The patch:

- adds `xenon` to the valid frontend targets;
- disables the zlib link when curl is off;
- maps `nsxenon` to the frontend options;
- provides the `inet_aton` declaration used by urldb;
- disables `mmap` on PowerPC;
- relaxes one min/max invariant in layout instead of aborting on odd pages.

The frontend itself is not in the patch. Copy `frontends/xenon` into
`netsurf-src/frontends/xenon` after applying it.
