# Paired ASTC layout for Fuxi NativeMetal

The pinned NativeMetal frontend refuses QuartzCore's raw ASTC texture request
because its memory layout is unknown. Patch 0011 implements the producer and
consumer together, following the layout SPI shape in UniXNU/AdrenoMetal
4d789e9c369c8daff06854432f5328ad515a1058 (MIT).

Supported descriptors are ASTC 4x4 LDR/sRGB, single-sample 2D, array and cube
textures with mip chains. The importer accepts ShaderRead usage alone and makes
an owned snapshot through the existing checked bridge upload operations.
It does not provide public no-copy aliasing semantics. 3D, other block formats
and heap/buffer-backed layout queries remain refused.

`initializeTextureMemory:` starts a bounded producer record. Only copies through
this layout count toward block coverage, and all blocks across every mip and
slice must be written before import. Invalid producer copies invalidate the
record. Finalization removes it. A known allocation must match the complete
descriptor and supplied length; unknown pointers still reach the existing
diagnostic and refusal path. Reads into the owned snapshot use bounded
self-memory copies. Upload failures return nil and retain the caller's ownership.
An accepted texture keeps its deallocator until its owner is released.

The watermark is `Fuxi ASTC pair 1`, distinct from Apple's and upstream DroidVM's.
Existing serialized data does not acquire provenance merely from its length or
format. Actual ImageIO/QuartzCore cache regeneration must be qualified on device.
Success messages use NSLog, because this port's NBLOG invokes the failure gate.
`FUXI_NATIVE_LINEAR_LAYOUT=0` disables both new entry points.

## Cross-process snapshots (patch 0012)

Device result for 0011 (2026-11-13): the refusal was unchanged. The refused
1125x2436 ASTC 4x4 request does not come from this process's producer. A client
that cannot open a Metal device (SpringBoard and the poster extensions on this
port) has no layout object; ImageIO writes its wallpaper snapshot as an ATX file
(`AAPL` signature, `HEAD`, `FILL` padding, `astc` chunk) whose payload starts at
a page-aligned file offset, maps it, and QuartzCore sends the mapping to the
render server flagged as texture-layout data. The render server passes that
mapping straight to `newTextureWithBytesNoCopy`.

The five ATX snapshots found on the device image were checked offline: the
payload is exactly 282 x 609 blocks x 16 bytes = 2747808 bytes at file offset
0x4000, and only a row width of 282 blocks makes every row self-consistent
(609 of 609 rows; neighbouring widths give at most 80). The data is plain
row-major ASTC.

Patch 0012 therefore admits an allocation with no producer record when all of
these hold: ASTC 4x4 LDR/sRGB, ShaderRead only, exactly one level and one slice,
a base aligned to 16384, and a length covering every block. The bytes are
copied with bounded self-memory reads into an owned snapshot and uploaded
through the same checked bridge call as 0011. Anything else still reaches the
diagnostic refusal. Multi-level and multi-slice foreign data stays refused
because its serialized order has not been observed.
`FUXI_NATIVE_FOREIGN_LINEAR=0` disables only this path.

## Validation

- Portable arithmetic, observed 1125x2436 extent, odd mip/slice offsets, padded
  block copies, limits and failures: Windows MSVC AddressSanitizer and Linux
  clang AddressSanitizer/UBSan passed.
- Full patched frontend: macOS and iOS 16 syntax checks passed in
  [43056824434](https://github.com/scyx2611/fuxi-metal-frontend-check-20261108/actions/runs/43056824434).
- Existing frontend tests and new actual producer/provenance/importer tests
  under AddressSanitizer/UBSan passed on macOS ARM64 in
  [43060844001](https://github.com/scyx2611/fuxi-metal-frontend-check-20261108/actions/runs/43060844001).
  Texture allocation/upload use a byte oracle; this does not qualify GPU pixels.
- iOS 16 arm64 build, signing and packaging passed in
  [43056837931](https://github.com/scyx2611/fuxi-metal-frontend-check-20261108/actions/runs/43056837931).
  Artifact ZIP SHA-256 is
  `d91e9407f1371a508ea85b1f1ec6ef3195f5d184c3a5c57c798a9ce278896298`.
  All 19 manifest files and the three reviewed source files matched independently.

Both successful full runs use source revision 9801b2d; later workflow changes
restrict full host acceptance to ARM64. The unmodified census forwarding fixture
fails on x86_64 when forwarding a large structure through the ARM64-style entry.
The new production patch and tests are unchanged across these workflow changes.

No device was available. This candidate has not been staged or booted on Fuxi.
Qualification still requires known-fixture GPU sampling, the actual producer and
cache path, sustained desktop GPU frames, correct panel output and touch.
