#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
foundation="$root/../ui-foundation"
motion="$root/../ui-motion"

fail() {
    printf 'extraction check: %s\n' "$1" >&2
    exit 1
}

test -f "$root/package.kira" || fail "package.kira is missing"
test -f "$root/linter.kira" || fail "linter.kira is missing"
test -f "$foundation/package.kira" || fail "ui-foundation source tree is missing"
test -f "$motion/package.kira" || fail "ui-motion source tree is missing"

grep -q 'Package KCoreAnimation' "$root/package.kira" || fail "wrong package name"
if grep -Eq 'KiraUIFoundation|KiraUIMotion' "$root/package.kira"; then
    fail "extracted package has a forbidden UI dependency"
fi
if grep -R -n -E '^import (KiraUIFoundation|KiraUIMotion)$' "$root/app" "$root/tests" 2>/dev/null; then
    fail "an extracted source still imports an old package"
fi

for file in \
    UiBatch.kira UiBatchDraw.kira UiBatchForeign.kira UiBatchGlass.kira \
    UiBatchGlassCache.kira UiBatchGlassDraw.kira UiBatchGlassTargets.kira \
    UiBatchGlyphs.kira UiBatchPipelines.kira UiBatchQuads.kira UiBatchRuns.kira \
    UiBatchShaders.kira UiBatchSlots.kira UiBatchState.kira UiBatchStats.kira; do
    test -f "$root/app/Render/$file" || fail "missing render source $file"
done

for file in MotionSpring.kira MotionPresets.kira MotionDecay.kira MotionScroll.kira \
    MotionPull.kira MotionGlow.kira MotionTransition.kira MotionMetaball.kira; do
    test -f "$root/app/Timing/Motion/$file" || fail "missing motion source $file"
done

for file in Paint.kira Materials.kira Text.kira TextInputHelpers.kira TextureView.kira; do
    test -f "$root/app/Content/$file" || fail "missing content source $file"
done

test -f "$root/NativeLibs/Text/kira_text.c" || fail "missing text native seam"
test -f "$root/NativeLibs/Icon/kira_icon.c" || fail "missing icon native seam"
test -f "$root/NativeLibs/StateStore/kira_state_store.c" || fail "missing state store shim"
test -f "$root/Shaders/UiBatch.ksl" || fail "missing batch shader source"
test -f "$root/generated/shaders/UiBatch.frag.glsl" || fail "missing generated shader artifact"

# Verification creates build caches, and the destination may be a repository.
# Check distributable sources without inspecting generated working directories.
source_files() {
    find "$root" -type d \( -name .git -o -name .kira-build -o -name .codex -o -name '.*-objects' \) -prune -o "$@"
}
if source_files -type f \( -name '*.o' -o -name '*.tmp' \) -print -quit | grep -q .; then
    fail "native object cache copied into extraction"
fi
while IFS= read -r -d '' file; do
    lines="$(wc -l < "$file")"
    if [ "$lines" -ge 700 ]; then
        fail "$(basename "$file") reaches the 700-line ceiling"
    fi
done < <(source_files -type f \( -name '*.kira' -o -name '*.ksl' \) -print0)

grep -q '^class UiBatch' "$root/app/Render/UiBatch.kira" || fail "UiBatch declaration is missing"
# These files were extracted from the old UI packages, but KCoreAnimation owns
# them now. Do not require byte-for-byte equality with the legacy copies: doing
# so rejects intentional migrations to newer Kira syntax and APIs (for example
# lerp/clamp, unary negation and compound assignment). The source trees above
# remain as provenance references; the package-boundary and owned-file checks
# are the invariants that matter after extraction.
for file in \
    "$root/app/Content/Materials.kira" \
    "$root/app/Content/Paint.kira" \
    "$root/app/Timing/Motion/MotionSpring.kira" \
    "$root/app/Timing/Motion/MotionScroll.kira"; do
    test -s "$file" || fail "owned extracted source is missing or empty: $(basename "$file")"
done

printf 'extraction check: ok\n'
