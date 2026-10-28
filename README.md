[![Build Status](https://github.com/dasch-swiss/vitrinli/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/dasch-swiss/vitrinli/actions)

# Vitrinli

## Overview

Vitrinli (formerly SIPI, the Simple Image Presentation Interface) is a multithreaded, high-performance, IIIF compatible media server developed by
the [Swiss National Data and Service Center for the Humanities](https://dasch.swiss) at the
[University of Basel](https://www.unibas.ch). It is designed to
be used by archives, libraries, and other institutions that need to
preserve high-quality images while making them available online.

Vitrinli implements the Image API 3.0 of the International Image Interoperability Framework
([IIIF](https://iiif.io/)) at Level 2 — see the conformance matrix in
[`docs/src/guide/introduction.md`](docs/src/guide/introduction.md) for the supported
region, size, rotation, quality, and format forms — and efficiently converts between
image formats, preserving metadata contained in image files. In particular, if
images are stored in [JPEG 2000](https://jpeg.org/jpeg2000/) format,
Vitrinli can convert them on the fly to formats that are commonly used on
the Internet. Vitrinli offers a flexible framework for specifying
authentication and authorization logic in [Lua](https://www.lua.org/)
scripts, and supports restricted access to images, either by reducing
image dimensions or by adding watermarks. It can easily be integrated
with [Knora](https://dsp.dasch.swiss/). In addition Vitrinli preserves most of
the [EXIF](http://www.exif.org),
[IPTC](https://iptc.org/standards/photo-metadata/iptc-standard/) and
[XMP](http://www.adobe.com/products/xmp.html) metadata and can preserve
or transform [ICC](https://en.wikipedia.org/wiki/ICC_profile) color
profiles.

In addition, a simple webserver is integrated. The server is able to
serve most common file types. In addition Lua scripts and embedded Lua
(i.e., Lua embedded into HTML pages using the tags
&lt;lua&gt;…&lt;/lua&gt; are supported.

Vitrinli can also be used from the command line to convert images to/from
TIFF-, [JPEG 2000](https://jpeg.org/jpeg2000/), JPEG- and PNG-
formats. For all these conversion, Vitrinli tries to preserve all embedded
metadata such as
- [IPTC](https://iptc.org/standards/photo-metadata/iptc-standard/)
- [EXIF](https://www.exif.org/)
- [XMP](https://www.adobe.com/products/xmp.html)
- [ICC](https://en.wikipedia.org/wiki/ICC_profile) color profiles.
However, due to the limitations of some file formats, it cannot be
guaranteed that all metadata and ICC profiles are preserved.
- [JPEG2000](https://jpeg.org/jpeg2000/) (J2k) does not allow all types of ICC profiles
  profiles. Unsupported profile types will be added to the J2k header as comment and will be

Vitrinli is a [free software](http://www.gnu.org/philosophy/free-sw.en.html),
released under the [GNU Affero General Public
License](http://www.gnu.org/licenses/agpl-3.0.en.html). It is written in
C++ and runs on Linux and macOS. Note: In order to compile Vitrinli, the user has
to provide a licensed source of the [kakadu software](https://kakadusoftware.com).

It is written in C++ and runs on Linux (including Debian, Ubuntu, and CentOS) and
macOS.

Freely distributable binary releases are available
[daschswiss/sipi](https://hub.docker.com/r/daschswiss/sipi) as docker image.

## Documentation

The documentation is online at https://sipi.io.

To build it locally, you will need [MkDocs](https://www.mkdocs.org/).
In the root the source tree, type:

```
make docs-build
```

You will then find the manual under `site/index.html`.

## Building from source

All commands are run from the repository root via `just`. Run `just` for a full list of targets.

### Build and test with Bazel (what CI runs)
```bash
just bazel-build       # bazel build --stamp //src/cli:sipi (fastbuild — fast incremental rebuilds)
just bazel-coverage    # build + run all tests (unit + approval + e2e) under coverage instrumentation
just run               # start SIPI server with the localdev config
```

### Build a Docker image (Bazel rules_oci)
```bash
just bazel-docker-build-amd64   # or bazel-docker-build-arm64 — build + load image as daschswiss/sipi:latest
just bazel-test-smoke           # Docker smoke test (Bazel builds + loads the image)
```

### Inner-loop development

`just bazel-build` IS the inner loop. The first build is slow (cold action cache); subsequent edits to a single `.cpp` file rebuild in seconds.

```bash
nix develop            # dev shell with build deps + bazelisk
just bazel-build       # cold action cache: slow; warm: seconds
./bazel-bin/src/cli/sipi server --config config/sipi.localdev-config.lua
```

See [Building Vitrinli from Source Code](https://sipi.io/development/building/) for full details.

## Sentry.io
If you would like to use Sentry.io for error reporting, you can set the environment variable
`SIPI_SENTRY_DSN` to the DSN of your Sentry project. If you do not set this variable, Sentry.io will not be used.

Further variables can be set to configure the Sentry client:
- `SIPI_SENTRY_ENVIRONMENT`: The environment in which the application is running. Defaults to `development`.
- `SIPI_SENTRY_RELEASE`: The release version number of the application.

Sentry is initialized once, in the Rust shell's `main`, and captures:
- **Panics** — for every verb (`server`, `health`, and the offline CLI subcommands).
- **Native crashes** (e.g. SIGSEGV in a codec) — `server` only, via an out-of-process minidump reporter. See [docs/adr/0018-minidump-crash-memory-accepted-risk.md](docs/adr/0018-minidump-crash-memory-accepted-risk.md) for what a minidump can contain.
- **Image processing failures** (`server` only) — a handled read/convert/write error is reported with rich image context (dimensions, colorspace, channels, bits/sample, ICC profile type, file paths), tagged with `sipi.phase`, `sipi.colorspace`, `sipi.bps`, and `sipi.output_format`. CLI conversion failures are not sent to Sentry — pass `--json` for the same diagnostic detail instead.

See [docs/src/running.md](docs/src/running.md) for full details on CLI exit codes, error output format, and integration guidance for calling services.

## Releases

Releases are published on Dockerhub: https://hub.docker.com/repository/docker/daschswiss/sipi

## How should I write my commits?

We use [Conventional Commit messages](https://www.conventionalcommits.org/), and
**every commit needs both a type and a scope** — `type(scope): subject`. The most
important prefixes:

* `fix(scope):` — a bug fix, a [SemVer](https://semver.org/) patch.
* `feat(scope):` — a new feature, a SemVer minor.
* `feat(scope)!:`, `fix(scope)!:`, etc. — a breaking change (the `!`), a SemVer major.

A CI gate (`commitlint-rs`) enforces the type + mandatory scope. The full type
list, the scope vocabulary, what `fix:` means, and how to run the check locally
(`just commit-lint`) all live in one place —
[docs/src/development/commit-conventions.md](docs/src/development/commit-conventions.md).

# Contact Information

DaSCH Support `<support@dasch.swiss>`
