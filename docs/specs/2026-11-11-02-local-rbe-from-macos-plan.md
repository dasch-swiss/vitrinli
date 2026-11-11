---
title: "feat(bazel): run local macOS builds on the NativeLink RBE backend"
date: 2026-11-11
author: "Ivan Subotic"
status: implemented
repositories: []
---

# feat(bazel): run local macOS builds on the NativeLink RBE backend

## Overview

Add an opt-in `build:rbe-darwin` config to `.bazelrc` so a developer on an Apple Silicon
Mac compiles on the DaSCH NativeLink worker and runs the tests on the Mac, the way CI's
darwin-arm64 leg does. Move the cross-leg flags that CI and the new config share into one
`.bazelrc` config, so there is one writer. Correct the docs that say local development
never contacts the backend, and the docs that give the internal service ports as the
endpoint.

## Problem Statement / Motivation

CI already compiles all three targets on the shared x86_64 worker. A local build still
compiles everything on the laptop. `CLAUDE.md:45` states "local dev never contacts it" (the
RBE backend). That held only because the mTLS client certificate was the CI identity. The
infra role now issues personal client certificates (`infra` repo,
`OS/ansible/roles/dasch.nativelink/README.md` § PKI Setup), so a developer can authenticate
as themselves.

A manual test on 2026-11-11 showed that the connection works from a Mac: building
`//src/logging:logging` for `//platforms:linux_x86_64` gave 231 remote cache hits and 5 remote
executions. A darwin target has not been tried.

**Endpoint.** From outside the DaSCH network, the only reachable port is 443 on
`rbe-01.ops.dasch.swiss`, a proxy in front of NativeLink. The `:50051` and `:50052` that
`rbe.md`, `ci.md`, `.bazelrc` and the `bazel-rbe` action name are the internal service
ports behind it. The docs currently read as if a client connects to `:50051`; this plan
corrects them.

## Proposed Solution

Three layers, each with one writer:

- **`build:rbe-cross` in the repository's `.bazelrc`** holds the flags every cross leg needs
  when the target is not x86_64 Linux: `--host_platform=//platforms:linux_x86_64`,
  `--strategy=TestRunner=local` and `--noremote_local_fallback`. The `bazel-rbe` action emits
  `--config=rbe-cross` on its cross legs in place of these three inline flags. The action
  keeps `--platforms` and `--extra_execution_platforms`, because they derive from its
  `target-platform` input.
- **The developer's `~/.bazelrc`** defines `build:dasch-rbe`: endpoint, instance name, exec
  property, mTLS file paths, upload policy, eviction retries and `--jobs`. Each developer has
  their own certificate, and Bazel expands neither `~` nor environment variables in the TLS
  flags, so these lines cannot be committed. The exact lines are the contract below.
- **`build:rbe-darwin` in the repository's `.bazelrc`** combines the two for a Mac and adds
  the darwin platforms and `--remote_download_toplevel`.

**The `build:dasch-rbe` contract.** Every developer who opts in adds exactly these lines to
their own `~/.bazelrc`, with absolute paths to their own certificate files:

```
build:dasch-rbe --remote_executor=grpcs://rbe-01.ops.dasch.swiss:443
build:dasch-rbe --remote_cache=grpcs://rbe-01.ops.dasch.swiss:443
build:dasch-rbe --remote_instance_name=main
build:dasch-rbe --remote_default_exec_properties=cpu_arch=x86_64
build:dasch-rbe --tls_certificate=/absolute/path/to/ca.crt
build:dasch-rbe --tls_client_certificate=/absolute/path/to/client.crt
build:dasch-rbe --tls_client_key=/absolute/path/to/client.key
build:dasch-rbe --noremote_upload_local_results
build:dasch-rbe --experimental_remote_cache_eviction_retries=5
build:dasch-rbe --jobs=64
```

`ca.crt` is the public `dasch-remotebuild-ca` certificate. `client.crt` and `client.key` are
the developer's personal client certificate (`extendedKeyUsage=clientAuth`, signed by that
CA) and its PKCS#8 key.

Differences from CI's flag string (`.github/actions/bazel-rbe/action.yml:63-84`), each
deliberate:

- `--noremote_upload_local_results` instead of `--remote_upload_local_results=true`, so
  actions that run on the laptop do not write action-cache entries. This is a client-side
  setting, not an access control; see Security.
- `--jobs=64` instead of 192, because the worker is shared with CI.
- No `--noremote_cache_compression`: compression is off by default and `.bazelrc` never
  enables it.

The `.bazelrc` side:

