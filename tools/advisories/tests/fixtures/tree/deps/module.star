module(
    name = "fixture",
    version = "0.0.0",
)

bazel_dep(name = "curl", version = "8.21.0.bcr.1")
bazel_dep(name = "zlib", version = "1.3.2")
bazel_dep(name = "rules_cc", version = "0.2.25")

oci = use_extension("@rules_oci//oci:extensions.bzl", "oci")
oci.pull(
    name = "base",
    digest = "sha256:1111111111111111111111111111111111111111111111111111111111111111",
    image = "registry.example/images/base-debian12",
    platforms = [
        "linux/amd64",
        "linux/arm64/v8",
    ],
)
use_repo(oci, "base")

# `http_archive` is also used (later in this file) to fetch the native-lib
# source archives, but those entries are below the LLVM
# toolchain block. We need it available here for the FFmpeg static
# pull, so bind it once now — `use_repo_rule` may be called multiple
# times for the same upstream rule.
http_archive = use_repo_rule("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# BtbN LGPL static FFmpeg — third-party prebuilt static-linked
# `ffmpeg`/`ffprobe`/`ffplay` binaries from the public CI pipeline at
# https://github.com/BtbN/FFmpeg-Builds/. We ingest only `ffmpeg` +
# `ffprobe` (no `ffplay`); see the `:ffmpeg_layer_*` `tar()` rules in
# `src/BUILD.bazel`.
#
# Why third-party rather than Debian's `ffmpeg`: Debian's `ffmpeg`
# package transitively pulls libsdl2 / libxv1 / libxext6 (~500 MiB) for
# ffplay's GPU video sink. The DSP stack only invokes `ffprobe` (via
# dsp-ingest's `docker run --entrypoint ffprobe daschswiss/sipi …` for
# video metadata) and never the GUI sinks; the static LGPL build is
# self-contained (no shared-lib closure to worry about) and ships at
# ~70 MiB for both binaries combined. License-clean: AGPL-licensed sipi
# invokes ffprobe as a subprocess (process boundary), so the LGPL+sipi
# combination has no viral linking concern.
#
# Pinning: BtbN's retention policy keeps the LAST autobuild of each month
# for two years; mid-month autobuild releases are pruned after a few weeks
# (a pruned URL 404s and breaks the fetch — exactly how the prior
# `autobuild-2026-05-08` mid-month pin died). So always pin a MONTH-END
# autobuild: `autobuild-2026-05-31-…` is retained until ~2028-05. The
# `n8.1.1-9-g…-lgpl-8.1` asset is the FFmpeg 8.1 stable branch as of that
# build (a few commits past the n8.1.1 tag), not the rolling-master
# nightly. Bumping means picking the last autobuild of a recent completed
# month, confirming it still carries `…-linux{64,arm64}-lgpl-8.X.tar.xz`
# assets, and refreshing both URLs + both sha256 sums.
http_archive(
    name = "ffmpeg_static_linux_amd64",
    build_file_content = """\
exports_files(
    ["bin/ffmpeg", "bin/ffprobe"],
    visibility = ["@@//src:__pkg__"],
)
""",
    sha256 = "9da613468e319c6444ff93d23f4a3fd16b67e44d4c9da62dc46f1822f2851ccb",
    strip_prefix = "ffmpeg-n8.1.1-9-g58d4114d36-linux64-lgpl-8.1",
    url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-05-31-13-22/ffmpeg-n8.1.1-9-g58d4114d36-linux64-lgpl-8.1.tar.xz",
)

http_archive(
    name = "ffmpeg_static_linux_arm64",
    build_file_content = """\
exports_files(
    ["bin/ffmpeg", "bin/ffprobe"],
    visibility = ["@@//src:__pkg__"],
)
""",
    sha256 = "cbc347fb037a45c2d8417d226567c961274c7f43bbd7765ad5a00d4f18aea63c",
    strip_prefix = "ffmpeg-n8.1.1-9-g58d4114d36-linuxarm64-lgpl-8.1",
    url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-05-31-13-22/ffmpeg-n8.1.1-9-g58d4114d36-linuxarm64-lgpl-8.1.tar.xz",
)

# A commented-out pin is not a dependency:
# http_archive(
#     name = "commented_out",
#     url = "https://example.invalid/commented_out-1.0.tar.gz",
# )

bazel_dep(name = "hedron_compile_commands", dev_dependency = True)
git_override(
    module_name = "hedron_compile_commands",
    commit = "078ba7675c05bea2666c92d9a8abe200d8547655",
    remote = "https://github.com/helly25/bazel-compile-commands-extractor.git",
)

kakadu_extension = use_extension("//bazel:kakadu_extension.bzl", "kakadu_extension")
use_repo(kakadu_extension, "kakadu")

approvaltests_cpp = use_repo_rule("//bazel:approvaltests_cpp.bzl", "approvaltests_cpp")

approvaltests_cpp(
    name = "approvaltests_cpp",
    sha256 = "c00f6390b81d9924dc646e9d32b61e1e09abda106c13704f714ac349241bb9ff",
    url = "https://github.com/approvals/ApprovalTests.cpp/releases/download/v.10.13.0/ApprovalTests.v.10.13.0.hpp",
)

http_archive(
    name = "mimalloc",
    build_file = "//bazel:mimalloc.BUILD.bazel",
    sha256 = "7b043c26b64dde66344d144fbcac3664858a4eb32197793ea2a18feb32741ca1",
    strip_prefix = "mimalloc-3.4.3",
    url = "https://github.com/microsoft/mimalloc/archive/refs/tags/v3.4.3.tar.gz",
)

http_archive(
    name = "tiff",
    build_file = "//bazel:tiff.BUILD.bazel",
    sha256 = "dd030ae5d6483033bd88d840fc97e36adc3022b844d9d896a3b5a50f2d660d6f",
    strip_prefix = "libtiff-4.7.2",
    url = "https://github.com/libsdl-org/libtiff/archive/refs/tags/v4.7.2.tar.gz",
)
