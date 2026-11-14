# New textures read zero on the phone (metal2vulkan patch 0002)

## Symptom

On the phone the background of Safari's tab overview was purple (user photo, 2026-11-13). A cached wallpaper blur
that SpringBoard had written (`Variant-home__CachedSnapshot-MediumGrayTintedBlur...`, host copy from 2026-11-13)
decodes to the single colour (179, 77, 179). That is `0.4 * (1, 0, 1) + 0.6 * 0.5`: opaque magenta under a 60 % mid-grey
tint.

## Cause

`metal2vulkan-runtime` reproduces what a new `Private` texture reads on an Apple-silicon Mac while Metal keeps it
losslessly compressed: not zero but a fixed texel, opaque magenta for the 8-bit colour formats
(`texture/heap_tiled.rs` `fresh_pattern`, written by `Device::fresh_patterned` in `texture.rs`). The bridge creates
every texture with the default `allow_gpu_optimized_contents = true`, so every eligible new texture started magenta.
Wherever CoreAnimation sampled a texture that nothing had written yet, the magenta reached the screen.

Which CoreAnimation texture is read unwritten, and why, is NOT established. This patch removes the visible colour,
not that read.

## Change

`metal2vulkan-fuxi-0002-fresh-zero.patch`: `Device::fresh_patterned` returns `None` (the texture then starts
zeroed or cleared exactly like a format that has no pattern) when the pattern is off.

- Default: off on iOS (`cfg!(target_os = "ios")`), on elsewhere, so host tests of the pattern are unchanged.
- `FUXI_M2V_FRESH_PATTERN=1` restores the pattern on the phone; `=0` turns it off on a host.

## Limits

- Not compiled locally (the host has rustc 1.97.1, the crate needs 1.98); CI is the first build.
- Not verified on device.
- Images already written to disk with the magenta in them (the cached wallpaper blur) stay as they are until
  SpringBoard regenerates them.
