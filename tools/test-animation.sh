#!/usr/bin/env bash
set -euo pipefail

# The retained-core suite (layer tree, animatable channels, transactions,
# geometry, hit testing, and the render PLANNER) compiled in isolation from the
# GPU/FFI owners, so it runs on the VM as well as LLVM. The full package cannot
# run on the VM: the extracted renderer's nested NativeState (BindGroupState)
# cannot be boxed by the VM callback-state representation. Everything asserted
# here is a value, so nothing here needs a window.
#
#   tools/test-animation.sh vm
#   tools/test-animation.sh llvm

root="$(cd "$(dirname "$0")/.." && pwd)"
backend="${1:-vm}"
case "$backend" in
    vm|llvm|hybrid) ;;
    *) printf 'usage: %s [vm|llvm|hybrid]\n' "$0" >&2; exit 2 ;;
esac

work="$(mktemp -d "${TMPDIR:-/tmp}/kcoreanimation-animation.XXXXXX")"
trap 'rm -rf "$work"' EXIT
mkdir "$work/app"
cat > "$work/package.kira" <<'MANIFEST'
Package AnimationKik {
    let version = "0.1.0"
    let kira = "0.1.0"
    let kind = PackageKind.App
    let defaults = Defaults { executionMode: Backend.Vm, buildTarget: BuildTarget.Host }
}
MANIFEST

# The no-FFI core: everything up to and including the render planner. The submit
# adapter (app/Compositor/Compositor.kira), the batch (app/Render) and the text/
# icon content bindings are excluded — they own native state.
cp "$root/app/Identifiers.kira" "$work/app/"
cp "$root/app/Geometry/"*.kira "$work/app/"
cp "$root/app/Animatable/"*.kira "$work/app/"
cp "$root/app/Transaction/"*.kira "$work/app/"
cp "$root/app/Layer/"*.kira "$work/app/"
cp "$root/app/Input/"*.kira "$work/app/"
cp "$root/app/Compositor/RenderPlan.kira" "$work/app/"
cp "$root/app/Compositor/Planner.kira" "$work/app/"
cp "$root/app/Timing/Motion/"*.kira "$work/app/"

# The test cases and the tests-kik harness, with the cross-package import dropped
# (in isolation they share one package namespace).
for source in "$root/tests/animation_kik/app/"*.kira; do
    sed '/^import KCoreAnimation$/d' "$source" > "$work/app/$(basename "$source")"
done

kira test --backend "$backend" "$work"
