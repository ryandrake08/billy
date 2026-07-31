#!/usr/bin/env bash
# Restores firmware/ to a pristine checkout: removes every ESP-IDF-generated and
# macOS sidecar artifact (all gitignored — nothing here is tracked). Regenerated
# on the next `idf.py set-target` / `idf.py build` from sdkconfig.defaults and
# idf_component.yml.
set -euo pipefail
cd "$(dirname "$0")"

rm -rf build .cache managed_components
rm -f dependencies.lock sdkconfig sdkconfig.old
find . -name '._*' -delete
