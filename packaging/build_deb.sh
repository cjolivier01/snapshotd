#!/usr/bin/env bash
set -euo pipefail

output=""
snapshotctl=""
snapshotd=""
snapshot_worker=""
preinst=""
postinst=""
prerm=""
service_unit=""
socket_unit=""
tmpfiles_conf=""
readme=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --output)
      output="$2"
      shift 2
      ;;
    --snapshotctl)
      snapshotctl="$2"
      shift 2
      ;;
    --snapshotd)
      snapshotd="$2"
      shift 2
      ;;
    --snapshot-worker)
      snapshot_worker="$2"
      shift 2
      ;;
    --preinst)
      preinst="$2"
      shift 2
      ;;
    --postinst)
      postinst="$2"
      shift 2
      ;;
    --prerm)
      prerm="$2"
      shift 2
      ;;
    --service)
      service_unit="$2"
      shift 2
      ;;
    --socket)
      socket_unit="$2"
      shift 2
      ;;
    --tmpfiles)
      tmpfiles_conf="$2"
      shift 2
      ;;
    --readme)
      readme="$2"
      shift 2
      ;;
    *)
      echo "Unknown flag: $1" >&2
      exit 1
      ;;
  esac
done

[[ -n "$output" ]]
[[ -n "$snapshotctl" ]]
[[ -n "$snapshotd" ]]
[[ -n "$snapshot_worker" ]]
[[ -n "$preinst" ]]
[[ -n "$postinst" ]]
[[ -n "$prerm" ]]
[[ -n "$service_unit" ]]
[[ -n "$socket_unit" ]]
[[ -n "$tmpfiles_conf" ]]
[[ -n "$readme" ]]

pkgroot="$(mktemp -d)"
trap 'rm -rf "$pkgroot"' EXIT

mkdir -p \
  "$pkgroot/DEBIAN" \
  "$pkgroot/usr/bin" \
  "$pkgroot/usr/libexec/snapshotd" \
  "$pkgroot/lib/systemd/system" \
  "$pkgroot/usr/lib/tmpfiles.d" \
  "$pkgroot/usr/share/doc/snapshotd"

cp "$snapshotctl" "$pkgroot/usr/bin/snapshotctl"
cp "$snapshotd" "$pkgroot/usr/libexec/snapshotd/snapshotd"
cp "$snapshot_worker" "$pkgroot/usr/libexec/snapshotd/snapshot-worker"
cp "$preinst" "$pkgroot/DEBIAN/preinst"
cp "$postinst" "$pkgroot/DEBIAN/postinst"
cp "$prerm" "$pkgroot/DEBIAN/prerm"
cp "$service_unit" "$pkgroot/lib/systemd/system/snapshotd.service"
cp "$socket_unit" "$pkgroot/lib/systemd/system/snapshotd.socket"
cp "$tmpfiles_conf" "$pkgroot/usr/lib/tmpfiles.d/snapshotd.conf"
cp "$readme" "$pkgroot/usr/share/doc/snapshotd/README.Debian"

chmod 0755 \
  "$pkgroot/usr/bin/snapshotctl" \
  "$pkgroot/usr/libexec/snapshotd/snapshotd" \
  "$pkgroot/usr/libexec/snapshotd/snapshot-worker" \
  "$pkgroot/DEBIAN/preinst" \
  "$pkgroot/DEBIAN/postinst" \
  "$pkgroot/DEBIAN/prerm"

cat > "$pkgroot/DEBIAN/control" <<'EOF'
Package: snapshotd
Version: 0.1.0
Section: admin
Priority: optional
Architecture: amd64
Maintainer: Snapshot Bootstrap
Depends: adduser
Description: Privileged CRIU broker for managed checkpoint jobs
 A socket-activated root broker that exposes a narrow checkpoint/restore API,
 launches user jobs under managed identifiers, and confines CRIU invocation to
 a short-lived private worker with fixed arguments.
EOF

dpkg-deb --root-owner-group --build "$pkgroot" "$output" >/dev/null
