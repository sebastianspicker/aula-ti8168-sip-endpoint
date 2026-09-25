#!/bin/sh
# Keep dependencies and coordination state in the canonical work tree.
set -eu
SCRIPT_DIR=$(CDPATH='' cd "$(dirname "$0")" && pwd)
exec python3 -B "$SCRIPT_DIR/dependencies.py" "$@"
