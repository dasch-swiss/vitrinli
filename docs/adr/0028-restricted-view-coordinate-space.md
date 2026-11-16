---
status: accepted
---

# A size-restricted image is served as its own image

A `restrict` *Permission* with a `size` turns the image into a smaller image of
its own: the restricted dimensions (RW, RH). Every request against it, region
and size included, is answered in that coordinate space and cut from one
rendering of the whole image at (RW, RH).

## Context

The restricted `info.json` already reports (RW, RH) and hides the native size
and tile grid. The image route did not follow it. Region and size
resolved against the native pixels, and each region was then capped to a
sampling factor `f` of its own extent (the per-region sampling cap). Three defects
followed from the mismatch:

- A tiling viewer reads (RW, RH) from `info.json` and requests regions in that
  space; the server read them as native pixels. Region requests returned the
  wrong part of the image.
- Each region was resampled on its own, with its own rounding, so tiles did not
  stitch to `full/max` and showed seams.
- The canonical `Link` carried the region in native pixels and so disclosed the
  native size the `info.json` hides.

The dims themselves were computed twice, with different rounding, in the shell
and in the engine, so `info.json` and `full/max` could differ by one pixel.

## Decision

**The shell decides (RW, RH), once.** `routes::restriction` takes the preflight
`size`, the watermark presence and the native dims, and either returns (RW, RH)
or refuses. `info.json`, `knora.json` and the image route all call it, and the
image route passes (RW, RH) across the seam as two integers (`0, 0` = no size
cap). The engine never re-parses the restriction.

- **Refusal is by kind, not by dims.** With no watermark, a size that can never
  reduce any image (`max`, `pct:` of 100 or more, `red:0`) marks a
  misconfiguration and is refused: 403 on the image and on `info.json`, 404 on
  `knora.json`. With a watermark it is served at the native size, watermarked.
- **Box restrictions are served.** A box (`!w,h`, `w,`, `,h`, `w,h`, `pct:`
  below 100, `red:n` with n of 1 or more) caps at its box. An image already
  inside the box is served at its native size: that is within the configured
  policy and discloses nothing. The shell logs a warning, so an over-large box
  is visible to the operator.
- **A restricted axis below 2 px is refused**, with or without a watermark: the
  scale operators cannot produce a 1 px axis.

**(RW, RH) is the restricted image's coordinate space.** When
`restricted_width > 0`, the engine:

1. resolves the region against (RW, RH) (a partially-outside region clamps, a
   start beyond the image is a 400) and the size against the region extent with
   the usual IIIF rules;
2. refuses with 400 any output wider than RW or taller than RH, checked on the
   size before rotation, and any crop that would need scaling from or to a
   1-sample axis, both before the cache lookup and before the HEAD return;
3. decodes the whole image once at (RW, RH) (FULL region, `PIXELS_XY` size),
   crops the region from that rendering and scales only when the requested size
   differs from the crop extent;
4. runs rotation, quality, watermark and encode unchanged.

**`maxWidth`/`maxHeight` = (RW, RH).** The restricted `info.json` states them
(IIIF Image API 3.0 §5.2), so `^max` equals `max` and a `^` request may upscale
a small region up to, never beyond, the `full/max` size. The bound is checked
before rotation because rotation only swaps or grows the bounding box; it adds
no information.

**The cache key carries the restriction.** The *Cache key* gets
`/r<RW>x<RH>`, so a restricted and an unrestricted request with the same
canonical coordinates, or two restrictions of one file, never share an entry.
The canonical `Link` does not carry it; it names the restricted-space region
and size and never the native size. In both, the size resolves against the
region extent, as the served output does, so two different outputs never share
a key.

**The passthrough never serves a restricted image.** The direct-file
passthrough requires `restricted_width == 0`; otherwise `full/max` in the
source format would return the unrestricted original.

**The decode-memory estimate charges the real decode.** For a restricted
request it is computed from (FULL, `PIXELS_XY(RW, RH)`) plus the output dims,
so a small region cannot hide a whole-image decode from the full-lane budget.

**Watermark-only `restrict`, `allow` and `stream` are unchanged**: native dims
and native coordinates.

### What this supersedes, and what it keeps

It replaces the per-region sampling cap (commit 6782442). Both residuals that
the cap accepted are gone by construction: overlapping
sub-pixel regions are cut from one rendering and so carry no more than
`full/max`, and the `Link` is in restricted coordinates. The cap's two
load-bearing decisions stand:

- Region requests on a restricted image are served, not rejected; rejecting
  them breaks tiling viewers.
- Nothing served carries more information than `full/max`.

## Considered options

**Keep native coordinates and report native dims with `maxWidth`/`maxArea`.**
Rejected: OpenSeadragon ignores `maxWidth`/`maxArea`, so the seams stay, and it
discloses the native size that the restricted `info.json` hides.

**Resample each native region to exactly its share of (RW, RH).** Rejected:
crop-from-one-rendering is exact by construction, needs no new resampling path
and keeps the change out of `SipiImage::read` and the codecs.

**Compute (RW, RH) in the engine and expose it over a new FFI call.**
Rejected: production IIIF parsing lives in Rust (ADR-0021), the shell's
`clamp_dims_to_size` is already the one clamp, and two integers are a smaller
seam change than a function.

## Consequences

- **Breaking for clients that send native coordinates** to a size-restricted
  image: they now get a different region, or a 400. A DSP deployment must move
  its viewer to `info.json` tiling in the same release that takes this SIPI.
- **Tiles at size `max` stitch to `full/max` exactly.** Tiles at another size
  involve a second resample and are not expected to stitch exactly.
- **Every restricted region request decodes the whole image at (RW, RH).** A JP2
  uses a reduce level and stays cheap; a non-pyramidal TIFF decodes at native
  size. Accepted: a restricted `info.json` advertises no tiles, so viewers
  mostly request `full/max`. There is no shrunk-image cache.
- **Size-restricted requests run in the Full admission lane.** The shell picks
  the lane before the preflight decision, from the IIIF params alone, so a
  restricted region request is first admitted as a Tile. Once the preflight
  yields a size restriction, the shell drops the Tile permit and acquires a
  Full permit when the engine is about to decode, after its cache check (DEV-7603, ADR-0022's 2026-11-16 amendment).
  A shed or timed-out Full acquire is a 503 with `Retry-After`. The Full lane
  is smaller, so restricted traffic is shed sooner than Tile traffic; a
  crawler on restricted non-pyramidal TIFFs occupies Full-lane threads, not
  Tile-lane threads.
