# Safe Root CRIU Broker Design

## Goal

This design lets higher-level tools use CRIU with root privileges without
granting users general-purpose root execution or direct `sudo criu` access.

The key idea is the same one used by container systems: users talk to a narrow
management interface that operates on managed objects, and only that privileged
management layer is allowed to invoke CRIU.

## Programs

### `snapshotctl`

- Runs as the invoking user.
- Installed at `/usr/bin/snapshotctl`.
- Has no elevated privileges, no setuid bit, and no direct CRIU access.
- Sends narrow requests over a Unix socket:
  - `run`
  - `status`
  - `checkpoint`
  - `restore`
  - downstream clients can also use an internal `checkpoint-pid` request for
    peer-owned self-checkpoint flows

### `snapshotd`

- Runs as a root systemd service.
- Installed at `/usr/libexec/snapshotd/snapshotd`.
- Activated only through `/run/snapshotd.sock`.
- Authenticates callers with `SO_PEERCRED`, not request-provided UID/PID fields.
- In packaged installs, reads daemon-owned runtime defaults from
  `/etc/snapshotd/snapshotd.conf`.
- Owns the policy boundary:
  - only accepts a small allowlisted API
  - rejects oversized control messages before allocation
  - rejects unknown request fields
  - stores root-owned job and checkpoint metadata under `/var/lib/snapshotd`
  - maps each caller to managed jobs owned by that caller's kernel UID

The packaged service still keeps socket activation in systemd. The daemon
config file is for broker-owned runtime settings such as state paths, CRIU
paths, and worker timeouts, while socket path/mode/group stay in
`snapshotd.socket`.

### `snapshot-worker`

- Runs as a short-lived helper started only by `snapshotd`.
- Installed at `/usr/libexec/snapshotd/snapshot-worker`.
- Has no public control socket and no user-facing CLI surface.
- Invokes CRIU with a fixed argv and a scrubbed environment.

## Permission Model

- Users do not get `sudo criu`.
- Users do not get a general `sudo snapshotctl`.
- Only the root broker can launch the worker.
- Only the worker can invoke CRIU.
- The control socket is group-gated:
  - owner: `root`
  - group: `snapshot-users`
  - mode: `0660`
- State directories are root-owned:
  - `/var/lib/snapshotd`
  - `/var/lib/snapshotd/<uid>/jobs/...`
  - `/var/lib/snapshotd/<uid>/exports/...`
- The private and user-facing trees are intentionally separate:
  - `jobs/` holds authoritative broker state only
  - `job.meta`, `checkpoint.meta`, `restore.pid`, images, and work/log state
    under `jobs/` stay root-owned and private
  - `exports/` holds compatibility/diagnostic copies only
- Private metadata files are written `0600` and private job/checkpoint
  directories stay `0700`.

## Managed Jobs, Not Arbitrary Host PIDs

The daemon does not expose "checkpoint any PID on the machine as root".

Instead:

1. `snapshotctl run -- ...` asks the broker to launch a managed job.
2. The broker launches that job as the caller's UID/GID.
3. The broker records a generated `job_id` plus the managed process identity.
4. That identity is not just a bare PID. The broker stores the process start-time
   token and revalidates it before privileged operations so a stale `job_id`
   cannot drift onto a reused PID.
5. Security validation is stricter than "same real UID":
   - all real/effective/saved/fs UIDs must still match the caller
   - all real/effective/saved/fs GIDs must still match the caller
   - the target must not hold Linux capabilities
6. The `run` path also rejects setuid/setgid or file-capability executables
   before `execve()` so the broker never launches a managed job that gains
   privilege on exec.
7. Later checkpoint/restore operations refer to `job_id`, not an arbitrary PID.

This removes the biggest abuse case: using a privileged CRIU wrapper to inspect,
freeze, or restore another user's host process tree.

After restore, the broker adopts the restored PID as the current managed job
identity so later `status` and `checkpoint` operations continue following the
restored process rather than the original pre-restore copy.

### Optional Self-Checkpoint Bridge

The public CLI uses managed jobs, but some clients need to checkpoint the
current process without first re-launching that process through
`snapshotctl run`.

For that case a client can use a narrower broker request:

- `checkpoint-pid`

That request is still not a raw privileged PID checkpoint primitive. The broker:

- authenticates the caller with `SO_PEERCRED`
- verifies that the requested PID still matches the caller's unprivileged
  uid/gid/capability state
- captures and revalidates the process identity token (`pid` + start time)
  before privileged use
- creates a broker-owned managed job record for that process
- stores the authoritative checkpoint only under `/var/lib/snapshotd`

So the self-checkpoint bridge still blocks the critical abuse case called out by
IT:
checkpointing another user's processes or converting the CRIU path into general
root command execution.

## Export Compatibility

Some downstream clients want user-visible troubleshooting files in their own
runtime directory, for example:

- `checkpoint.done`
- `checkpoint.error`
- `restore.pid`
- `logs/criu-dump.log`
- `logs/criu-restore.log`

