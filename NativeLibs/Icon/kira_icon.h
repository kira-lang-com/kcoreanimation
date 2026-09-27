/* Kira UI icon engine: rasterizes an SVG icon (a practical subset — see
 * kira_icon.c) into an 8-bit coverage bitmap. Coverage-only by design: color is
 * applied at draw time as a tint (the compositor's mode-1 atlas quads), exactly
 * like text glyphs, so one rasterization serves every icon color. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stable 63-bit FNV-1a hash of the SVG text — the compositor's atlas cache key. */
int64_t kira_icon_hash(const char *svg_utf8);

/* Rasterize `svg_utf8` into a px-by-px 8-bit coverage bitmap (row-major, pitch ==
 * px), scaled from the document's viewBox with aspect preserved and centered.
 * `out_buffer` is caller-allocated (px * px bytes) and fully overwritten.
 * Returns 1 on success, 0 on parse failure (buffer cleared to 0). */
int32_t kira_icon_rasterize(const char *svg_utf8, int32_t px, uint8_t *out_buffer);

/* Where a symbol's ink sits inside its own box, as fractions of that box from
 * its top edge. A library is not one shape — a folder fills the middle of its
 * canvas, an open folder is drawn lower and shorter — so a caller placing a
 * symbol against text has to ask per SYMBOL, the way it would ask a font per
 * glyph. Returns 1 and writes both, or 0 and leaves 0..1. */
int32_t kira_icon_ink_bounds(const char *svg_utf8, double *out_top, double *out_bottom);
/* The same two numbers, one call each, for callers that cannot take a pointer,
 * plus the shape of the ink: its width over its height. */
double kira_icon_ink_top(const char *svg_utf8);
double kira_icon_ink_bottom(const char *svg_utf8);
double kira_icon_ink_aspect(const char *svg_utf8);
/* How much of its own box the drawing covers: low for a stroked symbol, high
 * for a solid one. A caller matching a symbol's weight to type needs to know
 * which it has — a solid shape has no stems, and growing it only swells it. */
double kira_icon_ink_density(const char *svg_utf8);

/* Rasterize so the symbol's INK fills the output exactly, with no canvas margin
 * on any side, optionally grown by `dilate_px`. The margin belongs to the canvas
 * a symbol was authored on and not to the symbol: left in, it pushes the glyph
 * in from the edge a run of text starts at and holds it off the line text sits
 * on, and it differs per symbol so no caller can correct for it. */
int32_t kira_icon_rasterize_ink(const char *svg_utf8, int32_t out_w, int32_t out_h, double dilate_px, uint8_t *out_buffer);

/* The same, with the coverage grown outward by `dilate_px` pixels: the WEIGHT of
 * a symbol. The library is drawn at ONE weight, so a glyph standing beside
 * heavier type has to be thickened to match its stems — a hairline outline next
 * to semibold words reads as a different, lighter object sitting near them.
 * Zero is the artwork as drawn, which is what `kira_icon_rasterize` asks for. */
int32_t kira_icon_rasterize_weighted(const char *svg_utf8, int32_t px, double dilate_px, uint8_t *out_buffer);

#ifdef __cplusplus
}
#endif
