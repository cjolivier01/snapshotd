# AGENTS.md

## Scope
This file applies to the entire repository.

## Project
- Name: `snapshotd`
- Goal: provide a small privileged broker for safe CRIU checkpoint/restore operations behind a narrow, policy-enforced API.

## Structure
- C++ sources: `src/csrc/`
- Tests: `tests/csrc/`
- Debian packaging: `packaging/`
- Docs: `README.md`
- Design docs: `docs/`

## Common commands
- Build debug: `make debug`
- Build release: `make release`
- Build package: `bazel build //:snapshotd_deb`
- Run tests: `bazel test //tests/csrc:protocol_store_test //tests/csrc:daemon_integration_test`
- Install package: `make install`

## Expected workflow
1. Run the Bazel tests before and after code changes.
2. Keep the privileged API narrow; do not expose raw CRIU argv, raw config paths, or user-controlled image/work directories.
3. Keep README, packaging, and design docs aligned with the implemented broker behavior.
4. Create normal open pull requests by default; do not open draft PRs unless the user explicitly asks for a draft.

## CRIU notes
- Prefer the explicit CRIU path `/usr/local/sbin/criu`.
- Debian packaging must not depend on a distro `criu` package.
- It is acceptable for the package to hard-code `/usr/local/sbin/criu`.
- If feasible with normal Debian maintainer-script behavior, package install should fail when `/usr/local/sbin/criu` is missing or not executable.

## Code guidelines
- Keep dependencies minimal.
- Prefer deterministic, non-interactive CLI behavior.
- Avoid widening permissions on broker-owned private state.
