#!/bin/sh
# Diff src/gfx/ against a LongRoad2 checkout (src/gfx/README.md says which
# commit it was vendored from and why each change is there).
#   scripts/gfx_diff.sh [path/to/LongRoad2]
set -u
L=${1:-$(dirname "$0")/../../LongRoad2}/src
G=$(dirname "$0")/../src/gfx
for pair in shim/ddraw:ddraw.c shim/ddraw:ddraw.h shim/ddraw:ddraw_types.h \
            shim/d3d7:d3d7.c shim/d3d7:d3d7.h shim/d3d7:d3d7_internal.h shim/d3d7:d3d7_shader.c \
            shim/d3d7:d3d7_shader.h shim/d3d7:d3d7_cmdbuf.c shim/d3d7:d3d7_d3d3_bridge.c \
            render:render.c render:render.h render:gl_thread.c render:gl_thread.h \
            glstate:gl_state_cache.c glstate:gl_state_cache.h \
            platform:activity_sampler.c platform:activity_sampler.h; do
  dir=${pair%%:*} f=${pair#*:}
  diff -u "$L/$dir/$f" "$G/$f" --label "LongRoad2/src/$dir/$f" --label "src/gfx/$f"
done
