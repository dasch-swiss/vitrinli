---
title: "libpng transforms change the channel count, so a PNG's extra samples must follow the decoded layout"
date: 2026-11-27
category: "debugging"
component: "format_handlers"
module: "src/format_handlers/cpp/SipiIOPng.cpp, SipiIOJ2k.cpp (write)"
problem_type: "header-pixel-mismatch"
severity: "high"
symptoms: "A 228x320 8-bit RGB PNG without ICC failed to convert to JPX with 'Failed writing JPEG2000 image (Kakadu exception 1801745733)'. Kakadu's own text ('No colour description found') was only in the log."
root_cause: "SipiIOPng::read set the alpha extra sample from the PNG colour type before libpng's transforms ran. A bKGD chunk makes png_set_background composite and drop alpha (nc=3 with one extra sample); a tRNS chunk adds alpha (nc=4 or 2 with none). The JPX writer switches on nc - extra samples, and a value with no case gave Kakadu a layer without a colour box."
tags: [png, libpng, bkgd, trns, alpha, extra-samples, kakadu, jpx, colour-box, dev-8091]
issue: "DEV-8091"
---

# libpng transforms change the channel count

## What happened

The rdu-13 bulk ingest (2026-11-21) rejected some PNG uploads with an opaque
Kakadu exception. The file looked like plain RGB, and plain RGB without ICC
takes the writer's `case 3` and gets sRGB, so the first theory (a missing ICC
profile) did not explain it.

## Cause

`SipiIOPng::read` decided "this image has an alpha channel" from the colour
type in the header (`RGBA`, `GRAY_ALPHA`) and called `addEs(ASSOCALPHA)`.
Then libpng ran the transforms the reader asks for:

| Source | Transform | Decoded channels | Extra samples sipi set |
|---|---|---|---|
| RGBA + `bKGD` | `png_set_background` composites, drops alpha | 3 | 1 |
| RGB + `tRNS` | `png_set_tRNS_to_alpha` adds alpha | 4 | 0 |
| gray + `tRNS` | same | 2 | 0 |

`SipiIOJ2k::write` picks the colour space from `nc - es`. The values 2 (first
row and last row) have no case, so no colour box was written, and Kakadu
failed in `jp2_target::write_header`. Row 2 wrote a 4-colour JPX as CMYK.

## Fix

- The PNG reader reads the channel count after `png_read_update_info` and
  sets the alpha extra sample from that decoded layout (2 or 4 channels mean
  alpha), never from the header's colour type.
- `SipiIOJ2k::write` refuses a layout with `nc - es` not in {1, 3, 4} before
  any Kakadu object exists, with `kWriteFailed` naming `nc`, the extra-sample
  count and `bps`.
- Every returning `catch (kdu_exception)` in `SipiIOJ2k.cpp` appends the
  thread's Kakadu text, so the next opaque `kduE` names its reason.

## Prevention

- In any reader that asks a codec library for transforms, derive geometry
  (`nc`, extra samples, `bps`, `photo`) from the library's post-transform
  report, not from the header fields. `read_metadata` shares that code with
  `read`, so `query` reports the same geometry.
- `png_alpha_layout_test` (`formats_test`) builds RGBA+bKGD, RGB+tRNS,
  gray+tRNS, gray+alpha+bKGD and palette+tRNS PNGs and writes each as JPX.
- To check a transform's effect without Bazel, a 20-line C program against
  the system libpng with the reader's transform calls prints the decoded
  channel count.
