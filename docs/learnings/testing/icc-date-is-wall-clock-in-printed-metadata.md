---
title: "A printed CIELab ICC profile carries the wall-clock creation time, so two reads of one file differ by a second"
date: 2026-11-27
category: "testing"
component: "metadata"
module: "src/metadata/cpp/icc.cpp, src/format_handlers/cpp/read_metadata_equality_test.cpp"
problem_type: "flaky-test"
severity: "medium"
symptoms: "read_metadata_equality_test failed once in CI on unit_CIELab16_tif: the operator<< output of the metadata read and of the full read differed only in the ICC-Date line, by one second."
root_cause: "For a CIELab TIFF, lcms2 creates the Lab profile during the read and stamps it with the current time. SOURCE_DATE_EPOCH pins the date only inside Icc::iccBytes() (the bytes a codec writes, ADR-0002), not the date operator<< prints."
tags: [icc, lcms2, cielab, source-date-epoch, determinism, flaky, operator-print, read-metadata]
---

# A printed CIELab ICC profile carries the wall-clock time

## What happened

`read_metadata_equality_test` compares `operator<<` of an image read with
`SipiImage::read_metadata` against `operator<<` of the same file read with
`SipiImage::read`. It passed locally and in most CI runs, then failed once on
`unit_CIELab16_tif` with an `ICC-Date` one second apart.

## Cause

A CIELab TIFF has no embedded profile; the reader asks lcms2 for a Lab profile,
and lcms2 sets its creation date to now. Two reads that straddle a second
boundary print two dates. `SOURCE_DATE_EPOCH` does not help here: ADR-0002
normalizes the date only in `Icc::iccBytes()`, the bytes a codec embeds, and
production never sets it.

## Fix

The equality test drops the `ICC-Date` line from both outputs before it
compares them. Every other ICC field is still compared.

## Prevention

- A test that compares the printed metadata of two reads of a profile that
  sipi creates (CIELab, and any other lcms2-built profile) must ignore
  `ICC-Date`, or compare `iccBytes()` under `SOURCE_DATE_EPOCH` instead.
- A one-off CI failure in a test that compares two reads is a reason to look
  for wall-clock state in the output before calling it flaky.
