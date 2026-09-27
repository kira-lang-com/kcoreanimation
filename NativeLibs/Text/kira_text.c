/*
 * Kira text engine — FreeType-backed implementation (Stage 1).
 *
 * Real glyph rasterization and metrics. No placeholder bitmaps, no hardcoded
 * advances: every measurement and coverage bitmap comes from FreeType loading
 * the actual font outlines. HarfBuzz shaping is added in a later stage on top
 * of this rasterization core.
 */
#include "kira_text.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_OUTLINE_H
#include FT_MULTIPLE_MASTERS_H

#include <hb.h>
#include <hb-ft.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Bundled Inter variable font data (opsz 14..32, wght 100..900). */
#include "kira_inter_font.h"

/* Forward declaration: embedded Inter face loaded from the byte array above.
 * Defined further below alongside the draw-cache infrastructure. */
static kira_text_face* kira_text_embedded_face(float pixel_size, float weight);

struct kira_text_engine {
    FT_Library library;
};

struct kira_text_face {
    FT_Face        face;
    unsigned char* owned_blob; /* non-NULL when loaded from memory */
    float          pixel_size;
    float          weight;
    int            has_kerning;
    hb_font_t*     hb_font;    /* lazily created HarfBuzz font over `face` */
};

/* Lazily create (and cache) a HarfBuzz font wrapping the FreeType face. The
 * face's pixel size must already be set; HarfBuzz reads its scale from FreeType,
 * so advances come back in 26.6 pixels matching the rasterized glyphs. */
static hb_font_t* kira_text_hb_font(kira_text_face* face) {
    if (face == NULL || face->face == NULL) {
        return NULL;
    }
    if (face->hb_font == NULL) {
        face->hb_font = hb_ft_font_create_referenced(face->face);
    } else {
        /* Pick up any size change since the font was created. */
        hb_ft_font_changed(face->hb_font);
    }
    return face->hb_font;
}

/* 26.6 fixed-point -> float pixels. */
static float kira_text_f26dot6(FT_Pos value) {
    return (float)value / 64.0f;
}

/* ------------------------------------------------------------------------
 * Process-lifetime registries for engines and faces.
 *
 * The Kira side stores the engine and face handles inside UiBatchState for the
 * lifetime of a Foundation application and explicitly destroys both at runner
 * teardown. This registry remains as a process-exit safety net for abnormal or
 * non-Foundation owners that do not reach their explicit destroy path.
 *
 * Mirror the native runtime's approach (kira-zig
 * packages/kira_native_bridge/src/runtime_helpers.c —
 * kira_native_state_registry / kira_task_registry): record every engine and
 * face at creation, unlink on explicit destroy, and free the survivors from a
 * single atexit handler registered on the first create. The public ABI is
 * unchanged, and this is double-destroy safe — an explicit destroy unlinks
 * before disposing, so the atexit sweep never touches an already-freed handle.
 *
 * Ordering is load-bearing: a FreeType face belongs to its library
 * (FT_Done_Face must precede FT_Done_FreeType), so the teardown disposes every
 * face before any engine. Faces from distinct engines are independent, so a
 * single faces-then-engines pass keeps every library alive while its faces are
 * torn down. The draw-cache/embedded-face statics (g_draw_engine,
 * g_embedded_engine, and their faces) route through these same create/destroy
 * paths, so they are tracked and reclaimed too; mid-run cache eviction and
 * embedded-face reload call kira_text_face_destroy, which unlinks first, so the
 * atexit sweep never double-frees an evicted face.
 */
typedef struct KiraTextEngineNode {
    kira_text_engine* engine;
    struct KiraTextEngineNode* next;
} KiraTextEngineNode;

typedef struct KiraTextFaceNode {
    kira_text_face* face;
    struct KiraTextFaceNode* next;
} KiraTextFaceNode;

static KiraTextEngineNode* g_engine_registry = NULL;
static KiraTextFaceNode*   g_face_registry   = NULL;

#if defined(_WIN32)
#include <windows.h>
static SRWLOCK g_text_registry_lock = SRWLOCK_INIT;
static void kira_text_registry_acquire(void) { AcquireSRWLockExclusive(&g_text_registry_lock); }
static void kira_text_registry_release(void) { ReleaseSRWLockExclusive(&g_text_registry_lock); }
#else
#include <pthread.h>
static pthread_mutex_t g_text_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static void kira_text_registry_acquire(void) { pthread_mutex_lock(&g_text_registry_lock); }
static void kira_text_registry_release(void) { pthread_mutex_unlock(&g_text_registry_lock); }
#endif

/* Raw teardown of a face's FreeType/HarfBuzz resources (no registry unlink).
 * hb_font holds a reference on the FT_Face, so it must go first. */
static void kira_text_face_dispose(kira_text_face* face) {
    if (face == NULL) {
        return;
    }
    if (face->hb_font != NULL) {
        hb_font_destroy(face->hb_font);
    }
    if (face->face != NULL) {
        FT_Done_Face(face->face);
    }
    free(face->owned_blob);
    free(face);
}

/* Raw teardown of an engine's FreeType library (no registry unlink). All faces
 * belonging to this library must already be disposed. */
static void kira_text_engine_dispose(kira_text_engine* engine) {
    if (engine == NULL) {
        return;
    }
    if (engine->library != NULL) {
        FT_Done_FreeType(engine->library);
    }
    free(engine);
}

static void kira_text_registry_teardown(void) {
    kira_text_registry_acquire();
    KiraTextFaceNode* face_node = g_face_registry;
    g_face_registry = NULL;
    KiraTextEngineNode* engine_node = g_engine_registry;
    g_engine_registry = NULL;
    kira_text_registry_release();

    /* Faces first: FT_Done_Face needs its still-live FT_Library. */
    while (face_node != NULL) {
        KiraTextFaceNode* next = face_node->next;
        kira_text_face_dispose(face_node->face);
        free(face_node);
        face_node = next;
    }
    while (engine_node != NULL) {
        KiraTextEngineNode* next = engine_node->next;
        kira_text_engine_dispose(engine_node->engine);
        free(engine_node);
        engine_node = next;
    }
}

