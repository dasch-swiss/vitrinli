# Security Policy

## Reporting a Vulnerability

If you believe you have found a security vulnerability in Vitrinli, please report
it privately by emailing **security@dasch.swiss**, or through GitHub's
[private vulnerability reporting](https://github.com/dasch-swiss/vitrinli/security/advisories/new).
Do not open a public GitHub issue for suspected vulnerabilities.

Include as much detail as you can: affected version/commit, reproduction
steps, and the potential impact.

## Response Expectations

We will acknowledge your report within a few business days and keep you
informed as we investigate and address the issue.

## Fixing a Vulnerability

For maintainers. Until a fix is deployed, keep the details out of public issues,
PRs, commit messages and tests: a regression test can contain a working exploit.

1. Create a draft
   [security advisory](https://github.com/dasch-swiss/vitrinli/security/advisories).
   Drafts stay private until published.
2. Develop the fix in the advisory's temporary private fork
   (`POST /repos/dasch-swiss/vitrinli/security-advisories/{ghsa_id}/forks`).
3. Validate the fix yourself. GitHub runs no status checks on pull requests in
   temporary private forks, so CI proves nothing there. Run the tests locally, and
   for anything architecture-specific also test on `linux/amd64`: arm64 does not
   trap an integer division by zero that kills an x86-64 process. On arm64 hosts,
   emulate with QEMU, and do not run the test subject as PID 1, because the kernel
   ignores a fatal signal it sends itself.
4. Merge from the advisory, release, deploy, then publish the advisory so that
   other deployers learn to upgrade.

## Supported Versions

This project uses [release-please](https://github.com/googleapis/release-please)
to manage SemVer releases. Only the latest released major/minor version is
supported with security fixes. Please upgrade to the latest release before
reporting an issue.
