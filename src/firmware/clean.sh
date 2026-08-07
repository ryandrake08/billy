#!/usr/bin/env bash
# Restores src/firmware/ to a pristine checkout: removes every ESP-IDF-generated and
# macOS sidecar artifact (all gitignored — nothing here is tracked). Regenerated
# on the next `idf.py set-target` / `idf.py build` from sdkconfig.defaults and
# idf_component.yml.
#
# build/, .cache/, and managed_components/ are symlinks onto local (APFS) storage, not
# this NFS-mounted checkout -- macOS's NFS client caps at NFSv4.0 and can never store
# xattrs natively, so anything that touches those trees here picks up AppleDouble ._*
# sidecars, which desyncs the component manager's fetch-time vs. verify-time hash of a
# git-sourced component. Clearing them in place keeps the symlinks (and the local
# storage they point at) intact instead of recreating the dirs back on NFS.
set -euo pipefail
cd "$(dirname "$0")"

for d in build .cache managed_components; do
    if [ -L "$d" ]; then
        target=$(readlink "$d")
        rm -rf "$target"
        mkdir -p "$target"
    else
        rm -rf "$d"
    fi
done
rm -f dependencies.lock sdkconfig sdkconfig.old
find . -name '._*' -delete
