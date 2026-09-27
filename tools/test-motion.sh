#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
backend="${1:-vm}"
case "$backend" in
    vm|llvm|hybrid) ;;
    *) printf 'usage: %s [vm|llvm|hybrid]\n' "$0" >&2; exit 2 ;;
esac

# Compile the production math and its assertions without GPU native owners.
# The VM cannot box the renderer's nested NativeState storage.
work="$(mktemp -d "${TMPDIR:-/tmp}/kcoreanimation-motion.XXXXXX")"
trap 'rm -rf "$work"' EXIT
mkdir "$work/app"
cat > "$work/package.kira" <<'MANIFEST'
Package MotionKik {
    let version = "0.1.0"
    let kira = "0.1.0"
    let kind = PackageKind.App
    let defaults = Defaults { executionMode: Backend.Vm, buildTarget: BuildTarget.Host }
}
MANIFEST

for source in "$root/tests/motion_kik/app/"*.kira; do
    sed '/^import KCoreAnimation$/d' "$source" > "$work/app/$(basename "$source")"
done
cp "$root/app/Timing/Motion/"*.kira "$work/app/"
kira test --backend "$backend" "$work"
