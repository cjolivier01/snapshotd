# snapshotd

`snapshotd` is a small privileged CRIU broker for systems that need
checkpoint/restore without handing users direct `sudo criu` access.

The design goal is narrow privilege separation:

- unprivileged users call `snapshotctl`
- a root `snapshotd` daemon authenticates them over a Unix socket
- a short-lived `snapshot-worker` invokes CRIU with a fixed allowlist

That is the same high-level safety pattern used by Docker and Podman: callers
operate on managed objects through a privileged runtime boundary instead of
getting a raw CRIU command surface.

Current downstream use case: the `snapshot` Python runtime can use `snapshotd`
for `sudo=True` checkpoint/restore flows, but this repo is intentionally kept
independent so the broker can be built, reviewed, packaged, and audited on its
own.

## Why It Exists

Running CRIU as root is often necessary, but letting users invoke `sudo criu`
directly creates the wrong security boundary. It exposes dangerous CRIU
surfaces such as arbitrary target selection, config loading, action scripts,
restore inputs, and other privileged side effects.

`snapshotd` exists to replace that with a small semantic API:

- `run`
- `status`
- `checkpoint`
- `restore`

The daemon authenticates callers with `SO_PEERCRED`, stores broker-owned state
under `/var/lib/snapshotd`, and never exposes a raw CRIU argv tunnel.

## Design

- Main design doc: [docs/safe-root-criu-broker-design.md](docs/safe-root-criu-broker-design.md)
- Debian packaging notes: [packaging/debian/usr/share/doc/snapshotd/README.Debian](packaging/debian/usr/share/doc/snapshotd/README.Debian)

The short version for IT/security review:

- users do not get `sudo criu`
- users do not get a general-purpose privileged wrapper
- restore consumes only broker-created checkpoint state
- the worker clears the environment and forces `--no-default-config`
- the broker follows the same privilege-separation model used by Docker and Podman, but for managed checkpoint jobs instead of containers

## Layout

- `src/csrc/`: C++ daemon, worker, client, and protocol code
- `tests/csrc/`: Bazel C++ unit/integration tests
- `packaging/`: Debian packaging and systemd units
- `docs/`: design documentation

## Prerequisites

- Bazel
- a Linux host with systemd for packaged installation
- CRIU provisioned separately at `/usr/local/sbin/criu`
- the CRIU namespace helper provisioned separately at `/usr/local/sbin/criu-ns`

This package intentionally does not depend on a distro `criu` package.

## Build

Build the binaries:

```bash
bazel build //src/csrc:snapshotctl //src/csrc:snapshotd //src/csrc:snapshot-worker
```

Build the Debian package:

```bash
bazel build //:snapshotd_deb
```

There is also a small top-level `Makefile` for common workflows:

```bash
make
make debug
make release
make install
make clean
make distclean
```

## Test

Run the C++ test suite:

```bash
bazel test //tests/csrc:protocol_store_test //tests/csrc:daemon_integration_test
```

The integration test exercises the broker protocol, managed jobs, fixed worker
invocation, worker timeouts, response-write disconnect handling, and rejection
of several malicious inputs.

## Install

Build the package first:

```bash
bazel build //:snapshotd_deb
```

Or use the Makefile wrapper:

```bash
make install
```

Then install it. Copying the `.deb` out of Bazel's output tree avoids `_apt`
permission warnings:

```bash
cp bazel-bin/snapshotd_0.1.0_amd64.deb /tmp/
chmod 0644 /tmp/snapshotd_0.1.0_amd64.deb
sudo apt install /tmp/snapshotd_0.1.0_amd64.deb
```

Package install will fail if `/usr/local/sbin/criu` or
`/usr/local/sbin/criu-ns` is missing or not executable.

The package installs:

- `/usr/bin/snapshotctl`
- `/usr/libexec/snapshotd/snapshotd`
- `/usr/libexec/snapshotd/snapshot-worker`
- `/etc/snapshotd/snapshotd.conf`
- `snapshotd.socket`
- `snapshotd.service`

Post-install, the package:

- creates the `snapshot-users` system group if needed
- creates `/var/lib/snapshotd`
- enables and starts `snapshotd.socket`

The packaged daemon reads its runtime configuration from:

- `/etc/snapshotd/snapshotd.conf`

That file controls daemon-owned settings such as:

- `state_dir`
- `criu_bin`
- `criu_ns_bin`
- `worker_timeout_seconds`

Socket settings such as `/run/snapshotd.sock` and the `snapshot-users` group
remain systemd socket-unit settings, not daemon config-file settings.

To allow a user to access the broker socket:

```bash
sudo usermod -aG snapshot-users <user>
```

Start a fresh login shell afterward so the new group is active.

## Uninstall

Remove the package:

```bash
sudo apt remove snapshotd
```

If you also want to purge package-managed configuration:

```bash
sudo apt purge snapshotd
```

Optional manual cleanup after uninstall:

```bash
sudo rm -rf /var/lib/snapshotd
```

Whether to remove the `snapshot-users` group depends on whether anything else
on the host still relies on it.

## Smoke Test

After install, a simple managed-job flow looks like:

```bash
snapshotctl run -- /bin/sleep 120
snapshotctl status <job-id>
snapshotctl checkpoint <job-id>
snapshotctl restore <job-id> <checkpoint-id>
```

`snapshotctl restore` uses the host-PID restore path by default. If a workflow
explicitly needs pid-namespace restore, use:

```bash
snapshotctl restore --namespace-restore <job-id> <checkpoint-id>
```

Use `systemctl status snapshotd.socket --no-pager` to verify the socket is
active. `snapshotd.service` is socket-activated and may be inactive until the
first client request.
