# Kakadu SDK and Vitrinli builds

Vitrinli's JPEG2000 support uses the proprietary **Kakadu SDK**. The
SDK is not redistributable, so it lives in the private
[`dasch-swiss/dsp-ci-assets`](https://github.com/dasch-swiss/dsp-ci-assets)
repo as a release asset. Bazel fetches it at build time via a
custom `gh_release_archive` repository_rule (shared with the benchmark
fixture archive) that shells out to
`gh release download`. The SHA-256 is pinned, so builds are fully
reproducible and verifiable.

## How it works

The repository_rule is declared in
[`bazel/gh_release.bzl`](https://github.com/dasch-swiss/vitrinli/blob/main/bazel/gh_release.bzl)
and instantiated by the `kakadu_extension` module extension
([`bazel/kakadu_extension.bzl`](https://github.com/dasch-swiss/vitrinli/blob/main/bazel/kakadu_extension.bzl),
wired in `MODULE.bazel`). On the first `bazel build`
invocation that needs Kakadu (most do — `//src/cli:sipi`,
`//src:sipi_image`, every test that links sipi), Bazel:

1. Resolves the `gh` binary on PATH (the dev shell provides it).
2. Calls `gh release download <tag> --repo dasch-swiss/dsp-ci-assets
   --pattern v8_5-XXX.zip` into Bazel's repository cache.
3. Verifies the downloaded archive against the pinned SHA-256.
4. Exposes the unpacked source tree as the `@kakadu` repository,
   whose symlinked `BUILD.bazel` (`bazel/kakadu.BUILD.bazel`) wraps
   Kakadu's Makefile build and exports the `@kakadu//:kdu` library.

After the first download, the archive lives in Bazel's content-
addressed repository cache and never re-downloads (until the SHA
pin moves).

## Local-dev requirements

- Membership in the `dasch-swiss` GitHub organisation.
- `gh auth login` completed once (so `gh` resolves a usable PAT).
- The dev shell active (`nix develop`); `gh` is on PATH there.

That's it. There is **no** `vendor/` step, no
`just kakadu-fetch` recipe, no `GH_TOKEN` export. `gh` reads its
token from `~/.config/gh/hosts.yml` automatically.

```bash
gh auth login          # one-time
nix develop
just bazel-build       # repository_rule fetches Kakadu on first build
```

## CI requirements

Every Bazel-invoking workflow step sets
`GH_TOKEN: ${{ secrets.DASCHBOT_PAT }}` on its `env:` block so the
`gh_release_archive` repository_rule can authenticate non-interactively.
The PAT is scoped to read `dsp-ci-assets`. Bazel's repository cache
is keyed by SHA-256, so subsequent CI runs on the same key (and
the same `MODULE.bazel.lock`) reuse the download from the disk
cache.

## Updating the Kakadu version

1. Publish the new archive on `dsp-ci-assets` (see
   [its `kakadu/README.md`](https://github.com/dasch-swiss/dsp-ci-assets/blob/main/kakadu/README.md))
2. In `bazel/kakadu_extension.bzl`, update the tag/asset/sha256
   attributes to match the new release.
3. Run `bazel build //src/cli:sipi`. A SHA-256 mismatch at this step
   means the pin disagrees with the published archive — check the
   release asset and the pin are consistent.
4. Run `just bazel-test` to confirm the new SDK passes the test
   suite.
5. Commit `bazel/kakadu_extension.bzl` and `MODULE.bazel.lock`, open a PR.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `gh: command not found` | Outside the dev shell | `nix develop` |
| `release not found` from `gh` | Not authenticated, or no org membership | `gh auth login`; ask to be added to `dasch-swiss` |
| Bazel: `GH_TOKEN required` | CI: workflow step missing `env:` block | Add `GH_TOKEN: ${{ secrets.DASCHBOT_PAT }}` to the offending step |
| Bazel: `sha256 mismatch` | Release asset replaced or pin out of date | Recompute SHA-256 and update `bazel/kakadu_extension.bzl` |

## Why not vendor it directly?

- Vitrinli is a public repo; Kakadu is proprietary — committing it
  would be a licence breach.
- Keeping it in `dsp-ci-assets` aligns the licence-compliance
  boundary with repo membership.
- Hash-pinning in `bazel/kakadu_extension.bzl` gives reproducible, cacheable
  builds without the ~15 MB re-commit churn per version bump.
