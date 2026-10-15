#!/usr/bin/env bash
# Applies patches/<name>/*.patch to the submodules below: fixes this fork carries in code it doesn't
# own (see FORK.md). A patch that no longer applies fails the build, so a submodule update can't
# silently drop a fix; one that's already in the submodule only warns.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)

# patches/<name> -> the submodule they apply to
declare -A submodules=(
  [enet]="third-party/moonlight-common-c/enet"
)

status=0
for name in "${!submodules[@]}"; do
  dir="$root/${submodules[$name]}"
  for patch in "$root/patches/$name"/*.patch; do
    [ -e "$patch" ] || continue
    label="patches/$name/$(basename "$patch")"
    if git -C "$dir" apply --reverse --check "$patch" 2> /dev/null; then
      echo "::warning::$label is already in ${submodules[$name]} (applied by an earlier run, or upstream has it: then remove it)"
    elif git -C "$dir" apply --check "$patch"; then
      git -C "$dir" apply "$patch"
      echo "Applied $label to ${submodules[$name]}"
    else
      echo "::error::$label no longer applies to ${submodules[$name]}; update it (see FORK.md)"
      status=1
    fi
  done
done
exit $status
