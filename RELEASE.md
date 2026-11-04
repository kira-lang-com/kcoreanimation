# 26.11.1

**Project:** KCoreAnimation  
**Release week:** 2026-11-02 – 2026-11-08

## Highlights

Added CPU text-image generation and offset rendering into textures for the UI compositor.

## Added

- `textImage` / `TextImage` for generating text coverage on the CPU when a GPU is not yet available, such as an early boot screen.
- `UiBatch.setTargetOrigin(x, y)` to place a batch inside a larger target texture using Ember's draw area.

## Validation

- No new runtime or GPU validation was performed for this release preparation.
