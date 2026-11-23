#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Install the userspace MD1 gate tools. Use --root for staging into an archive.
set -eu

SRC=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=/
case "${1:-}" in
	--root) ROOT=${2:?missing staging root}; shift 2 ;;
	'') ;;
	*) echo "usage: $0 [--root <dir>]" >&2; exit 2 ;;
esac

install -d -m 0755 "$ROOT/usr/local/lib/mt6893-modem" \
		  "$ROOT/usr/local/sbin" \
		  "$ROOT/usr/share/doc/mt6893-modem-bringup"
for f in "$SRC"/*.py; do
	install -m 0644 "$f" "$ROOT/usr/local/lib/mt6893-modem/"
done
install -m 0755 "$SRC/mt6893-modem-fs-runner" "$ROOT/usr/local/sbin/"
install -m 0755 "$SRC/mt6893-modem-bringup" "$ROOT/usr/local/sbin/"
install -m 0644 "$SRC/README.md" "$ROOT/usr/share/doc/mt6893-modem-bringup/"

echo "installed mt6893 modem userspace tools into $ROOT"