```
# Cross legs: the target is not x86_64 Linux, so compiles and exec tools run on the
# x86_64 worker and the tests run on the client. Used by the bazel-rbe action and by
# rbe-darwin.
build:rbe-cross --host_platform=//platforms:linux_x86_64
build:rbe-cross --strategy=TestRunner=local
# Do not remove: a local fallback runs the x86_64 exec tools on the client, which fails
# with "cannot execute binary file" and hides the remote error.
build:rbe-cross --noremote_local_fallback

# Opt-in remote build execution from an Apple Silicon Mac. Needs `build:dasch-rbe` in
# ~/.bazelrc; see docs/src/development/rbe.md § Local use from macOS.
build:rbe-darwin --config=dasch-rbe
build:rbe-darwin --config=rbe-cross
build:rbe-darwin --platforms=//platforms:darwin_aarch64
build:rbe-darwin --extra_execution_platforms=//platforms:linux_x86_64,//platforms:darwin_aarch64
build:rbe-darwin --remote_download_toplevel
```

`--remote_download_toplevel` overrides the `.bazelrc` default `--remote_download_minimal`.
Without it, `bazel-bin/src/cli/rust/sipi` is not downloaded and a developer has no binary
to run (the same reason `justfile:332` and `justfile:692` pass it).

A developer runs `just bazel-test --config=rbe-darwin`. If `dasch-rbe` is not defined,
Bazel fails with "Config value 'dasch-rbe' is not defined".

**Supported recipes.** The build and test recipes that take `*FLAGS`: `bazel-build`,
`bazel-build-server`, `bazel-test`, `bazel-test-unit`, `bazel-test-e2e`. Not supported:
recipes that `bazel run` an exec-config tool (`bazel-rustfmt`, `compile-commands`) and
`bazel-coverage`, because `--host_platform=linux_x86_64` makes those tools x86_64 Linux
binaries that cannot run on the Mac.

## Alternative Approaches Considered

- **Put the endpoint and certificate paths in the repository's `.bazelrc`.** Rejected:
  certificates are personal, and Bazel does not expand `~` or environment variables in the
  TLS flags.
- **Use `user.bazelrc` (already `try-import`ed at `.bazelrc:523`) instead of `~/.bazelrc`.**
  Rejected: each repository would need its own copy. A `~/.bazelrc` serves every DaSCH
  repository on the same backend.
- **Copy the cross-leg flags into `build:rbe-darwin` and leave the action alone.** Rejected:
  two writers of the same flags drift. The action is the documented source of truth today
  (`rbe.md:398`), so an agent would change it and miss the copy.
- **A drift check between the action and `.bazelrc`.** Rejected in favour of one writer,
  which needs no check.

## Technical Considerations

- **Cache keys.** `build --action_env=PATH` (`.bazelrc:38`) puts the invoking `PATH` into
  action keys. A Nix dev shell `PATH` differs from that of CI's Nix-free macOS runner, so
  where `PATH` reaches an action, the developer's build executes on the worker instead of
  reusing CI's entry. This costs worker time and storage. It is not a security boundary:
  a client chooses its own keys. Changing `PATH` handling is out of scope.
- **`build:macos` follows the client OS.** `--enable_platform_specific_config`
  (`.bazelrc:47`) applies `build:macos` (`.bazelrc:158-162`) to every build started on a
  Mac, so `-mmacosx-version-min=14.0` also reaches the x86_64 Linux exec tools. CI's macOS
  runner has the same flags. Check the first run's log for anything worse than
  "argument unused" warnings.
- **Tests run locally.** `--strategy=TestRunner=local` keeps test execution on the Mac.
  `exclusive-if-local` then serialises the e2e tests (`test/e2e/sipi_e2e_test.bzl:184`).
  `just bazel-test` filters out `docker_smoke` (`--test_tag_filters=-requires-docker`).
- **Download volume.** A CI runner sits close to the backend; a laptop downloads the test
  inputs and top-level outputs over the internet. A cold run may be bound by download, not
  by the worker.
- **Linux targets from a Mac** are out of scope. A correct command needs `--config=rbe-cross`
  too, and nobody has tested one. The docs do not describe one.

### Security

NativeLink authenticates by CA signature only and has no revocation list. Every holder of
a valid certificate can write the action cache of instance `main` directly over the
remote-execution API, whatever their Bazel flags say. CI and release builds
(`publish.yml`) read that cache. `--noremote_upload_local_results` therefore only keeps an
honest client's local results out; it does not protect release artifacts from a malicious
or compromised certificate holder. Personal certificates widen the set of holders from the
CI identity to every opted-in developer.

This plan does not change the backend (`CLAUDE.md` § Infrastructure boundary). It:

