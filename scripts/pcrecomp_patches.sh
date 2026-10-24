#!/bin/sh
# Put ../pcrecomp on branch ht2: [pcrecomp].base_commit from
# config/hardtruck2.toml plus patches/pcrecomp/*.patch (git am). Refuses to
# touch a checkout with uncommitted changes.
#   scripts/pcrecomp_patches.sh [path/to/pcrecomp]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
P=${1:-${PCRECOMP:-$ROOT/../pcrecomp}}
BASE=$(sed -n 's/^base_commit *= *"\(.*\)".*/\1/p' "$ROOT/config/hardtruck2.toml")
cd "$P"
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "pcrecomp at $P has uncommitted changes; commit or stash them first" >&2; exit 1
fi
if git rev-parse -q --verify ht2 >/dev/null; then
  echo "branch ht2 exists: $(git log --oneline -1 ht2)"
  echo "to rebuild it: git -C $P branch -m ht2 ht2-old && $0"
  exit 0
fi
git switch -c ht2 "$BASE"
git am "$ROOT"/patches/pcrecomp/*.patch
git log --oneline "$BASE"..HEAD