To preserve that behavior safely, the broker keeps the authoritative checkpoint
root-owned under `/var/lib/snapshotd/...` and can also emit a user-readable
export copy of images/logs. A downstream client may mirror those exported
artifacts into its own runtime directory for inspection and local bookkeeping.

Concretely, the authoritative checkpoint lives under the private `jobs/` tree,
while only the compatibility copy is published under `exports/`. Restore always
uses the private `jobs/` path and never consumes the exported copy.

The important boundary is:

- restore uses the broker-owned checkpoint, not the exported compatibility copy
- the exported copy exists only for diagnostics and compatibility

## Retention, Cleanup, And Eviction

The broker has a different cleanup problem than rootless generated-bootstrap
autosnapshots.

For privileged brokered flows, the authoritative checkpoints are consolidated
under a root-owned global state root:

```text
/var/lib/snapshotd/
  <uid>/
    jobs/
      <job-id>/
        checkpoints/
          <checkpoint-id>/
    exports/
      <job-id>/
        <checkpoint-id>/
```

That means retention needs both:

- a global machine budget for `/var/lib/snapshotd`
- optional per-user quotas under `/var/lib/snapshotd/<uid>/...`

The root-owned `jobs/` tree is the authoritative restore input. The user-facing
`exports/` tree is a compatibility copy and is pruned together with the
authoritative checkpoint it mirrors.

### Persisted Metadata

Each authoritative checkpoint now carries explicit retention metadata in
broker-owned metadata rather than relying on filesystem access times.

Implemented fields:

- `created_at`
- `last_restored_at`
- `restore_count`
- `size_bytes`
- `job_id`
- `checkpoint_id`

Recommended derived values:

- total bytes per job
- total bytes per UID subtree
- total bytes under the entire state root

`last_restored_at` and `restore_count` should be updated only after a
successful brokered restore. Filesystem `atime` should not be used.

### Implemented Knobs

The broker exposes retention controls in daemon-owned config in
`/etc/snapshotd/snapshotd.conf`:

- `max_checkpoint_age_seconds`
- `min_keep_checkpoints_per_job`
- `max_keep_checkpoints_per_job`
- `max_bytes_per_user`
- `max_bytes_total`

These knobs are intentionally daemon-owned. Unprivileged callers should not be
able to weaken the host retention policy through request fields.

### Eviction Order

Retention is applied in this order:

1. keep the job's latest checkpoint and the request's current checkpoint as a floor
2. keep enough newest checkpoints to satisfy `min_keep_checkpoints_per_job`
3. delete checkpoints older than `max_checkpoint_age_seconds`
4. enforce `max_keep_checkpoints_per_job`
5. if still over a per-user or global byte budget, evict additional checkpoints
   until back under budget

When budget enforcement requires a choice, the eviction priority should be:

1. oldest `last_restored_at`
2. lowest `restore_count`
3. largest `size_bytes`
4. oldest `created_at`

This gives the operator all three desired controls:

- age
- frequency of use
- total space consumed

### Operational Model

Cleanup currently runs opportunistically after successful checkpoint and
restore operations. That keeps the root-owned state bounded without exposing
retention control to unprivileged callers.

All cleanup decisions operate only on broker-owned metadata and broker-owned
paths. User workspaces and exported compatibility mirrors are not the source of
truth for deletion decisions.

### Relationship To `snapshot`

This broker policy is distinct from rootless autosnapshot cleanup:

- `snapshot` rootless autosnapshots are per-runtime-dir user state
- `snapshotd` authoritative checkpoints are consolidated root-owned machine
  state

The two systems therefore need different quota scopes:

- per-runtime-dir quotas for rootless autosnapshots
- global and per-UID quotas for broker-owned checkpoints

## Fixed CRIU Invocation

The worker constructs CRIU argv internally. Users do not get a raw CRIU command
tunnel through the socket API.

Current fixed dump path:

```text
<criu> dump --no-default-config -t <pid> \
  --images-dir <root-owned images dir> \
  --work-dir <root-owned work dir> \
  --log-file <root-owned dump log> \
  --leave-running \
  --shell-job
```

Current fixed restore path:

```text
<criu> restore --no-default-config \
  --images-dir <root-owned images dir> \
  --work-dir <root-owned restore work dir> \
  --log-file <root-owned restore log> \
  --pidfile <root-owned restore pidfile> \
  --shell-job -d
```

The public `snapshotctl restore` path defaults to the host-PID restore shown
above. Pid-namespace restore is still supported, but only as an explicit,
broker-controlled mode for callers that specifically need it.

For pid-namespace flows the worker swaps the base binary for `criu-ns` but
still keeps the pid-namespace orchestration inside the broker boundary. The
worker creates the pid namespace, runs CRIU restore there with
broker-controlled images/work/log paths, discovers the restored local PID, and
resolves that back to the host PID before writing the broker-owned
`restore.pid`.

The only passthrough CRIU options are a small allowlist needed by the runtime,
for example:

- `--ghost-limit`
- `--link-remap`
- `--tcp-established`
- `--tcp-close`
- `--file-locks`

