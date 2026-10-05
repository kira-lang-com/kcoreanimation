# KCoreAnimation

KCoreAnimation is the extraction seam for Kira's rendering and motion core. It
owns the batched `UiBatch` compositor, its text and icon content support, the
material data used by that compositor, the shader sources and generated shader
artifacts, and the numeric motion implementation formerly published by
`KiraUIMotion`.

The package depends only on `KiraGraphics` and `KiraLayout`. It deliberately has
no dependency on `KiraUIFoundation` or `KiraUIMotion`: motion declarations are
copied into `app/Timing/Motion` without renaming their existing symbols.

## Owned extraction

- `app/Render/UiBatch*.kira` exports the existing batched backend contract,
  including `UiBatch` and its native-state store path.
- `app/Content/` carries the existing paint, material, text, icon-support and
  texture declarations used by the batch.
- `app/Timing/Motion/` carries all existing motion declarations and public math
  names: springs, presets, decay, scrolling, pull, glow and transitions.
- `NativeLibs/` contains the required text, icon and state-store C seams and the
  selected FreeType and HarfBuzz source inputs. `Shaders/` and `generated/`
  contain the source and deployable generated artifacts consumed by the batch.
- `app/Legacy/` is an explicitly temporary integration copy of the remaining
  UI Foundation declarations. It preserves their original names so the old
  renderer can compile while the retained Layer-to-render bridge is migrated.
  It is not a rebuilt retained architecture.

The old `ui-foundation` and `ui-motion` source trees remain untouched. Run
`tools/check-extraction.sh` to verify the provenance, package boundary, source
layout and artifact hygiene.

## Verification

```text

These commands want **`kk`**, the native frontend of the
[Kira Language Framework](https://github.com/kira-lang-com/klf-kira), which
`klf build .` produces in that repository. `kk`'s binary is also called `kira`,
so the two are told apart by which one is on your `PATH`, not by the name you
type. The oracle compiler from
[kira-lang-com/kira](https://github.com/kira-lang-com/kira) aborts partway
through semantic analysis on this codebase.

kira check . --backend vm
kira check . --backend llvm
bash tools/test-motion.sh vm
kira test --backend llvm tests/motion_kik
kira lint
tools/check-extraction.sh
```

The motion tests are numeric and window-free. A graphics surface still needs a
host backend and an on-screen integration check; this extraction does not claim
that the temporary legacy bridge is the final Layer renderer.

`tools/test-motion.sh` compiles the production motion sources and the same
assertions in an isolated package so VM tests do not import GPU native owners.
The LLVM command above also verifies linkage through the full package.