/* Register the atexit sweep exactly once. Caller must hold the registry lock. */
static void kira_text_registry_register_atexit(void) {
    static int registered = 0;
    if (!registered) {
        registered = 1;
        atexit(kira_text_registry_teardown);
    }
}

static void kira_text_engine_registry_add(kira_text_engine* engine) {
    KiraTextEngineNode* node = (KiraTextEngineNode*)malloc(sizeof(*node));
    if (node == NULL) {
        return; /* untracked: survives to exit unreclaimed, never unsafe */
    }
    node->engine = engine;
    kira_text_registry_acquire();
    kira_text_registry_register_atexit();
    node->next = g_engine_registry;
    g_engine_registry = node;
    kira_text_registry_release();
}

static void kira_text_engine_registry_remove(kira_text_engine* engine) {
    kira_text_registry_acquire();
    KiraTextEngineNode** link = &g_engine_registry;
    while (*link != NULL) {
        if ((*link)->engine == engine) {
            KiraTextEngineNode* dead = *link;
            *link = dead->next;
            free(dead);
            break;
        }
        link = &(*link)->next;
    }
    kira_text_registry_release();
}

static void kira_text_face_registry_add(kira_text_face* face) {
    KiraTextFaceNode* node = (KiraTextFaceNode*)malloc(sizeof(*node));
    if (node == NULL) {
        return; /* untracked: survives to exit unreclaimed, never unsafe */
    }
    node->face = face;
    kira_text_registry_acquire();
    kira_text_registry_register_atexit();
    node->next = g_face_registry;
    g_face_registry = node;
    kira_text_registry_release();
}

static void kira_text_face_registry_remove(kira_text_face* face) {
    kira_text_registry_acquire();
    KiraTextFaceNode** link = &g_face_registry;
    while (*link != NULL) {
        if ((*link)->face == face) {
            KiraTextFaceNode* dead = *link;
            *link = dead->next;
            free(dead);
            break;
        }
        link = &(*link)->next;
    }
    kira_text_registry_release();
}

kira_text_engine* kira_text_engine_create(void) {
    kira_text_engine* engine = (kira_text_engine*)calloc(1, sizeof(*engine));
    if (engine == NULL) {
        return NULL;
    }
    FT_Error err = FT_Init_FreeType(&engine->library);
    if (err != 0) {
        free(engine);
        return NULL;
    }
    kira_text_engine_registry_add(engine);
    return engine;
}

void kira_text_engine_destroy(kira_text_engine* engine) {
    if (engine == NULL) {
        return;
    }
    /* Unlink before disposing so the atexit sweep never double-frees. */
    kira_text_engine_registry_remove(engine);
    kira_text_engine_dispose(engine);
}

const char* kira_text_backend_version(void) {
    /* Static buffer is fine: version is constant for the process lifetime. */
    static char buffer[32];
    if (buffer[0] == '\0') {
        snprintf(buffer, sizeof(buffer), "FreeType %d.%d.%d",
                 FREETYPE_MAJOR, FREETYPE_MINOR, FREETYPE_PATCH);
    }
    return buffer;
}

static kira_text_face* kira_text_face_wrap(FT_Face face, unsigned char* owned_blob) {
    kira_text_face* wrapper = (kira_text_face*)calloc(1, sizeof(*wrapper));
    if (wrapper == NULL) {
        FT_Done_Face(face);
        free(owned_blob);
        return NULL;
    }
    wrapper->face = face;
    wrapper->owned_blob = owned_blob;
    wrapper->pixel_size = 0.0f;
    wrapper->has_kerning = FT_HAS_KERNING(face) ? 1 : 0;
    kira_text_face_registry_add(wrapper);
    return wrapper;
}

kira_text_face* kira_text_face_load(kira_text_engine* engine,
                                    const char* path,
                                    int face_index) {
    if (engine == NULL || engine->library == NULL || path == NULL) {
        return NULL;
    }
    FT_Face face = NULL;
    if (FT_New_Face(engine->library, path, (FT_Long)face_index, &face) != 0) {
        return NULL;
    }
    return kira_text_face_wrap(face, NULL);
}

kira_text_face* kira_text_face_load_memory(kira_text_engine* engine,
                                           const uint8_t* data,
                                           long length,
                                           int face_index) {
    if (engine == NULL || engine->library == NULL || data == NULL || length <= 0) {
        return NULL;
    }
    /* FreeType keeps a pointer to the memory for the face lifetime, so we own a
     * private copy and free it when the face is destroyed. */
    unsigned char* blob = (unsigned char*)malloc((size_t)length);
    if (blob == NULL) {
        return NULL;
    }
    memcpy(blob, data, (size_t)length);

    FT_Face face = NULL;
    if (FT_New_Memory_Face(engine->library, blob, (FT_Long)length,
                           (FT_Long)face_index, &face) != 0) {
        free(blob);
        return NULL;
    }
    return kira_text_face_wrap(face, blob);
}

kira_text_face* kira_text_face_load_bundled(kira_text_engine* engine) {
    return kira_text_face_load_memory(engine,
                                      kira_inter_font_data,
                                      (long)KIRA_INTER_FONT_SIZE,
                                      0);
}

/* Drive a variable font's optical size from the size being rendered.
 *
 * This is what `opsz` is FOR, and it is the whole reason Inter ships a Display
 * cut: a face drawn at 11 points wants open counters and sturdy stems, and the
 * same outlines scaled up to 28 look coarse and loose. Leaving the axis at its
 * default renders every size with the 14-point design, which is exactly the
 * flat, slightly-wrong look that separates a bundled webfont from a system one.
 *
 * The axis is found by TAG rather than by position: a font is free to order its
 * axes however it likes, and Inter happens to put `opsz` first while most put
 * `wght` there. Anything without the axis is left alone. */
