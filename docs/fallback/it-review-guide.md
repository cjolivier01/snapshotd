# IT Review Guide

The question for `snapshotd` is not "can this host run CRIU as root?" but
"what exact policy boundary stands between an unprivileged caller and that
root operation?"

- callers talk to a narrow Unix-socket API, not a general-purpose root wrapper
- the daemon authenticates the peer with `SO_PEERCRED`
- privileged checkpoint/restore only runs through a short-lived worker
- the worker builds fixed CRIU commands against broker-owned directories
- restore consumes only broker-created checkpoint state

Concrete implementation entry points:

- `snapshotd::HandleRequest`
- `snapshotd::BuildDumpCommand`
- `snapshotd::BuildRestoreCommand`
- `snapshotd::ProcessMatchesPeerSecurity`
- `snapshotd::Store`

## Docker And Podman Parallels

`snapshotd` deliberately mirrors the same high-level safety pattern used by
Docker and Podman:

- users operate on managed objects through a runtime boundary
- the privileged side owns CRIU integration
- the control socket or API must be permission-gated because the privileged side is security-sensitive

The object model is different:

- Docker and Podman manage containers
- `snapshotd` manages broker-owned checkpoint jobs identified by `job_id`

## CUDA-Checkpoint Context

CUDA-aware checkpointing does not change the need for a narrow root boundary.
NVIDIA's current documentation describes `cuda-checkpoint` as a per-process
utility keyed by PID and used together with CRIU for full checkpoints.

As of April 10, 2026, I did not find an official Docker or Podman design
document specifically focused on CUDA checkpoint root-permission usage. What
does exist is still enough to draw the comparison:

- official Docker security material about the daemon's privileged attack surface
- official Docker checkpoint/restore CLI documentation
- official Podman checkpoint/restore command documentation
- official NVIDIA CUDA checkpoint API documentation and the `cuda-checkpoint` utility documentation

## External References

- Docker checkpoint reference: https://docs.docker.com/reference/cli/docker/checkpoint/
- Docker checkpoint create: https://docs.docker.com/reference/cli/docker/checkpoint/create/
- Docker daemon security / attack surface: https://docs.docker.com/engine/security/
- Podman overview: https://docs.podman.io/en/latest/
- Podman checkpoint: https://docs.podman.io/en/latest/markdown/podman-container-checkpoint.1.html
- Podman restore: https://docs.podman.io/en/latest/markdown/podman-container-restore.1.html
- NVIDIA CUDA checkpoint driver API: https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__CHECKPOINT.html
- NVIDIA `cuda-checkpoint` utility: https://github.com/NVIDIA/cuda-checkpoint
- CRIU configuration files: https://www.criu.org/Configuration_files
- CRIU action scripts: https://criu.org/Action_scripts