- documents this as an accepted risk in `rbe.md`, with the response to a leaked key (rotate
  the CA, which also re-issues CI's certificate, and flush the action cache);
- tells developers to keep the key mode 0600 in a mode 0700 directory outside any repository
  and outside synced folders, to delete the PKCS#1 intermediate after conversion, and to ask
  for certificates as short-lived as the infra role allows;
- lists a follow-up for the infra repository: a separate instance name (for example `dev`)
  or a store that CI and release builds never read, or a read-only identity for
  developers. Either removes the risk. The maintainer applies infra changes.

**Key format.** Bazel's TLS stack reads only PKCS#8 keys (`BEGIN PRIVATE KEY`). A PKCS#1 key
(`BEGIN RSA PRIVATE KEY`, from `openssl genrsa` on LibreSSL) fails with "Unable to decode
key". Convert it with `openssl pkcs8 -topk8 -nocrypt -in client-rsa.key -out client.key`.

## Implementation Approach

- [x] Preflight (stop and report the plan as blocked if a check fails): the three files
      named by the `--tls_*` flags in `~/.bazelrc` exist; the key file starts with
      `-----BEGIN PRIVATE KEY-----`; `openssl s_client -connect rbe-01.ops.dasch.swiss:443
      </dev/null` completes a TLS handshake. Run every Bazel command inside the repository's
      dev shell (`nix develop -c …`, or the direnv shell).
- [x] `.bazelrc`: add the `build:rbe-cross` and `build:rbe-darwin` blocks above, directly
      after `build --remote_max_connections=200`.
- [x] `.bazelrc` remote section comments:
      - say the client reaches the backend on `grpcs://rbe-01.ops.dasch.swiss:443`, a proxy in
        front of NativeLink's internal `:50051`;
      - replace "Local dev never sets the connection flags, so the tuning flags below are
        safe no-ops without a remote configured" with: the connection flags come from the
        `bazel-rbe` action in CI and from the opt-in `--config=rbe-darwin` locally, and the
        tuning flags are no-ops without a remote;
      - add to the `--remote_local_fallback` bullet that cross legs turn it off
        (`rbe-cross`).
- [x] `.bazelrc:153`: change "the macos-14 CI leg" to "the macos-15 CI leg"
      (`ci.yml:119`).
- [x] `.github/actions/bazel-rbe/action.yml`: in the cross-leg branch, emit
      `--config=rbe-cross` in place of `--strategy=TestRunner=local
      --host_platform=//platforms:linux_x86_64 --noremote_local_fallback`. Update the
      comment above it to point at `.bazelrc`. Update the `runner-endpoint` input example
      to the external form (`grpcs://<host>:443`).
- [x] `justfile:692`: replace "A no-op in local dev (no remote configured)" with "A no-op
      without a remote configured".
- [x] `docs/src/development/rbe.md`:
      - Backend table (lines 35-38): state that `:50051` and `:50052` are internal ports and
        that clients, CI included, reach the backend through the proxy on
        `rbe-01.ops.dasch.swiss:443`. The bazel-remote row cites
        `--experimental_remote_downloader`, which `.bazelrc` and the action say is not
        used; reconcile it with the "Why a separate bazel-remote" section.
      - GitHub Actions wiring (lines 82-83) and "The fix" item 4 and 5: the cross-leg flags
        now come from `--config=rbe-cross`.
      - Reference (line 398): the action is the source of truth for the connection flags;
        `.bazelrc` `rbe-cross` is the source of truth for the cross-leg flags.
      - New section "Local use from macOS", before "CI topology". Content, nothing more:
        the `dasch-rbe` contract block and the three deliberate differences from CI; a
        link to the infra role README § PKI Setup; the PKCS#8 note and conversion command;
        key storage; the two commands (`just bazel-test --config=rbe-darwin`,
        `just bazel-build-server --config=rbe-darwin`); the supported and unsupported
        recipes; one sentence on `--noremote_local_fallback`; one sentence on the `PATH`
        cache-key effect; the security risk and the leaked-key response from Security.
- [x] `docs/src/development/ci.md`: line 297, the ports are internal behind the `:443`
      proxy; lines 304-305, replace "safe no-ops without a remote configured" with the
      `.bazelrc` wording; "Local reproduction", add one line pointing to
      `rbe.md#local-use-from-macos`.
- [x] `docs/src/development/bazel.md` `--config=` table: add a row for
      `--config=rbe-darwin` linking to `rbe.md#local-use-from-macos`.
- [x] `CLAUDE.md:45`: replace "local dev never contacts it" with "local development uses it
      only through the opt-in `--config=rbe-darwin`", keeping the `rbe.md` link.
- [x] `CONVENTIONS.md` § Build Reproducibility Invariant: add one paragraph.
      `--config=rbe-darwin` is a local-only opt-in that CI never uses. CI gets its RBE flags
      from the `bazel-rbe` action. The cross-leg flags both share live in `.bazelrc`
      `build:rbe-cross`.
- [x] `docs/mkdocs.yml`: add `- Remote Build Execution: development/rbe.md` to `nav:`
      directly before `RBE Write Pressure`, and add `development/rbe.md` with a one-line
      description to the page-description list after `development/ci.md`.
- [x] Run `just docs-build`. It succeeds, and its output has no warning that names
      `development/rbe.md`.
- [x] Run this sweep and reword every hit that contradicts opt-in local use or the `:443`
      proxy:
      `rg -n -i 'local dev.*(never|no remote)|never contacts|no-?ops? (in|without)|:5005[12]' CLAUDE.md CONVENTIONS.md ARCH-MAP.md justfile .bazelrc .github docs --glob '!docs/specs/**'`.
- [x] Run `rg -n -- '--host_platform=//platforms:linux_x86_64' .bazelrc .github`. It shows
      exactly one hit, in `.bazelrc`.
- [x] Run `bazel build --nobuild --config=rbe-darwin //src/cli:sipi //src/cli/rust:sipi` on
      the Mac. Analysis succeeds.
- [x] Run `just bazel-test --config=rbe-darwin` on the Mac. All tests pass, and the final
      `INFO: N processes: …` line shows a non-zero `remote` or `remote cache hit` count. If a
      test fails only under this config, stop and report the plan as blocked with the test
      name and error. Do not change test tags and do not add workarounds to `.bazelrc`.
- [x] Run `just bazel-build-server --config=rbe-darwin` on the Mac. Then
      `bazel-bin/src/cli/rust/sipi --version` runs on the Mac.
- [x] Push and confirm that all three `test` legs of CI pass, which proves the action's
      `--config=rbe-cross` change.

## Human Actions

| Id | Action | Who | When | Why not the agent |
|----|--------|-----|------|-------------------|
| H1 | Provide a Mac whose `~/.bazelrc` defines `build:dasch-rbe` per the contract in Proposed Solution, with a personal client certificate issued from the infra role (§ PKI Setup) and its PKCS#8 key | Ivan Subotic | before start | Issuing and installing a personal certificate needs the CA key and the developer's own credentials |
| H2 | Decide on the infra follow-up in Security (separate instance or read-only identity) | Ivan Subotic | after ship | Infra changes are operator-only |
| H3 | Review and merge the PR | Ivan Subotic | after ship | Merging is the owner's decision |

## Acceptance Criteria

- [x] `just bazel-test --config=rbe-darwin` passes on an Apple Silicon Mac, with remote
      executions or remote cache hits greater than zero.
- [x] `just bazel-build-server --config=rbe-darwin` leaves a runnable
      `bazel-bin/src/cli/rust/sipi`.
- [x] All three CI `test` legs pass with the action emitting `--config=rbe-cross`.
- [x] The cross-leg flags have one writer, `.bazelrc` `build:rbe-cross`.
- [x] No file outside `docs/specs/` claims that local development never contacts the RBE
      backend, or gives `:50051` as the address a client connects to.
- [x] `rbe.md` § Local use from macOS documents the contract, the certificate, the commands,
      the supported recipes and the accepted security risk, and the mkdocs nav lists
      `rbe.md`.

## Dependencies & Risks

- **The darwin target is untested remotely.** CI's darwin-arm64 leg proves the flag set
  from a macOS runner. A developer's Nix dev shell differs in `PATH` and `DEVELOPER_DIR`. If
  `just bazel-test --config=rbe-darwin` fails on something these flags cannot fix, stop and
  report the error text.
- **The action change touches every CI cross leg.** `--config=rbe-cross` must expand to the
  same three flags; the CI run on the PR proves it.
- **The backend is shared.** A cold local run adds load on the worker CI uses; `--jobs=64`
  limits it.
- **Infra.** No change to `dasch-remotebuild-prod-01` or the NativeLink role. The security
  follow-up is a proposal only.

## Success Metrics

Informational, not gating: record in the PR description the elapsed time of
`just bazel-test --config=rbe-darwin` next to plain `just bazel-test` on the same Mac, both
after one warm-up run.

## References

- `.github/actions/bazel-rbe/action.yml:59-104`: CI flag string and cross-leg flags
- `.github/workflows/ci.yml:117-120,184`: darwin-arm64 leg
- `.bazelrc:38,47,153,158-162,462-492,523`: `PATH`, platform config, `build:macos`, remote
  section, `user.bazelrc`
- `justfile:332,692`: `--remote_download_toplevel` uses
- `docs/src/development/rbe.md`: topology, cross-compile pattern, `exclusive-if-local`
- `docs/src/development/ci.md:292-311`: remote build execution summary
- `test/e2e/sipi_e2e_test.bzl:67-73,184`: e2e tags
- `infra` repo `OS/ansible/roles/dasch.nativelink/README.md` § PKI Setup: client certificate
  issuance
- dasch-specs `learnings/best-practices/sipi-nix-to-bazel-migration-lessons.md` §5:
  `strict_action_env` and Nix `PATH` in action keys
