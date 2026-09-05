#!/bin/sh
set -u

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
"$root/tools/install_dependencies.sh" || exit $?

exec /usr/bin/env python3 "$root/tools/build.py" "$@"
