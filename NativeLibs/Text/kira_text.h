/*
 * Kira text engine — public C ABI.
 *
 * Stage 1 (this header) provides real glyph rasterization and metrics backed by
 * FreeType. Text shaping (HarfBuzz) is layered on top in a later stage via
 * kira_text_shape_*; the API is deliberately shaped so a shaped run can replace
 * the simple left-to-right advance walk without changing call sites.
 *
 * All sizes/positions are expressed in pixels at the face's current pixel size.
 * The engine is single-threaded; callers must serialize access to a face.
 */
#ifndef KIRA_TEXT_H
#define KIRA_TEXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kira_text_engine kira_text_engine;
typedef struct kira_text_face   kira_text_face;

/* Engine lifetime. Returns NULL if the FreeType library failed to initialize. */
kira_text_engine* kira_text_engine_create(void);
void              kira_text_engine_destroy(kira_text_engine* engine);

/* Backend identity, e.g. "FreeType 2.13.3". Proves the real library is linked. */
const char* kira_text_backend_version(void);

/* Load a scalable face from a font file. face_index selects within a .ttc.
 * Returns NULL on failure (missing file, unsupported format). */
kira_text_face* kira_text_face_load(kira_text_engine* engine,
                                    const char* path,
                                    int face_index);

/* Load a face from an in-memory font blob. The engine copies the bytes, so the
 * caller may free `data` immediately after this returns. */
kira_text_face* kira_text_face_load_memory(kira_text_engine* engine,
                                           const uint8_t* data,
                                           long length,
                                           int face_index);

/* Load the Figtree variable face compiled into this library.
 *
 * The one face that exists wherever the library is linked, including a
 * wasm32-emscripten build, where there is no filesystem to name a system font
 * in. Callers that want a host face ask for one by path first and fall back to
 * this, so a face is never simply absent and text is never simply missing. */
kira_text_face* kira_text_face_load_bundled(kira_text_engine* engine);

void kira_text_face_destroy(kira_text_face* face);

/* Select the working pixel size for metrics and rasterization.
 * Returns 1 on success, 0 on failure. */
int kira_text_face_set_pixel_size(kira_text_face* face, float pixel_size, float weight);

/* Vertical line metrics in pixels at the current size. */
typedef struct {
    float ascender;    /* distance baseline -> top, positive up */
    float descender;   /* distance baseline -> bottom, positive down */
    float line_height; /* recommended baseline-to-baseline advance */
} kira_text_vmetrics;
void kira_text_face_vmetrics(const kira_text_face* face, kira_text_vmetrics* out);

/* Glyph index for a Unicode scalar value (0 == .notdef / missing). */
uint32_t kira_text_face_glyph_index(const kira_text_face* face,
                                        uint32_t codepoint);

/* Advance width (pixels) of a UTF-8 string at the current size, including
 * FreeType kerning where the face provides a legacy `kern` table. Returns 0 for
 * empty/invalid input. byte_len < 0 means NUL-terminated. */
float kira_text_measure_utf8(kira_text_face* face,
                             const char* utf8,
                             int byte_len);

/* A rasterized glyph. `buffer` is an 8-bit coverage bitmap (0..255) of
 * width*rows, row stride = pitch. It is owned by the face and only valid until
 * the next render call on the same face — copy it out (e.g. into a GPU atlas)
 * before rendering another glyph. */
typedef struct {
    int            width;
    int            rows;
    int            bearing_x;  /* left side bearing, pixels */
    int            bearing_y;  /* top bearing above baseline, pixels */
    float          advance;    /* horizontal advance, pixels */
    int            pitch;      /* bytes per buffer row (>= width) */
    const uint8_t* buffer;
} kira_text_glyph_bitmap;

/* Rasterize a glyph (by glyph index) into an anti-aliased coverage bitmap.
 * Returns 1 on success, 0 on failure. A zero-area glyph (e.g. space) succeeds
 * with width==rows==0 and a valid advance. */
/* As kira_text_face_render_glyph, with the outline shifted horizontally by
 * x_offset_26_6 (26.6 fixed point: 64 = one pixel) before it is scan-converted.
 * Both entry points render UNHINTED — see the note in kira_text.c. */
int kira_text_face_render_glyph_offset(kira_text_face* face,
                                       uint32_t glyph_index,
                                       int x_offset_26_6,
                                       kira_text_glyph_bitmap* out);

int kira_text_face_render_glyph(kira_text_face* face,
                                uint32_t glyph_index,
                                kira_text_glyph_bitmap* out);

/* Decode the next UTF-8 scalar starting at *index (0-based byte offset into a
 * buffer of `len` bytes). Advances *index past the consumed bytes and writes
 * the scalar to *codepoint. Returns 1 if a scalar was produced, 0 at end of
 * input. Invalid bytes decode to U+FFFD and advance by one byte. */
int kira_text_utf8_next(const char* s, int len, int32_t* index, uint32_t* codepoint);

/* Decode a whole NUL-terminated UTF-8 string into up to `max` int32 codepoints
 * (written to `out`); returns the number decoded. */
int kira_text_decode_codepoints(const char* s, int32_t* out, int max);

/* Diagnostic: run the full FreeType pipeline (face load -> sizing -> glyph index
 * -> rasterization -> measurement) against a real on-disk font and return a
 * human-readable one-line report of the actual measured values. `font_path` may
 * be NULL/empty to auto-discover a common system font. On any failure the report
 * begins with "kira-text: ERROR". The returned string is valid until the next
 * probe call. This exercises real behavior; it is not a success marker. */
const char* kira_text_probe_report(const char* font_path);

/* Layout helpers over a (path,size) face cache of their own, so measured
 * advances match what the caller later rasterizes. `font_path` may be
 * NULL/empty to auto-discover. Both return 0 if no face is available. */
double kira_text_measure_run(const char* font_path, const char* utf8, double pixel_size, double weight);
double kira_text_line_height(const char* font_path, double pixel_size, double weight);
/* The face's own ascent and descent at this size: baseline to top, and baseline
 * to bottom (positive down). A caller that places a run — or places anything ON
 * that run's baseline — needs the same two numbers the rasterizer uses, or the
 * two disagree about where the baseline is. */
double kira_text_ascent(const char* font_path, double pixel_size, double weight);
double kira_text_descent(const char* font_path, double pixel_size, double weight);
double kira_text_run_ink_left(const char* font_path, const char* utf8, double pixel_size, double weight);
double kira_text_run_ink_right(const char* font_path, const char* utf8, double pixel_size, double weight);
double kira_text_run_ink_top(const char* font_path, const char* utf8, double pixel_size, double weight);
double kira_text_run_ink_bottom(const char* font_path, const char* utf8, double pixel_size, double weight);

#ifdef __cplusplus
}
#endif

#endif /* KIRA_TEXT_H */
