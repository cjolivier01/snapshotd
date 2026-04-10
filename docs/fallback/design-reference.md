# Design Reference

This page is the fallback-site companion to
`docs/safe-root-criu-broker-design.md`.

## Core Broker Invariants

- `snapshotd::HandleRequest` is the single policy entry point for accepted commands
- `snapshotd::ValidateAllowedFields` rejects request fields outside each command's allowlist
- `snapshotd::ProcessMatchesPeerSecurity` prevents stale or elevated process identities from drifting onto privileged operations
- `snapshotd::Store` confines authoritative images and metadata to broker-owned state beneath `state_dir`
- `snapshotd::BuildDumpCommand` and `snapshotd::BuildRestoreCommand` construct fixed CRIU invocations rather than forwarding raw user argv
- `snapshotd::PruneCheckpoints` applies retention only from daemon-owned metadata and budgets

## Recommended Reading Order

1. [IT Review Guide](it-review-guide.md)
2. [Long-form design](design/safe-root-criu-broker-design.md)
3. [API Reference](api/index.md)
