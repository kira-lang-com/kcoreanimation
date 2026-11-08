# 26.11.1

**Project:** KCoreAnimation  
**Release week:** 2026-11-02 – 2026-11-08

## Highlights

- The retained compositor can route eligible macOS glass on transparent, unrotated root surfaces to a native Metal-generated displacement map. Opaque coverage, complex geometry, and unsupported host configurations continue to use the existing Liquid Glass shader.

Added CPU text-image generation and offset rendering into textures for the UI compositor.

## Added

- Added native CAText underline and strikethrough drawing with five repeating patterns and optional independent colors. Decoration geometry uses measured text advances per rendered line and the same layer transforms, clip region, blend routing and font fitting as the glyph run.

- Added retained `focusKey` geometry and editing-focus hit regions to presentation-layer hit testing, independently of press and hover keys. Focus-only views no longer need fake click targets to participate in keyboard navigation.

- Added real text redaction in the glyph compositor: original source strings are retained for layout, line limits and transitions, but each displayed line draws its measured rounded placeholder bar instead of rasterizing sensitive text or its shadow.

- Added `CATextLineHeight` to native text runs, with a shared baseline pitch resolver for intrinsic measurement, wrapping, and the compositor's multiline glyph positioning. Font glyph boxes remain independent of the requested baseline distance.

- Added registered OpenType `wdth` axis support to native variable-font instances. Font-specific normalized axis coordinates are interpolated from each font's own standard/min/max design widths, with cache identities and glyph rasterization keyed by requested width so measurement and rendered outlines match.

- Added bounded, width-sensitive tightening of text cluster spacing before line-limit truncation, shared between the layout and glyph draw passes with no out-of-band clip or alpha mask.

- Added native shaped-glyph-cluster letter spacing to text run measurement, line wrapping, truncation, font fitting and rasterization. An optional tracking final advance is measured, while glyph positioning inserts spacing only at cluster boundaries so attached marks remain grouped.

- Added native Text baseline offset carrying positive/negative vertical glyph displacement through the compositor while keeping measured text height in agreement with glyph bounds.

- Added a native minimum text scale factor and shared width-measured font-size fitting for constrained line-limited text. Text layout and glyph drawing both use the fitted font size, avoiding oversized clipping when a Text can shrink rather than truncate.

- Added native `CATextTruncationMode` and Unicode-scalar head/middle/tail truncation, with width-measured ellipsis placement shared by text layout and compositor drawing. The final displayed line now truncates long unbreakable words rather than overflowing a fixed line limit.

- Extended the hover hit result to carry each observer's inverse-transformed local point and global input point for continuous hover events, independently of all press/scroll routing.

- Added independent `hoverKey` geometry on layer descriptions, retained nodes, draw/hit items and `CAHitResult.hoverKeys`. Pointer hit tests retain the complete stack of matching nested hover observers without changing click/scroll targeting, enabling correct SwiftUI-style `.onHover` callbacks.

- Added nullable, edge-specific section margin preferences to CALayoutIntent for list section measurement, including distinction between explicit default restoration and no override.

- Added optional, edge-specific row-separator visibility and tint preferences to CALayoutIntent for KiraUI's native grouped-row renderer. Presence flags distinguish an explicit default reset from no modifier.

- Added per-column grid alignment metadata to `CALayoutIntent` for KiraUI Grid's column-wide alignment resolution, alongside grid-cell anchors and unsized-axis flags.

- Added grid-cell intrinsic-sizing exclusions and explicit cell anchors to `CALayoutIntent`, consumed by KiraUI Grid's measurement and placement passes.

- Added `CALayoutIntent.layoutPriority` for toolkit-controlled compression ordering independent of flex/expansion weights.

- Added `CALayoutIntent.centersChildAtPosition` and `childCenter` as toolkit-resolved placement metadata for positioned views. Placement honors a child's measured size; the compositor remains agnostic to toolkit layout semantics.

- Added `CALayerDescription.allowsHitTesting` as a retained subtree-level input gate; it prunes pointer hit traversal without changing drawing, geometry, children or accessibility, used by KiraUI's `.allowsHitTesting(_:)`.

- Separated scroll layout-axis measurement from scroll hit-test registration with `CALayerDescription.acceptsScrollInput`. Disabled scroll surfaces keep unbounded content measurement and normal child controls while the compositor stops routing wheel and pan gestures to the scroll surface, supporting KiraUI's inherited `.scrollDisabled(_:)` behavior.

- `textImage` / `TextImage` for generating text coverage on the CPU when a GPU is not yet available, such as an early boot screen.
- `UiBatch.setTargetOrigin(x, y)` to place a batch inside a larger target texture using Ember's draw area.
- Fixed root `CALayerTree.reconcile` to honor `disablesActions` and explicit timing transactions consistently with child reconciliation. Newly mounted roots now take their initial geometry immediately; subsequent root sizing and label changes marked immediate no longer animate with KCA's default ease-out. This prevents Workspace slider widths and text from lagging behind the active Liquid Glass lens.
- Fixed animated text raster-size flicker: `drawText` now passes the font's original unscaled point size to `UiBatch.pushText`. Glyph atlas keys and font instances are determined from the authored point size and display backing scale, while the presentation transform scales the already-rasterized quads. This removes the former multiply-then-divide cancellation whose subpixel rounding could cause a font instance to alternate between neighboring raster sizes during a spring. Added two focused tests in `tests/text_animation_kik` covering that precision failure and invariant raster sizing across changing presentation scales.
- Fixed the related **vertical text jump** during scale/position animations: glyph baselines are now aligned in the text run's local coordinates and transformed continuously around the visible box center, instead of re-snapping the run's screen-space Y origin to device pixels every frame and abruptly switching anchoring near 1× scale. The authored font size and glyph raster cache remain stable. Two additional baseline-motion regressions cover scale springs crossing 1× and subpixel parent translation; the four text-animation regressions and 180 KiraUI tests pass.
- Fixed the **remaining horizontal letter jitter** from the analogous X-axis code path: `UiBatch.pushText` was still rounding the screen-space X origin on every animation frame and abruptly switching that origin to zero at precisely 1×. Both axes now use a continuously moving, center-anchored text box; glyph raster keys and subpixel phases are measured in invariant local coordinates and the existing geometry transform moves the entire run. New regressions exercise horizontal translation, scale springs crossing 1×, and the former device-pixel snap discontinuity. **7/7 text-animation, 180/180 KiraUI, and 30/30 Matter editor tests pass**, and the native editor builds and renders with the Metal offscreen backend.
- Removed three more discontinuities in animated vertical text: the final device baseline is no longer independently rounded to a full pixel; metric-based versus tight-ink baseline placement now blends continuously as an animated line box crosses its nominal height; and glyph quad bounds use `floor` instead of truncation toward zero, which had clipped the top/left pixel of glyphs in the negative local coordinates introduced by centered run transforms. Atlas UV slack preserves coverage and follows subpixel movement. Added five regression cases for incremental baseline movement, varying height, simultaneous scale/size, negative local glyph bounds, and continuous atlas UV positioning. **12/12 text-animation regressions, 180/180 KiraUI tests** pass; the updated NextEditor native build succeeds.

## Validation

- The complete animated text regression set passes 12/12, and KiraUI's full suite passes 180/180. The Project Matter editor's 30 tests and native build passed with the updated baseline calculation; Metal capture was validated separately.
