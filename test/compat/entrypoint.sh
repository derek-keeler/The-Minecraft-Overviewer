#!/bin/sh
# Container entry for the Ubuntu legs of the compatibility matrix.
#
# Mounts (see run_matrix.py):
#   /repo         the Overviewer checkout (read-only)
#   /worlds       staged test worlds + manifest.json (read-only)
#   /mc-versions  Minecraft client jars, <ver>/<ver>.jar (read-only)
#   /output       writable; renders + result.json land here
set -e

# Copy the read-only repo to a writable location, leaving out anything built
# on the host so the C extension is rebuilt against this container's numpy.
mkdir -p /work
rsync -a \
    --exclude '.git' \
    --exclude '.venv' \
    --exclude 'build' \
    --exclude 'overviewer_core/c_overviewer*.so' \
    --exclude 'overviewer_core/c_overviewer*.pyd' \
    /repo/ /work/

cd /work
exec python3 test/compat/render_test.py \
    --repo /work \
    --worlds /worlds \
    --versions /mc-versions \
    --output /output \
    "$@"