static void kira_text_face_apply_variations(kira_text_face* face, float pixel_size, float weight) {
    if (face == NULL || face->face == NULL) {
        return;
    }
    if ((face->face->face_flags & FT_FACE_FLAG_MULTIPLE_MASTERS) == 0) {
        return;
    }

    FT_MM_Var* mm = NULL;
    if (FT_Get_MM_Var(face->face, &mm) != 0 || mm == NULL) {
        return;
    }

    FT_Fixed coords[16];
    FT_UInt count = mm->num_axis;
    if (count > 16) {
        count = 16;
    }
    if (FT_Get_Var_Design_Coordinates(face->face, count, coords) != 0) {
        FT_Done_MM_Var(face->face->glyph->library, mm);
        return;
    }

    int changed = 0;
    for (FT_UInt axis = 0; axis < count; axis += 1) {
        FT_ULong tag = mm->axis[axis].tag;
        FT_Fixed wanted;
        if (tag == FT_MAKE_TAG('o', 'p', 's', 'z')) {
            /* The axis is stated in points, and this is asked in pixels — the
             * same number at 1x and the honest reading of "the size it is set
             * at". */
            wanted = (FT_Fixed)(pixel_size * 65536.0f);
        } else if (tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
            wanted = (FT_Fixed)(weight * 65536.0f);
        } else {
            continue;
        }
        /* Both axes are set in ONE pass on purpose. An optical size shifts stem
         * contrast, so a face asked for a heavier weight and a larger optical
         * size in two separate calls spends the interval drawn at a combination
         * neither caller asked for. */
        if (wanted < mm->axis[axis].minimum) {
            wanted = mm->axis[axis].minimum;
        }
        if (wanted > mm->axis[axis].maximum) {
            wanted = mm->axis[axis].maximum;
        }
        if (coords[axis] != wanted) {
            coords[axis] = wanted;
            changed = 1;
        }
    }

    if (changed) {
        FT_Set_Var_Design_Coordinates(face->face, count, coords);
    }
    FT_Done_MM_Var(face->face->glyph->library, mm);
}

void kira_text_face_destroy(kira_text_face* face) {
    if (face == NULL) {
        return;
    }
    /* Unlink before disposing so the atexit sweep never double-frees. */
    kira_text_face_registry_remove(face);
    kira_text_face_dispose(face);
}

int kira_text_face_set_pixel_size(kira_text_face* face, float pixel_size, float weight) {
    if (face == NULL || face->face == NULL || pixel_size <= 0.0f) {
        return 0;
    }
    /* Use the fractional-aware setter so sub-pixel UI sizes round consistently
     * with how the platform requests them. */
    FT_F26Dot6 size_26dot6 = (FT_F26Dot6)(pixel_size * 64.0f + 0.5f);
    if (FT_Set_Char_Size(face->face, 0, size_26dot6, 0, 0) != 0) {
        return 0;
    }
    /* Size and weight are one setting, not two. A face carries both as design
     * axes, and every caller that knows one knows the other — so they arrive
     * together and there is no window in which a face is sized but not yet
     * weighted. */
    kira_text_face_apply_variations(face, pixel_size, weight);
    face->pixel_size = pixel_size;
    face->weight = weight;
    return 1;
}

void kira_text_face_vmetrics(const kira_text_face* face, kira_text_vmetrics* out) {
    if (out == NULL) {
        return;
    }
    out->ascender = 0.0f;
    out->descender = 0.0f;
    out->line_height = 0.0f;
    if (face == NULL || face->face == NULL) {
        return;
    }
    FT_Size_Metrics metrics = face->face->size->metrics;
    out->ascender = kira_text_f26dot6(metrics.ascender);
    out->descender = -kira_text_f26dot6(metrics.descender); /* descender is negative in FT */
    out->line_height = kira_text_f26dot6(metrics.height);
}

uint32_t kira_text_face_glyph_index(const kira_text_face* face,
                                        uint32_t codepoint) {
    if (face == NULL || face->face == NULL) {
        return 0;
    }
    return (unsigned int)FT_Get_Char_Index(face->face, (FT_ULong)codepoint);
}

float kira_text_measure_utf8(kira_text_face* face, const char* utf8, int byte_len) {
    if (face == NULL || face->face == NULL || utf8 == NULL) {
        return 0.0f;
    }
    if (byte_len < 0) {
        byte_len = (int)strlen(utf8);
    }
    if (byte_len == 0) {
        return 0.0f;
    }

    hb_font_t* hb = kira_text_hb_font(face);
    if (hb == NULL) {
        return 0.0f;
    }

    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, utf8, byte_len, 0, byte_len);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(hb, buffer, NULL, 0);

    unsigned int count = hb_buffer_get_length(buffer);
    hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, NULL);
    double width = 0.0;
    for (unsigned int i = 0; i < count; i += 1) {
        width += (double)positions[i].x_advance / 64.0;
    }
    hb_buffer_destroy(buffer);
    return (float)width;
}

/* Rasterize a glyph the way macOS does, rather than the way FreeType defaults to.
 *
 * Two departures from FT_LOAD_DEFAULT, and they go together:
 *
 *   FT_LOAD_NO_HINTING — hinting distorts an outline to line its stems up with the
 *   pixel grid. It buys crispness and pays for it in shape and spacing: every glyph
 *   is nudged somewhere slightly different from where the designer drew it. Apple
 *   has ignored hints since the beginning; the letterforms are rendered as drawn.
 *
 *   x_offset_26_6 — the price of unhinted rendering is that a glyph's position
 *   matters at finer than whole-pixel resolution, so the caller quantizes the pen to
 *   a fraction of a pixel and asks for the outline shifted by that much before it is
 *   scan-converted. This is subpixel POSITIONING (not subpixel antialiasing, which
 *   Apple dropped in Mojave): the coverage is computed for where the glyph actually
 *   sits instead of being computed once and resampled, which is what makes a run of
 *   text evenly spaced rather than visibly jittering between letters.
 *
 * The offset is in 26.6 fixed point, so 64 is one pixel and 16 is a quarter of one.
 */
