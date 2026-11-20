#!/bin/sh
# Installs these hooks in this clone's .git/hooks, so they work on every branch, including
# offline-web, where this folder doesn't exist. Run it once per clone: sh tools/git-hooks/install.sh
set -e
source_dir=$(cd "$(dirname "$0")" && pwd)
hooks_dir="$(git rev-parse --git-common-dir)/hooks"
mkdir -p "$hooks_dir"
for hook in reference-transaction pre-push never_merge_offline_web.sh; do
    cp "$source_dir/$hook" "$hooks_dir/$hook"
    chmod +x "$hooks_dir/$hook"
done
echo "Installed the never-merge-offline-web hooks in $hooks_dir"
