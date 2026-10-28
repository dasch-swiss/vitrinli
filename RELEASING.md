# Releasing Vitrinli

Releases are fully automated via [release-please](https://github.com/googleapis/release-please).
CI/CD details live in [`docs/src/development/ci.md`](docs/src/development/ci.md);
this file lists only the manual steps a human maintainer takes around an
automated release.

## How a release happens (overview)

1. [Conventional Commit](https://www.conventionalcommits.org/) prefixes on
   `main` drive release-please. The full prefix-to-release mapping table is
   in [`ci.md`](docs/src/development/ci.md#release-automation-release-please).
2. release-please opens (or updates) a release PR with title
   `chore(main): release X.Y.Z`. The PR diff updates `version.txt`,
   `CHANGELOG.md`, and `.github/release-please/manifest.json`.
3. Merging the release PR pushes a `vX.Y.Z` tag.
4. The tag triggers [`.github/workflows/publish.yml`](.github/workflows/publish.yml):
   - `validate-docker` — per-arch Nix-built Docker image + smoke test.
   - `publish-docker` — push images to Docker Hub, generate SBOM, upload
     debug symbols to Sentry.
   - `manifest` — multi-arch Docker manifest combining amd64 + arm64.
   - `docs` — mkdocs deploy to https://sipi.io.
   - `sentry` — release finalisation.

## Maintainer checklist (per release)

**Before merging the release PR:**

- [ ] Announce the upcoming release in the DaSCH GitHub team channel so no
      last-minute PRs are merged.
- [ ] Sanity-check the diff: `version.txt`, `CHANGELOG.md`, manifest.
- [ ] Confirm CI is green on `main` for the commit being released.

**After merging the release PR (publish.yml runs automatically):**

- [ ] Watch `publish.yml` to green: `validate-docker` →
      `publish-docker` → `manifest` → `docs` → `sentry`.
- [ ] Verify the Docker manifest exists:
      `docker pull daschswiss/sipi:vX.Y.Z`.
- [ ] Verify the GitHub release page shows the changelog from
      release-please.
- [ ] Verify https://sipi.io reflects the new docs.

## Manual interventions (rare)

When release-please goes wrong:

- **Wrong commit prefix shipped to `main`** — open a follow-up commit with
  the correct prefix (or a `Release-As: X.Y.Z` footer commit) so
  release-please catches up. Do not edit `version.txt` / `CHANGELOG.md`
  by hand.
- **Need to skip a release** — leave the release PR unmerged. Every commit
  type is changelog-visible, so any merge to `main` keeps a release PR open;
  landing only internal-work commits no longer suppresses it. A release made
  up solely of internal-work commits is a patch bump.
- **Emergency patch on an old version** — branch from the tag, cherry-pick
  the fix, push a tag manually. `publish.yml` runs the same way.
- **`publish.yml` fails after a tag** — fix forward; do not delete the tag.
  Push a new patch-bump tag via release-please.

## What is NOT manual any more

For the avoidance of doubt — none of the following are manual steps:

- Bumping the version in `CMakeLists.txt`. The CMake project version is
  read from `version.txt`, which release-please owns.
- Editing `manual/conf.py`. The file no longer exists; mkdocs is the
  documentation system.
- Creating or closing GitHub milestones.
- Tagging the release commit. release-please does this on PR merge.
- Building or pushing Docker images. `publish.yml` does this.
- Publishing documentation. `publish.yml`'s `docs` job does this.
- Travis CI. Replaced by GitHub Actions.

See [`docs/src/development/ci.md`](docs/src/development/ci.md) for the
canonical CI/release pipeline description.