int kira_text_face_render_glyph_offset(kira_text_face* face,
                                       uint32_t glyph_index,
                                       int x_offset_26_6,
                                       kira_text_glyph_bitmap* out) {
    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (face == NULL || face->face == NULL) {
        return 0;
    }
    FT_Face ft = face->face;
    if (FT_Load_Glyph(ft, (FT_UInt)glyph_index, FT_LOAD_NO_HINTING) != 0) {
        return 0;
    }
    if (ft->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        if (x_offset_26_6 != 0) {
            FT_Outline_Translate(&ft->glyph->outline, (FT_Pos)x_offset_26_6, 0);
        }
        /* Stem darkening.
         *
         * An unhinted outline puts a stem wherever the designer drew it, which at
         * a normal UI size is often most of a pixel wide and none of it aligned.
         * Scan-converted honestly that yields two half-lit pixels where the design
         * has one solid stroke, and the text reads lighter and rougher than it was
         * drawn — the price hinting used to pay for by moving the stem onto the
         * grid and distorting the letter.
         *
         * Apple pays it a third way: leave the outline where it is and fatten it
         * slightly, so a thin stem covers enough of its pixels to survive. This is
         * what made pre-Retina macOS text look soft and full rather than spindly,
         * and it is why their glyphs still look heavier than a default FreeType
         * render of the same font at the same size.
         *
         * Half a RASTER pixel, in 26.6 fixed point. The caller rasterizes text at
         * twice the device resolution, so this is a quarter of a device pixel where
         * it lands — enough to carry a hairline stem, short of the blurring that
         * comes from fattening a shape that was already thick enough. */
        FT_Outline_Embolden(&ft->glyph->outline, (FT_Pos)0);
    }
    if (FT_Render_Glyph(ft->glyph, FT_RENDER_MODE_NORMAL) != 0) {
        return 0;
    }
    FT_GlyphSlot slot = ft->glyph;
    out->width = (int)slot->bitmap.width;
    out->rows = (int)slot->bitmap.rows;
    out->bearing_x = slot->bitmap_left;
    out->bearing_y = slot->bitmap_top;
    out->advance = kira_text_f26dot6(slot->advance.x);
    out->pitch = slot->bitmap.pitch;
    out->buffer = slot->bitmap.buffer;
    return 1;
}

int kira_text_face_render_glyph(kira_text_face* face,
                                uint32_t glyph_index,
                                kira_text_glyph_bitmap* out) {
    return kira_text_face_render_glyph_offset(face, glyph_index, 0, out);
}

static const char* kira_text_probe_font(void) {
#if defined(__APPLE__)
    /* On Apple platforms the system font wins, ahead of anything bundled.
     *
     * SF Pro is not merely a nicer default here — it is the one this text stack is
     * judged against. Every other window on the screen is drawn in it, so a UI in
     * any other face reads as foreign at a glance, and the difference is sharpest
     * at exactly the sizes a UI uses: SF carries optical sizing, so its small text
     * is drawn with more open counters and sturdier stems than a display face
     * scaled down to the same point size. That is most of what remains between this
     * renderer's output and an AppKit app's once the rasterization matches.
     *
     * The file is the OS's own, read in place and never copied or shipped, which is
     * what the system font is for.
     *
     * SFNS.ttf is a variable font. FreeType instantiates its default master, which
     * is Regular — weights above that still come from the requested size alone
     * until the weight axis is driven (see kira_text_face_set_pixel_size). */
    static const char* const apple_candidates[] = {
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/SFNSDisplay.ttf",
        "/System/Library/Fonts/SFNSText.ttf",
    };
    for (size_t i = 0; i < sizeof(apple_candidates) / sizeof(apple_candidates[0]); i += 1) {
        FILE* probe = fopen(apple_candidates[i], "rb");
        if (probe != NULL) {
            fclose(probe);
            return apple_candidates[i];
        }
    }
#endif

    /* Prefer the bundled Inter file shipped alongside the library. */
    static const char* const bundled_candidates[] = {
        "fonts/InterVariable.ttf",
        "../fonts/InterVariable.ttf",
    };
    for (size_t i = 0; i < sizeof(bundled_candidates) / sizeof(bundled_candidates[0]); i += 1) {
        FILE* probe = fopen(bundled_candidates[i], "rb");
        if (probe != NULL) {
            fclose(probe);
            return bundled_candidates[i];
        }
    }

    /* Inter may be installed as a system font. */
    static const char* const inter_candidates[] = {
        "C:/Windows/Fonts/InterVariable.ttf",
        "/System/Library/Fonts/InterVariable.ttf",
        "/usr/share/fonts/truetype/inter/InterVariable.ttf",
        "/usr/share/fonts/opentype/inter/InterVariable.ttf",
        "~/.fonts/InterVariable.ttf",
    };
    for (size_t i = 0; i < sizeof(inter_candidates) / sizeof(inter_candidates[0]); i += 1) {
        FILE* probe = fopen(inter_candidates[i], "rb");
        if (probe != NULL) {
            fclose(probe);
            return inter_candidates[i];
        }
    }

#if defined(_WIN32)
    /* Windows stops here, at the embedded Inter, rather than falling through
     * to Segoe UI.
     *
     * This has to agree with `windowsFontCandidates()` on the Kira side, which
     * names no system face for the same reason: measurement and rasterization
     * read the face through two different entry points, and a build where one
     * of them resolves Segoe while the other resolves Inter lays text out to
     * one font's advances and then draws it in another's. */
    return "<builtin>";
#else
    /* Common system fonts across the host platforms this engine targets. */
    static const char* const sys_candidates[] = {
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    for (size_t i = 0; i < sizeof(sys_candidates) / sizeof(sys_candidates[0]); i += 1) {
        FILE* probe = fopen(sys_candidates[i], "rb");
        if (probe != NULL) {
            fclose(probe);
            return sys_candidates[i];
        }
    }
#endif

    /* No system font is available — use the embedded Inter variable font.
     * The sentinel value "<builtin>" is handled by kira_text_cached_face(). */
    return "<builtin>";
}

/* The default face path, probed once.
 *
 * Which fonts are installed does not change while a process runs, but the probe
 * that answers it opens files: up to thirteen of them, and the first hit is
 * usually the last candidate on the list. Every measured run without an
 * explicit font path asks this question, so a UI frame asked it hundreds of
 * times and spent more of itself in open(2) than in anything else — the largest
 * single cost in a Project Matter frame once the compiler stopped copying
 * aggregates. Every result, including the "<builtin>" fallback, is a string
 * literal, so caching the pointer keeps it valid for the life of the process. */
static const char* kira_text_discover_font(void) {
    static const char* cached = NULL;
    if (cached == NULL) {
        cached = kira_text_probe_font();
    }
    return cached;
}

/* Locate a CJK-capable system font. The bundled Inter face (and Segoe/Arial)
 * have no CJK coverage, so committed Hanzi would render as .notdef tofu. These
 * fonts also carry Latin glyphs, so a mixed Latin+Hanzi run can be shaped
 * entirely with the CJK face. Returns NULL when none is installed. */
static const char* kira_text_probe_cjk_font(void) {
    static const char* const cjk_candidates[] = {
        "C:/Windows/Fonts/msyh.ttc",   /* Microsoft YaHei */
        "C:/Windows/Fonts/msyh.ttf",
        "C:/Windows/Fonts/simsun.ttc", /* SimSun */
        "C:/Windows/Fonts/msgothic.ttc",
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    };
    for (size_t i = 0; i < sizeof(cjk_candidates) / sizeof(cjk_candidates[0]); i += 1) {
        FILE* probe = fopen(cjk_candidates[i], "rb");
        if (probe != NULL) {
            fclose(probe);
            return cjk_candidates[i];
        }
    }
    return NULL;
}

/* The CJK face path, probed once, on the same terms as the default face.
 *
 * A separate flag, because "no CJK font is installed" is a real answer and a
 * null pointer would otherwise re-probe every run — the expensive case, since
 * a miss opens every candidate. */
static const char* kira_text_discover_cjk_font(void) {
    static const char* cached = NULL;
    static int probed = 0;
    if (!probed) {
        cached = kira_text_probe_cjk_font();
        probed = 1;
    }
    return cached;
}

/* Decode UTF-8 and report whether any scalar lands in a CJK/Kana/Hangul block
 * (>= U+3000), i.e. text that the default Latin face cannot render. */
static int kira_text_utf8_has_cjk(const char* utf8) {
    const unsigned char* p = (const unsigned char*)utf8;
    while (*p != '\0') {
        uint32_t cp;
        int len;
        if (p[0] < 0x80) {
            cp = p[0];
            len = 1;
        } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
            len = 2;
        } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
            len = 3;
        } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
            len = 4;
        } else {
            /* Malformed byte — skip it. */
            p += 1;
            continue;
        }
        if (cp >= 0x3000) {
            return 1;
        }
        p += len;
    }
    return 0;
}

