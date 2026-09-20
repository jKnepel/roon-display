#!/usr/bin/env bash
set -euo pipefail
root_dir=$(cd "$(dirname "$0")/.." && pwd)
build_dir="$root_dir/build-tizen"
stage_dir="$root_dir/packaging/tizen"
command -v emcmake >/dev/null || { echo "Samsung Emscripten's emcmake is required" >&2; exit 1; }
command -v tizen >/dev/null || { echo "Tizen Studio CLI is required" >&2; exit 1; }
emcmake cmake -S "$root_dir" -B "$build_dir" -DROON_DISPLAY_BUILD_TIZEN=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target roon-display-tizen
rm -f "$stage_dir"/roon-display-tizen.js "$stage_dir"/roon-display-tizen.wasm "$stage_dir"/roon-display-tizen.worker.js
cp "$build_dir"/roon-display-tizen.js "$stage_dir"/
cp "$build_dir"/roon-display-tizen.wasm "$stage_dir"/
[ ! -f "$build_dir/roon-display-tizen.worker.js" ] || cp "$build_dir/roon-display-tizen.worker.js" "$stage_dir"/
cp "$root_dir/packaging/icon.svg" "$stage_dir/icon.svg"
rm -rf "$stage_dir/ui"
cp -R "$root_dir/ui" "$stage_dir/ui"
cp "$stage_dir/tizen-host.js" "$stage_dir/ui/hosts/tizen-host.js"
sed -i 's#<script src="hosts/host.js"></script>#<script src="hosts/tizen-host.js"></script>\n  <script src="hosts/host.js"></script>#' "$stage_dir/ui/index.html"
cp "$stage_dir/ui/index.html" "$stage_dir/index.html"
sed -i 's#href="styles.css"#href="ui/styles.css"#; s#src="hosts/#src="ui/hosts/#; s#src="app.js"#src="ui/app.js"#' "$stage_dir/index.html"
tizen build-web -- "$stage_dir"
echo "Tizen web build: $stage_dir/.buildResult"
