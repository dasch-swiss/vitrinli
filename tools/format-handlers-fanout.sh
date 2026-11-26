#!/usr/bin/env bash
# Lists the shared edit sites a NEW image format handler must touch, beyond its
# own src/format_handlers/cpp/SipiIO<Fmt>.{h,cpp} pair. This is the mechanically-generated
# source for the `format_handlers` fan-out note in ARCH-MAP.md (DUNE-005): the format
# handlers are NOT added by dropping a file — the dispatch is a shared registry
# + switch. Re-run to refresh the site list; the count is the
# "cost of a 5th format" the ARCH-MAP entry cites.
#
# Enforcement of the boundary this documents is `docs-only` (convention): there
# is no build-time check that a new format updated every site. A future
# descriptor-registration table (deferred until a real 5th format arrives, per
# ADR-0006) would turn this fan-out into one registration point.
set -euo pipefail
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$ROOT"

echo "# Shared edit sites for a new image format (src/format_handlers fan-out)"
echo

echo "## 1. static handler registry — src/format_handlers/cpp/format_registry.cpp"
grep -nE '#include "SipiIO|make_shared<SipiIO' src/format_handlers/cpp/format_registry.cpp

echo
echo "## 2. read_dispatch (read + read_metadata) and read_shape dispatch branches — src/image/cpp/SipiImage.cpp"
grep -nE 'dispatched_key = "(tif|jpg|png|jpx)"' src/image/cpp/SipiImage.cpp

echo
echo "## 3. Bazel target srcs/hdrs (+ any new codec dep) — src/format_handlers/BUILD.bazel"
grep -nE 'SipiIO(Tiff|J2k|Jpeg|Png)\.(cpp|h)' src/format_handlers/BUILD.bazel

echo
echo "NOTE: the handler implements every pure virtual of SipiIO (read, read_shape,"
echo "read_metadata); read_metadata shares the read code path, as the four handlers do."
echo "NOTE: SipiImage::write() needs no shared edit — it is keyed generically by"
echo "\`ftype\` via io.at(ftype), so a caller just passes the new format's key string."