/* Choose the face path for a run: when the caller left the path unset, prefer a
 * CJK font for runs that contain CJK scalars, otherwise the default Latin face. */
static const char* kira_text_resolve_font(const char* font_path, const char* utf8) {
    if (font_path != NULL && font_path[0] != '\0') {
        return font_path;
    }
    if (utf8 != NULL && kira_text_utf8_has_cjk(utf8)) {
        const char* cjk = kira_text_discover_cjk_font();
        if (cjk != NULL) {
            return cjk;
        }
    }
    return kira_text_discover_font();
}

const char* kira_text_probe_report(const char* font_path) {
    static char report[512];

    if (font_path == NULL || font_path[0] == '\0') {
        font_path = kira_text_discover_font();
        if (font_path == NULL) {
            snprintf(report, sizeof(report),
                     "kira-text: ERROR no system font found to probe");
            return report;
        }
    }

    kira_text_engine* engine = kira_text_engine_create();
    if (engine == NULL) {
        snprintf(report, sizeof(report),
                 "kira-text: ERROR FreeType failed to initialize");
        return report;
    }

    const float px = 32.0f;

    kira_text_face* face = NULL;
    if (strcmp(font_path, "<builtin>") == 0) {
        face = kira_text_embedded_face(px, 400.0f);
        if (face == NULL) {
            snprintf(report, sizeof(report),
                     "kira-text: ERROR could not load embedded Inter font");
            kira_text_engine_destroy(engine);
            return report;
        }
    } else {
        face = kira_text_face_load(engine, font_path, 0);
        if (face == NULL) {
            snprintf(report, sizeof(report),
                     "kira-text: ERROR could not load face '%s'", font_path);
            kira_text_engine_destroy(engine);
            return report;
        }
    }

    if (face->pixel_size != px && !kira_text_face_set_pixel_size(face, px, 400.0f)) {
        snprintf(report, sizeof(report),
                 "kira-text: ERROR could not size face '%s'", font_path);
        kira_text_face_destroy(face);
        kira_text_engine_destroy(engine);
        return report;
    }

    uint32_t glyph_h = kira_text_face_glyph_index(face, (uint32_t)'H');
    kira_text_glyph_bitmap bitmap;
    int rendered = kira_text_face_render_glyph(face, glyph_h, &bitmap);
    float hello = kira_text_measure_utf8(face, "Hello", -1);
    kira_text_vmetrics vmetrics;
    kira_text_face_vmetrics(face, &vmetrics);

    if (glyph_h == 0 || !rendered) {
        snprintf(report, sizeof(report),
                 "kira-text: ERROR glyph 'H' missing/unrenderable in '%s'",
                 font_path);
    } else {
        snprintf(report, sizeof(report),
                 "%s | font=%s | px=%.0f | line=%.1f asc=%.1f desc=%.1f | "
                 "glyph(H)=%u dims=%dx%d adv=%.1f bearing=(%d,%d) | "
                 "measure(\"Hello\")=%.1fpx",
                 kira_text_backend_version(), font_path, px,
                 vmetrics.line_height, vmetrics.ascender, vmetrics.descender,
                 glyph_h, bitmap.width, bitmap.rows, bitmap.advance,
                 bitmap.bearing_x, bitmap.bearing_y, hello);
    }

    if (strcmp(font_path, "<builtin>") != 0) {
        kira_text_face_destroy(face);
    }
    kira_text_engine_destroy(engine);
    return report;
}