Dangerous CRIU surfaces such as `--config`, `--action-script`, `--exec-cmd`,
arbitrary image/work directories, and environment-driven config loading are not
user-controllable through the broker API.

The worker also clears the environment before `execve()` and supplies only a
small fixed environment (`PATH`, `LANG`). That prevents ambient variables such
as `CRIU_CONFIG_FILE` from influencing the privileged CRIU process.

This matters because CRIU configuration is not just CLI-driven. CRIU documents
that it reads default config files plus `CRIU_CONFIG_FILE` unless
`--no-default-config` is used:

- CRIU configuration files: <https://www.criu.org/Configuration_files>

CRIU also documents hook execution through `--action-script` / RPC
notifications:

- CRIU action scripts: <https://criu.org/Action_scripts>

Those surfaces stay broker-only.

## Why This Is Safe To Hand To IT

The trust boundary is intentionally small:

- The unprivileged CLI cannot ask for arbitrary root commands.
- The daemon accepts only a small semantic API.
- The control protocol is length-prefixed and capped, so one socket client
  cannot force unbounded message-buffer allocation in the root daemon.
- Unknown request fields are rejected, so there is no hidden unrestricted "raw
  CRIU args" tunnel.
- Checkpoint identifiers and job identifiers are validated as simple safe IDs.
- Checkpoint images live in root-owned directories created by the broker.
- Restore consumes only broker-created checkpoint directories, not
  user-supplied image paths.
- Runtime compatibility exports exist for the user workflow, but they are not
  the restore authority.

If a user is compromised, the attacker gets the authority of that user inside
the managed-job API, not general root authority.

## Comparison To Docker And Podman

The right comparison is "same security pattern, different object model."

We are **not** cloning Docker or Podman's exact internal stack. We are copying
their high-level safety model:

- callers operate on managed objects, not arbitrary host PIDs
- a privileged management/runtime layer owns CRIU integration
- CRIU is behind that management layer instead of being exposed directly to
  users

### Docker

Docker exposes checkpoint/restore in terms of container identifiers:

- `docker checkpoint create [OPTIONS] CONTAINER CHECKPOINT`
- Docker docs: <https://docs.docker.com/reference/cli/docker/checkpoint/create/>

Docker also documents checkpoint/restore as a CRIU-backed feature:

- Docker checkpoint docs: <https://docs.docker.com/reference/cli/docker/checkpoint/>

That means the caller does not get "run CRIU on any host PID"; the caller asks
Docker to checkpoint a managed container object.

### Podman

Podman documents checkpoint and restore in terms of container names/IDs, not
host PIDs:

- Podman checkpoint docs: <https://docs.podman.io/en/v4.7.2/markdown/podman-container-checkpoint.1.html>
- Podman restore docs: <https://docs.podman.io/en/latest/markdown/podman-container-restore.1.html>

Podman also documents that it relies on an OCI runtime such as `runc` or
`crun`, which is the layer that interfaces with the operating system:

- Podman overview: <https://docs.podman.io/>

### What Is The Same, And What Is Different?

Same:

- privileged checkpoint/restore is hidden behind a narrow manager/runtime
- callers name managed objects or their own runtime process
- CRIU stays behind that boundary

Different:

- Docker/Podman manage containers
- `snapshotd` manages checkpoint jobs created through `snapshotctl run` or
  through a peer-owned self-checkpoint bridge

So the precise answer for IT is:

> We are using the same privilege-separation pattern as Docker and Podman, but
> applied to managed checkpoint jobs instead of containers.

## Systemd Packaging

The Debian package installs:

- `snapshotctl` in `/usr/bin`
- `snapshotd` and `snapshot-worker` in `/usr/libexec/snapshotd`
- `snapshotd.socket` and `snapshotd.service`
- a tmpfiles rule that creates `/var/lib/snapshotd`

The package intentionally does not depend on a distro `criu` package. Instead,
it assumes the site provisions CRIU separately at `/usr/local/sbin/criu` and
`/usr/local/sbin/criu-ns`. The maintainer scripts fail the install if either
executable is missing so the host does not silently end up with an
installed-but-nonfunctional broker.

Post-install behavior:

- creates the `snapshot-users` system group if missing
- creates `/var/lib/snapshotd`
- reloads systemd
- enables and starts `snapshotd.socket`

This makes the host ready to use the broker without granting direct sudo access
to CRIU itself.

## Service Sandboxing

The Debian unit intentionally avoids aggressive systemd sandboxing for the
daemon itself.

Reason:

- `snapshotd` can launch managed user jobs
- those managed jobs inherit systemd service restrictions from the daemon
- options like `NoNewPrivileges=`, `ProtectHome=`, `PrivateTmp=`,
  `RestrictAddressFamilies=`, and aggressive mount sandboxing can break normal
  workloads or CRIU itself once inherited by the managed job

So the primary security boundary here is the broker API and fixed CRIU
invocation policy, not a heavily sandboxed long-lived systemd unit.
