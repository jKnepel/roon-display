#!/usr/bin/env bash
set -euo pipefail
profile=${1:?Usage: scripts/package-tizen.sh CERTIFICATE_PROFILE}
root_dir=$(cd "$(dirname "$0")/.." && pwd)
"$root_dir/scripts/build-tizen.sh"
mkdir -p "$root_dir/dist/tizen"
tizen package -t wgt -s "$profile" -o "$root_dir/dist/tizen" -- "$root_dir/packaging/tizen/.buildResult"
echo "Signed WGT: $root_dir/dist/tizen"