/* Embedded Inter font face — lazily loaded from the bundled byte array. */
static kira_text_engine* g_embedded_engine = NULL;
static kira_text_face*   g_embedded_face   = NULL;
static float             g_embedded_ps     = 0.0f;
static float             g_embedded_weight = 0.0f;

static kira_text_face* kira_text_embedded_face(float pixel_size, float weight) {
    if (g_embedded_face != NULL && g_embedded_ps == pixel_size && g_embedded_weight == weight) {
        return g_embedded_face;
    }
    if (g_embedded_face != NULL) {
        kira_text_face_destroy(g_embedded_face);
        g_embedded_face = NULL;
        g_embedded_ps = 0.0f;
        g_embedded_weight = 0.0f;
    }
    if (g_embedded_engine == NULL) {
        g_embedded_engine = kira_text_engine_create();
        if (g_embedded_engine == NULL) return NULL;
    }
    g_embedded_face = kira_text_face_load_memory(
        g_embedded_engine,
        kira_inter_font_data,
        (long)KIRA_INTER_FONT_SIZE,
        0
    );
    if (g_embedded_face == NULL) return NULL;
    if (!kira_text_face_set_pixel_size(g_embedded_face, pixel_size, weight)) {
        kira_text_face_destroy(g_embedded_face);
        g_embedded_face = NULL;
        return NULL;
    }
    g_embedded_ps = pixel_size;
    g_embedded_weight = weight;
    return g_embedded_face;
}

/* Face cache: FT_New_Face parses the whole font, far too slow to repeat per
 * draw call. Cache a handful of faces keyed by (path, quantized pixel size,
 * weight).
 *
 * Weight is part of the KEY, not a property set afterwards. A face carries its
 * variation coordinates, so two runs at the same size and different weights are
 * two different faces — keying on size alone hands the second run the first
 * one's weight, and which weight a label ends up drawn in then depends on the
 * order the panel happened to be built in. */
#define KIRA_TEXT_DRAW_CACHE_SLOTS 16

typedef struct {
    char            path[260];
    int             pixel_size_q; /* pixel_size rounded to 0.5px units */
    int             weight_q;     /* weight rounded to whole units */
    kira_text_face* face;
} kira_text_draw_cache_slot;

static kira_text_engine* g_draw_engine = NULL;
static kira_text_draw_cache_slot g_draw_cache[KIRA_TEXT_DRAW_CACHE_SLOTS];
static int g_draw_cache_count = 0;

static kira_text_face* kira_text_cached_face(const char* path, float pixel_size, float weight) {
    /* The embedded Inter font is loaded from the bundled byte array. */
    if (strcmp(path, "<builtin>") == 0) {
        return kira_text_embedded_face(pixel_size, weight);
    }

    if (g_draw_engine == NULL) {
        g_draw_engine = kira_text_engine_create();
        if (g_draw_engine == NULL) {
            return NULL;
        }
    }
    int quantized = (int)(pixel_size * 2.0f + 0.5f);
    int weight_q = (int)(weight + 0.5f);

    for (int i = 0; i < g_draw_cache_count; i += 1) {
        if (g_draw_cache[i].pixel_size_q == quantized &&
            g_draw_cache[i].weight_q == weight_q &&
            strcmp(g_draw_cache[i].path, path) == 0) {
            return g_draw_cache[i].face;
        }
    }

    kira_text_face* face = kira_text_face_load(g_draw_engine, path, 0);
    if (face == NULL) {
        return NULL;
    }
    kira_text_face_set_pixel_size(face, pixel_size, weight);

    kira_text_draw_cache_slot* slot;
    if (g_draw_cache_count < KIRA_TEXT_DRAW_CACHE_SLOTS) {
        slot = &g_draw_cache[g_draw_cache_count++];
    } else {
        /* Evict the oldest slot. */
        kira_text_face_destroy(g_draw_cache[0].face);
        for (int i = 1; i < KIRA_TEXT_DRAW_CACHE_SLOTS; i += 1) {
            g_draw_cache[i - 1] = g_draw_cache[i];
        }
        slot = &g_draw_cache[KIRA_TEXT_DRAW_CACHE_SLOTS - 1];
    }
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->pixel_size_q = quantized;
    slot->weight_q = weight_q;
    slot->face = face;
    return face;
}

double kira_text_measure_run(const char* font_path, const char* utf8, double pixel_size, double weight) {
    if (utf8 == NULL || utf8[0] == '\0' || pixel_size <= 0.0) {
        return 0.0;
    }
    font_path = kira_text_resolve_font(font_path, utf8);
    if (font_path == NULL) {
        return 0.0;
    }
    kira_text_face* face = kira_text_cached_face(font_path, (float)pixel_size, (float)weight);
    if (face == NULL) {
        return 0.0;
    }
    return (double)kira_text_measure_utf8(face, utf8, -1);
}

double kira_text_line_height(const char* font_path, double pixel_size, double weight) {
    if (pixel_size <= 0.0) {
        return 0.0;
    }
    if (font_path == NULL || font_path[0] == '\0') {
        font_path = kira_text_discover_font();
        if (font_path == NULL) {
            return 0.0;
        }
    }
    kira_text_face* face = kira_text_cached_face(font_path, (float)pixel_size, (float)weight);
    if (face == NULL) {
        return 0.0;
    }
    kira_text_vmetrics vmetrics;
    kira_text_face_vmetrics(face, &vmetrics);
    return (double)vmetrics.line_height;
}

