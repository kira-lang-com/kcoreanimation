# 26.10.4

**Project:** KCoreAnimation  
**Release week:** 2026-10-26 – 2026-11-01

## Highlights

The first source release of KCoreAnimation includes:

- The `UiBatch` compositor and its paint, material, text, icon, and texture declarations.
- Motion primitives for springs, decay, scrolling, pull, glow, and transitions.
- Shader sources and native text, icon, and state-store integration.


## Changed

- Added `textImage` and `TextImage`: a line of text set in the default face and drawn as coverage on the CPU, for a caller that paints pixels itself, such as a boot screen before there is a GPU to draw with.
- Added `UiBatch.setTargetOrigin`: a batch presented into a texture can be drawn at an offset inside a texture larger than its drawable, through Ember's pass draw area. A compositor uses it to keep part of the screen unused.
- Scoped raw-pointer and native-state operations in the `UiBatch` compositor, text and symbol rendering, and runtime input handling.

## Known limitations

- The legacy UI Foundation layer-to-render bridge remains under migration.
