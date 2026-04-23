#!/bin/bash
#
# Regenerate charts/ from benchmarks.csv and push to
# https://liboil-bench.netlify.app.
#
# Usage:
#   ./deploy.sh                              # plot + deploy
#   LIBOIL_REPO=/path/to/liboil ./deploy.sh  # also fill in commit author/subject

set -o pipefail

SWEEP_DIR=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
cd "$SWEEP_DIR" || exit 2

python3 plot.py || exit $?
netlify deploy --prod --dir=charts