double kira_text_ascent(const char* font_path, double pixel_size, double weight) {
    if (pixel_size <= 0.0) {
        return 0.0;
    }
    if (font_path == NULL || font_path[0] == '\0') {
        font_path = kira_text_discover_font();
        if (font_path == NULL) {
            return 0.0;
        }
    }
    kira_text_face* face = kira_text_cached_face(font_path, (float)pixel_size, (float)weight);
    if (face == NULL) {
        return 0.0;
    }
    kira_text_vmetrics vmetrics;
    kira_text_face_vmetrics(face, &vmetrics);
    return (double)vmetrics.ascender;
}

double kira_text_descent(const char* font_path, double pixel_size, double weight) {
    if (pixel_size <= 0.0) {
        return 0.0;
    }
    if (font_path == NULL || font_path[0] == '\0') {
        font_path = kira_text_discover_font();
        if (font_path == NULL) {
            return 0.0;
        }
    }
    kira_text_face* face = kira_text_cached_face(font_path, (float)pixel_size, (float)weight);
    if (face == NULL) {
        return 0.0;
    }
    kira_text_vmetrics vmetrics;
    kira_text_face_vmetrics(face, &vmetrics);
    return (double)vmetrics.descender;
}

/* Visible run bounds, cached separately from the face/advance cache. The UI
 * asks for these during lowering, so the first request may shape and rasterize
 * the string, but steady frames only do two small key lookups. Horizontal values
 * are coordinates from the run origin; vertical values are positive distances
 * from the baseline: top is above it, bottom is below it. */
#define KIRA_TEXT_INK_CACHE_SLOTS 128
#define KIRA_TEXT_INK_CACHE_PATH_MAX 260
#define KIRA_TEXT_INK_CACHE_TEXT_MAX 256

typedef struct {
    int valid;
    int pixel_size_q;
    int weight_q;
    char path[KIRA_TEXT_INK_CACHE_PATH_MAX];
    char text[KIRA_TEXT_INK_CACHE_TEXT_MAX];
    float left;
    float right;
    float top;
    float bottom;
} kira_text_ink_cache_slot;

static kira_text_ink_cache_slot g_ink_cache[KIRA_TEXT_INK_CACHE_SLOTS];
static int g_ink_cache_next = 0;

static int kira_text_run_ink_bounds(
    kira_text_face* face,
    const char* utf8,
    float* out_left,
    float* out_right,
    float* out_top,
    float* out_bottom
) {
    if (out_left != NULL) {
        *out_left = 0.0f;
    }
    if (out_right != NULL) {
        *out_right = 0.0f;
    }
    if (out_top != NULL) {
        *out_top = 0.0f;
    }
    if (out_bottom != NULL) {
        *out_bottom = 0.0f;
    }
    if (face == NULL || face->face == NULL || utf8 == NULL || utf8[0] == '\0') {
        return 0;
    }

    hb_font_t* hb = kira_text_hb_font(face);
    if (hb == NULL) {
        return 0;
    }
    hb_buffer_t* buffer = hb_buffer_create();
    if (buffer == NULL) {
        return 0;
    }
    hb_buffer_add_utf8(buffer, utf8, -1, 0, -1);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(hb, buffer, NULL, 0);

    unsigned int count = hb_buffer_get_length(buffer);
    hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, NULL);
    hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, NULL);
    float pen_x = 0.0f;
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
    int found = 0;
    for (unsigned int i = 0; i < count; i += 1) {
        float glyph_x = pen_x + kira_text_f26dot6(positions[i].x_offset);
        pen_x += kira_text_f26dot6(positions[i].x_advance);
        if (FT_Load_Glyph(face->face, infos[i].codepoint, FT_LOAD_NO_HINTING) != 0) {
            continue;
        }
        if (face->face->glyph->format == FT_GLYPH_FORMAT_OUTLINE &&
            FT_Render_Glyph(face->face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
            continue;
        }
        FT_GlyphSlot slot = face->face->glyph;
        if (slot->bitmap.rows <= 0 || slot->bitmap.width <= 0) {
            continue;
        }
        float glyph_left = glyph_x + (float)slot->bitmap_left;
        float glyph_right = glyph_left + (float)slot->bitmap.width;
        float y_offset = kira_text_f26dot6(positions[i].y_offset);
        float glyph_top = y_offset + (float)slot->bitmap_top;
        float glyph_bottom = glyph_top - (float)slot->bitmap.rows;
        if (!found || glyph_left < left) {
            left = glyph_left;
        }
        if (!found || glyph_right > right) {
            right = glyph_right;
        }
        if (!found || glyph_top > top) {
            top = glyph_top;
        }
        if (!found || glyph_bottom < bottom) {
            bottom = glyph_bottom;
        }
        found = 1;
    }
    hb_buffer_destroy(buffer);
    if (!found) {
        return 0;
    }
    if (out_left != NULL) {
        *out_left = left;
    }
    if (out_right != NULL) {
        *out_right = right;
    }
    if (out_top != NULL) {
        *out_top = top;
    }
    if (out_bottom != NULL) {
        *out_bottom = 0.0f - bottom;
    }
    return 1;
}

