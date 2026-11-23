#!/bin/sh
# ror-build: see buildkit/ror_build.py. Runs it with the bundled Python.
d=$(dirname "$(readlink -f "$0")")
exec "$d/buildkit/python/bin/python3" -I -B "$d/buildkit/ror_build.py" "$@"
