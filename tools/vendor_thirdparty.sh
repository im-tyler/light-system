#!/usr/bin/env bash
# Vendor third-party dependencies for the standalone prototype.
#
# meshoptimizer is cloned (not committed) into prototype/thirdparty/meshoptimizer.
# The commit is pinned to the version the builder is validated against; the
# provenance matching between base clusters and clod level-0 clusters depends
# on clusterizer/clusterlod behavior, so do not bump this blindly.
set -euo pipefail

MESHOPT_PIN=c645e49d40416466f6b347ea69cd9b96c9e532f4
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MESHOPT_DIR="$ROOT/prototype/thirdparty/meshoptimizer"

if [ -d "$MESHOPT_DIR/.git" ]; then
    current="$(git -C "$MESHOPT_DIR" rev-parse HEAD)"
    if [ "$current" = "$MESHOPT_PIN" ]; then
        echo "meshoptimizer already at pinned commit $MESHOPT_PIN"
        exit 0
    fi
    echo "updating meshoptimizer $current -> $MESHOPT_PIN"
    git -C "$MESHOPT_DIR" fetch --depth 1 origin "$MESHOPT_PIN"
    git -C "$MESHOPT_DIR" checkout --detach "$MESHOPT_PIN"
    exit 0
fi

echo "cloning meshoptimizer at pinned commit $MESHOPT_PIN"
mkdir -p "$MESHOPT_DIR"
rm -rf "$MESHOPT_DIR"
git clone https://github.com/zeux/meshoptimizer.git "$MESHOPT_DIR"
git -C "$MESHOPT_DIR" fetch --depth 1 origin "$MESHOPT_PIN"
git -C "$MESHOPT_DIR" checkout --detach "$MESHOPT_PIN"
echo "done"