static int kira_text_cached_ink_bounds(
    const char* font_path,
    const char* utf8,
    double pixel_size,
    double weight,
    double* out_left,
    double* out_right,
    double* out_top,
    double* out_bottom
) {
    if (out_left != NULL) {
        *out_left = 0.0;
    }
    if (out_right != NULL) {
        *out_right = 0.0;
    }
    if (out_top != NULL) {
        *out_top = 0.0;
    }
    if (out_bottom != NULL) {
        *out_bottom = 0.0;
    }
    if (utf8 == NULL || utf8[0] == '\0' || pixel_size <= 0.0) {
        return 0;
    }
    font_path = kira_text_resolve_font(font_path, utf8);
    if (font_path == NULL) {
        return 0;
    }

    size_t path_len = strlen(font_path);
    size_t text_len = strlen(utf8);
    const double raster_measure_scale = 4.0;
    double raster_size = pixel_size * raster_measure_scale;
    int pixel_size_q = (int)(raster_size * 100.0 + 0.5);
    /* Ink bounds are a property of the WEIGHT as much as the size: a semibold
     * run is wider and its stems reach further than the regular cut of the same
     * string. Leaving weight out of the key is how a bold label gets laid out to
     * the regular run's ink and then painted past it. */
    int weight_q = (int)(weight + 0.5);

    if (path_len < KIRA_TEXT_INK_CACHE_PATH_MAX &&
        text_len < KIRA_TEXT_INK_CACHE_TEXT_MAX) {
        for (int i = 0; i < KIRA_TEXT_INK_CACHE_SLOTS; i += 1) {
            kira_text_ink_cache_slot* slot = &g_ink_cache[i];
            if (slot->valid && slot->pixel_size_q == pixel_size_q &&
                slot->weight_q == weight_q &&
                strcmp(slot->path, font_path) == 0 &&
                strcmp(slot->text, utf8) == 0) {
                if (out_left != NULL) {
                    *out_left = (double)slot->left / raster_measure_scale;
                }
                if (out_right != NULL) {
                    *out_right = (double)slot->right / raster_measure_scale;
                }
                if (out_top != NULL) {
                    *out_top = (double)slot->top / raster_measure_scale;
                }
                if (out_bottom != NULL) {
                    *out_bottom = (double)slot->bottom / raster_measure_scale;
                }
                return 1;
            }
        }
    }

    kira_text_face* face = kira_text_cached_face(font_path, (float)raster_size, (float)weight);
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
    if (!kira_text_run_ink_bounds(face, utf8, &left, &right, &top, &bottom)) {
        return 0;
    }

    if (path_len < KIRA_TEXT_INK_CACHE_PATH_MAX &&
        text_len < KIRA_TEXT_INK_CACHE_TEXT_MAX) {
        kira_text_ink_cache_slot* slot = &g_ink_cache[g_ink_cache_next];
        g_ink_cache_next = (g_ink_cache_next + 1) % KIRA_TEXT_INK_CACHE_SLOTS;
        slot->valid = 1;
        slot->pixel_size_q = pixel_size_q;
        slot->weight_q = weight_q;
        snprintf(slot->path, sizeof(slot->path), "%s", font_path);
        snprintf(slot->text, sizeof(slot->text), "%s", utf8);
        slot->left = left;
        slot->right = right;
        slot->top = top;
        slot->bottom = bottom;
    }
    if (out_left != NULL) {
        *out_left = (double)left / raster_measure_scale;
    }
    if (out_right != NULL) {
        *out_right = (double)right / raster_measure_scale;
    }
    if (out_top != NULL) {
        *out_top = (double)top / raster_measure_scale;
    }
    if (out_bottom != NULL) {
        *out_bottom = (double)bottom / raster_measure_scale;
    }
    return 1;
}

double kira_text_run_ink_left(const char* font_path, const char* utf8, double pixel_size, double weight) {
    double left = 0.0;
    kira_text_cached_ink_bounds(font_path, utf8, pixel_size, weight, &left, NULL, NULL, NULL);
    return left;
}

double kira_text_run_ink_right(const char* font_path, const char* utf8, double pixel_size, double weight) {
    double right = 0.0;
    kira_text_cached_ink_bounds(font_path, utf8, pixel_size, weight, NULL, &right, NULL, NULL);
    return right;
}

double kira_text_run_ink_top(const char* font_path, const char* utf8, double pixel_size, double weight) {
    double top = 0.0;
    kira_text_cached_ink_bounds(font_path, utf8, pixel_size, weight, NULL, NULL, &top, NULL);
    return top;
}

double kira_text_run_ink_bottom(const char* font_path, const char* utf8, double pixel_size, double weight) {
    double bottom = 0.0;
    kira_text_cached_ink_bounds(font_path, utf8, pixel_size, weight, NULL, NULL, NULL, &bottom);
    return bottom;
}

int kira_text_utf8_next(const char* s, int len, int32_t* index, uint32_t* codepoint) {
    if (s == NULL || index == NULL || codepoint == NULL) {
        return 0;
    }
    int i = *index;
    if (i < 0 || i >= len) {
        return 0;
    }

    unsigned char first = (unsigned char)s[i];
    unsigned int cp;
    int extra;

    if (first < 0x80u) {
        cp = first;
        extra = 0;
    } else if ((first & 0xE0u) == 0xC0u) {
        cp = first & 0x1Fu;
        extra = 1;
    } else if ((first & 0xF0u) == 0xE0u) {
        cp = first & 0x0Fu;
        extra = 2;
    } else if ((first & 0xF8u) == 0xF0u) {
        cp = first & 0x07u;
        extra = 3;
    } else {
        /* Invalid leading byte. */
        *index = i + 1;
        *codepoint = 0xFFFDu;
        return 1;
    }

    if (i + extra >= len) {
        /* Truncated sequence. */
        *index = len;
        *codepoint = 0xFFFDu;
        return 1;
    }

    for (int k = 1; k <= extra; k += 1) {
        unsigned char cont = (unsigned char)s[i + k];
        if ((cont & 0xC0u) != 0x80u) {
            /* Malformed continuation: emit replacement, resync at this byte. */
            *index = i + k;
            *codepoint = 0xFFFDu;
            return 1;
        }
        cp = (cp << 6) | (cont & 0x3Fu);
    }

    *index = i + extra + 1;
    *codepoint = cp;
    return 1;
}

/* Decode a whole NUL-terminated UTF-8 string into codepoints in one call, so the
 * Kira side gets an editable buffer from an opaque String without per-codepoint
 * FFI round-trips. Writes up to `max` int32 codepoints into `out`, returns the
 * count. */
int kira_text_decode_codepoints(const char* s, int32_t* out, int max) {
    if (s == NULL || out == NULL || max <= 0) {
        return 0;
    }
    int len = (int)strlen(s);
    int32_t index = 0;
    int n = 0;
    uint32_t cp;
    while (n < max) {
        if (!kira_text_utf8_next(s, len, &index, &cp)) {
            break;
        }
        out[n] = (int32_t)cp;
        n += 1;
    }
    return n;
}
