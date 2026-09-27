/* Kira UI icon engine — a compact SVG-subset rasterizer producing 8-bit
 * coverage bitmaps for the batched compositor's glyph atlas.
 *
 * Supported subset (chosen to cover hand-authored and lucide/feather-style
 * icons):
 *   elements   svg (viewBox), g (passthrough), path, rect, circle, ellipse,
 *              line, polyline, polygon
 *   path data  M m L l H h V v C c S s Q q T t A a Z z
 *   paint      fill / stroke = none | anything (color is IGNORED — coverage
 *              only, tinted at draw), fill-rule (nonzero | evenodd),
 *              stroke-width, per-element or inherited from <svg>
 *
 * Rendering model:
 *   - Fills: subpaths are flattened to polygons and filled by a scanline
 *     accumulator with 4 sub-rows per pixel row and analytic horizontal span
 *     coverage (winding or even-odd).
 *   - Strokes: flattened polylines rendered as a distance field (min distance
 *     to segments), which gives round caps and joins for free — matching the
 *     round-cap style icon sets use.
 *   - Both accumulate into a float coverage plane; max-combined, quantized to
 *     8-bit.
 *
 * Not supported (silently ignored): transforms, gradients, clip paths, text,
 * markers, CSS. Icons needing those should be pre-flattened.
 */

#include "kira_icon.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* `M_PI` is not ISO C. glibc and Apple's libc define it in <math.h> as an
 * extension, so this compiled everywhere it had been built; the UCRT defines it
 * only when <math.h> is included with `_USE_MATH_DEFINES` already set, so the
 * first Windows build failed on every use of it here. Naming it once, guarded,
 * is portable to both and to a libc that offers neither — and it cannot
 * conflict with a definition that is already there. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------- geometry */

typedef struct {
    float x, y;
} IconPt;

typedef struct {
    IconPt *pts;
    int32_t count;
    int32_t cap;
    int32_t closed;
} IconPath;

typedef struct {
    IconPath *paths;
    int32_t count;
    int32_t cap;
} IconPathSet;

static void icon_path_push(IconPath *p, float x, float y) {
    if (p->count == p->cap) {
        int32_t next = p->cap < 16 ? 16 : p->cap * 2;
        IconPt *grown = (IconPt *)realloc(p->pts, (size_t)next * sizeof(IconPt));
        if (grown == NULL) return;
        p->pts = grown;
        p->cap = next;
    }
    /* Collapse exact duplicates (zero-length segments confuse winding). */
    if (p->count > 0) {
        IconPt last = p->pts[p->count - 1];
        if (last.x == x && last.y == y) return;
    }
    p->pts[p->count].x = x;
    p->pts[p->count].y = y;
    p->count += 1;
}

static IconPath *icon_set_begin(IconPathSet *set) {
    if (set->count == set->cap) {
        int32_t next = set->cap < 8 ? 8 : set->cap * 2;
        IconPath *grown = (IconPath *)realloc(set->paths, (size_t)next * sizeof(IconPath));
        if (grown == NULL) return NULL;
        set->paths = grown;
        set->cap = next;
    }
    IconPath *p = &set->paths[set->count];
    memset(p, 0, sizeof(*p));
    set->count += 1;
    return p;
}

static void icon_set_free(IconPathSet *set) {
    for (int32_t i = 0; i < set->count; i++) free(set->paths[i].pts);
    free(set->paths);
    memset(set, 0, sizeof(*set));
}

/* ------------------------------------------------------------------- hash */

int64_t kira_icon_hash(const char *svg_utf8) {
    if (svg_utf8 == NULL) return 0;
    uint64_t hash = 1469598103934665603ULL; /* FNV offset basis */
    for (const unsigned char *p = (const unsigned char *)svg_utf8; *p != 0; p++) {
        hash ^= (uint64_t)(*p);
        hash *= 1099511628211ULL; /* FNV prime */
    }
    hash &= 0x7FFFFFFFFFFFFFFFULL;
    if (hash == 0) hash = 1;
    return (int64_t)hash;
}

/* ------------------------------------------------------------- attributes */

/* Find attribute `name` inside the tag text [tag, tag_end); returns pointer to
 * the value (inside quotes) and its length, or NULL. */
static const char *icon_attr(const char *tag, const char *tag_end, const char *name, int32_t *out_len) {
    size_t name_len = strlen(name);
    const char *p = tag;
    while (p + name_len < tag_end) {
        p = (const char *)memchr(p, name[0], (size_t)(tag_end - p));
        if (p == NULL || p + name_len >= tag_end) return NULL;
        /* attribute boundary: preceded by whitespace, followed by = */
        if ((p == tag || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r') &&
            strncmp(p, name, name_len) == 0) {
            const char *q = p + name_len;
            while (q < tag_end && (*q == ' ' || *q == '\t')) q++;
            if (q < tag_end && *q == '=') {
                q++;
                while (q < tag_end && (*q == ' ' || *q == '\t')) q++;
                if (q < tag_end && (*q == '"' || *q == '\'')) {
                    char quote = *q;
                    q++;
                    const char *end = (const char *)memchr(q, quote, (size_t)(tag_end - q));
                    if (end == NULL) return NULL;
                    *out_len = (int32_t)(end - q);
                    return q;
                }
            }
        }
        p += 1;
    }
    return NULL;
}

static float icon_attr_float(const char *tag, const char *tag_end, const char *name, float fallback) {
    int32_t len = 0;
    const char *v = icon_attr(tag, tag_end, name, &len);
    if (v == NULL || len <= 0) return fallback;
    char buf[64];
    int32_t n = len < 63 ? len : 63;
    memcpy(buf, v, (size_t)n);
    buf[n] = 0;
    return (float)atof(buf);
}

static int32_t icon_attr_is(const char *tag, const char *tag_end, const char *name, const char *value) {
    int32_t len = 0;
    const char *v = icon_attr(tag, tag_end, name, &len);
    if (v == NULL) return 0;
    return (int32_t)strlen(value) == len && strncmp(v, value, (size_t)len) == 0;
}

/* --------------------------------------------------------- number scanning */

static const char *icon_skip_sep(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char *icon_read_float(const char *p, const char *end, float *out) {
    p = icon_skip_sep(p, end);
    if (p >= end) return NULL;
    char *stop = NULL;
    float value = strtof(p, &stop);
    if (stop == p) return NULL;
    *out = value;
    return stop;
}

/* ----------------------------------------------------------- path flatten */

#define ICON_BEZ_STEPS 18

static void icon_flatten_cubic(IconPath *p, IconPt p0, IconPt c1, IconPt c2, IconPt p1) {
    for (int32_t i = 1; i <= ICON_BEZ_STEPS; i++) {
        float t = (float)i / (float)ICON_BEZ_STEPS;
        float u = 1.0f - t;
        float x = u * u * u * p0.x + 3.0f * u * u * t * c1.x + 3.0f * u * t * t * c2.x + t * t * t * p1.x;
        float y = u * u * u * p0.y + 3.0f * u * u * t * c1.y + 3.0f * u * t * t * c2.y + t * t * t * p1.y;
        icon_path_push(p, x, y);
    }
}

static void icon_flatten_quad(IconPath *p, IconPt p0, IconPt c, IconPt p1) {
    for (int32_t i = 1; i <= ICON_BEZ_STEPS; i++) {
        float t = (float)i / (float)ICON_BEZ_STEPS;
        float u = 1.0f - t;
        float x = u * u * p0.x + 2.0f * u * t * c.x + t * t * p1.x;
        float y = u * u * p0.y + 2.0f * u * t * c.y + t * t * p1.y;
        icon_path_push(p, x, y);
    }
}

/* SVG elliptical arc (endpoint parametrization) flattened by angle stepping. */
static void icon_flatten_arc(IconPath *p, IconPt p0, float rx, float ry, float rot_deg,
                             int32_t large_arc, int32_t sweep, IconPt p1) {
    if (rx == 0.0f || ry == 0.0f) {
        icon_path_push(p, p1.x, p1.y);
        return;
    }
    rx = fabsf(rx);
    ry = fabsf(ry);
    float phi = rot_deg * (float)M_PI / 180.0f;
    float cosp = cosf(phi), sinp = sinf(phi);
    float dx2 = (p0.x - p1.x) / 2.0f;
    float dy2 = (p0.y - p1.y) / 2.0f;
    float x1p = cosp * dx2 + sinp * dy2;
    float y1p = -sinp * dx2 + cosp * dy2;
    float lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0f) {
        float s = sqrtf(lambda);
        rx *= s;
        ry *= s;
    }
    float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    float radicand = den == 0.0f ? 0.0f : num / den;
    if (radicand < 0.0f) radicand = 0.0f;
    float coef = sqrtf(radicand);
    if (large_arc == sweep) coef = -coef;
    float cxp = coef * (rx * y1p / ry);
    float cyp = coef * (-ry * x1p / rx);
    float cx = cosp * cxp - sinp * cyp + (p0.x + p1.x) / 2.0f;
    float cy = sinp * cxp + cosp * cyp + (p0.y + p1.y) / 2.0f;

    float ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
    float vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
    float start = atan2f(uy, ux);
    float dot = ux * vx + uy * vy;
    float len = sqrtf((ux * ux + uy * uy) * (vx * vx + vy * vy));
    float delta = acosf(fmaxf(-1.0f, fminf(1.0f, len == 0.0f ? 1.0f : dot / len)));
    if (ux * vy - uy * vx < 0.0f) delta = -delta;
    if (sweep == 0 && delta > 0.0f) delta -= 2.0f * (float)M_PI;
    if (sweep == 1 && delta < 0.0f) delta += 2.0f * (float)M_PI;

    int32_t steps = (int32_t)(fabsf(delta) / (float)M_PI * 24.0f) + 2;
    for (int32_t i = 1; i <= steps; i++) {
        float t = start + delta * (float)i / (float)steps;
        float ex = cx + rx * cosf(t) * cosp - ry * sinf(t) * sinp;
        float ey = cy + rx * cosf(t) * sinp + ry * sinf(t) * cosp;
        icon_path_push(p, ex, ey);
    }
}

/* Parse an SVG path `d` string into flattened subpaths. */
static void icon_parse_path_data(const char *d, int32_t d_len, IconPathSet *set) {
    const char *p = d;
    const char *end = d + d_len;
    IconPath *cur = NULL;
    IconPt pos = {0, 0}, start = {0, 0}, prev_ctrl = {0, 0};
    char cmd = 0, prev_cmd = 0;

    while (p != NULL && p < end) {
        p = icon_skip_sep(p, end);
        if (p >= end) break;
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            cmd = c;
            p++;
        } else if (cmd == 'M') {
            cmd = 'L'; /* implicit lineto chains after moveto */
        } else if (cmd == 'm') {
            cmd = 'l';
        }
        int32_t rel = (cmd >= 'a' && cmd <= 'z');
        char op = (char)(rel ? cmd - 32 : cmd);
        float ox = rel ? pos.x : 0.0f;
        float oy = rel ? pos.y : 0.0f;

        if (op == 'M') {
            float x, y;
            if ((p = icon_read_float(p, end, &x)) == NULL) break;
            if ((p = icon_read_float(p, end, &y)) == NULL) break;
            cur = icon_set_begin(set);
            if (cur == NULL) return;
            pos.x = ox + x;
            pos.y = oy + y;
            start = pos;
            icon_path_push(cur, pos.x, pos.y);
        } else if (op == 'L' || op == 'H' || op == 'V') {
            float x = pos.x, y = pos.y;
            if (op == 'L') {
                if ((p = icon_read_float(p, end, &x)) == NULL) break;
                if ((p = icon_read_float(p, end, &y)) == NULL) break;
                x += ox;
                y += oy;
            } else if (op == 'H') {
                if ((p = icon_read_float(p, end, &x)) == NULL) break;
                x += ox;
            } else {
                if ((p = icon_read_float(p, end, &y)) == NULL) break;
                y += oy;
            }
            if (cur == NULL) {
                cur = icon_set_begin(set);
                if (cur == NULL) return;
                icon_path_push(cur, pos.x, pos.y);
            }
            pos.x = x;
            pos.y = y;
            icon_path_push(cur, pos.x, pos.y);
        } else if (op == 'C' || op == 'S') {
            float x1, y1, x2, y2, x, y;
            if (op == 'C') {
                if ((p = icon_read_float(p, end, &x1)) == NULL) break;
                if ((p = icon_read_float(p, end, &y1)) == NULL) break;
                x1 += ox;
                y1 += oy;
            } else {
                if (prev_cmd == 'C' || prev_cmd == 'S') {
                    x1 = 2.0f * pos.x - prev_ctrl.x;
                    y1 = 2.0f * pos.y - prev_ctrl.y;
                } else {
                    x1 = pos.x;
                    y1 = pos.y;
                }
            }
            if ((p = icon_read_float(p, end, &x2)) == NULL) break;
            if ((p = icon_read_float(p, end, &y2)) == NULL) break;
            if ((p = icon_read_float(p, end, &x)) == NULL) break;
            if ((p = icon_read_float(p, end, &y)) == NULL) break;
            x2 += ox;
            y2 += oy;
            x += ox;
            y += oy;
            if (cur == NULL) {
                cur = icon_set_begin(set);
                if (cur == NULL) return;
                icon_path_push(cur, pos.x, pos.y);
            }
            IconPt c1 = {x1, y1}, c2 = {x2, y2}, p1 = {x, y};
            icon_flatten_cubic(cur, pos, c1, c2, p1);
            prev_ctrl = c2;
            pos = p1;
        } else if (op == 'Q' || op == 'T') {
            float x1, y1, x, y;
            if (op == 'Q') {
                if ((p = icon_read_float(p, end, &x1)) == NULL) break;
                if ((p = icon_read_float(p, end, &y1)) == NULL) break;
                x1 += ox;
                y1 += oy;
            } else {
                if (prev_cmd == 'Q' || prev_cmd == 'T') {
                    x1 = 2.0f * pos.x - prev_ctrl.x;
                    y1 = 2.0f * pos.y - prev_ctrl.y;
                } else {
                    x1 = pos.x;
                    y1 = pos.y;
                }
            }
            if ((p = icon_read_float(p, end, &x)) == NULL) break;
            if ((p = icon_read_float(p, end, &y)) == NULL) break;
            x += ox;
            y += oy;
            if (cur == NULL) {
                cur = icon_set_begin(set);
                if (cur == NULL) return;
                icon_path_push(cur, pos.x, pos.y);
            }
            IconPt c = {x1, y1}, p1 = {x, y};
            icon_flatten_quad(cur, pos, c, p1);
            prev_ctrl = c;
            pos = p1;
        } else if (op == 'A') {
            float rx, ry, rot, laf, swf, x, y;
            if ((p = icon_read_float(p, end, &rx)) == NULL) break;
            if ((p = icon_read_float(p, end, &ry)) == NULL) break;
            if ((p = icon_read_float(p, end, &rot)) == NULL) break;
            if ((p = icon_read_float(p, end, &laf)) == NULL) break;
            if ((p = icon_read_float(p, end, &swf)) == NULL) break;
            if ((p = icon_read_float(p, end, &x)) == NULL) break;
            if ((p = icon_read_float(p, end, &y)) == NULL) break;
            x += ox;
            y += oy;
            if (cur == NULL) {
                cur = icon_set_begin(set);
                if (cur == NULL) return;
                icon_path_push(cur, pos.x, pos.y);
            }
            IconPt p1 = {x, y};
            icon_flatten_arc(cur, pos, rx, ry, rot, laf != 0.0f, swf != 0.0f, p1);
            pos = p1;
        } else if (op == 'Z') {
            if (cur != NULL) {
                cur->closed = 1;
                pos = start;
            }
        } else {
            break; /* unknown command: stop parsing this path */
        }
        prev_cmd = (char)(rel ? op + 32 : op);
        /* normalize: S after C behaves via op chars */
        prev_cmd = op;
    }
}

/* ------------------------------------------------------------ rasterizers */

#define ICON_SUBROWS 4

/* Accumulate fill coverage for one path set into `plane` (px*px floats). */
static void icon_fill(const IconPathSet *set, int32_t px, float *plane, int32_t even_odd) {
    typedef struct {
        float x;
        int32_t dir;
    } Crossing;
    int32_t max_cross = 0;
    for (int32_t i = 0; i < set->count; i++) max_cross += set->paths[i].count + 1;
    if (max_cross == 0) return;
    Crossing *cross = (Crossing *)malloc((size_t)max_cross * sizeof(Crossing));
    if (cross == NULL) return;

    for (int32_t row = 0; row < px; row++) {
        for (int32_t sub = 0; sub < ICON_SUBROWS; sub++) {
            float sy = (float)row + ((float)sub + 0.5f) / (float)ICON_SUBROWS;
            int32_t n = 0;
            for (int32_t i = 0; i < set->count; i++) {
                const IconPath *path = &set->paths[i];
                if (path->count < 3) continue;
                int32_t count = path->count;
                for (int32_t j = 0; j < count; j++) {
                    IconPt a = path->pts[j];
                    IconPt b = path->pts[(j + 1) % count];
                    if (a.y == b.y) continue;
                    float y0 = a.y, y1 = b.y;
                    int32_t dir = 1;
                    if (y0 > y1) {
                        float t = y0;
                        y0 = y1;
                        y1 = t;
                        dir = -1;
                    }
                    if (sy < y0 || sy >= y1) continue;
                    float t = (sy - a.y) / (b.y - a.y);
                    cross[n].x = a.x + t * (b.x - a.x);
                    cross[n].dir = dir;
                    n++;
                }
            }
            if (n == 0) continue;
            /* insertion sort by x (n is small) */
            for (int32_t i = 1; i < n; i++) {
                Crossing key = cross[i];
                int32_t j = i - 1;
                while (j >= 0 && cross[j].x > key.x) {
                    cross[j + 1] = cross[j];
                    j--;
                }
                cross[j + 1] = key;
            }
            /* walk spans */
            int32_t winding = 0;
            for (int32_t i = 0; i + 1 <= n - 1 || i < n; i++) {
                if (i >= n) break;
                int32_t prev_inside = even_odd ? (winding & 1) : (winding != 0);
                winding += even_odd ? 1 : cross[i].dir;
                int32_t now_inside = even_odd ? (winding & 1) : (winding != 0);
                if (!prev_inside && now_inside) {
                    /* span opens at cross[i].x; find close */
                    float open_x = cross[i].x;
                    int32_t w = winding;
                    int32_t k = i + 1;
                    float close_x = open_x;
                    while (k < n) {
                        w += even_odd ? 1 : cross[k].dir;
                        int32_t inside = even_odd ? (w & 1) : (w != 0);
                        if (!inside) {
                            close_x = cross[k].x;
                            break;
                        }
                        k++;
                    }
                    if (k >= n) close_x = (float)px;
                    /* accumulate horizontal coverage into pixels */
                    if (close_x > 0.0f && open_x < (float)px) {
                        float x0 = open_x < 0.0f ? 0.0f : open_x;
                        float x1 = close_x > (float)px ? (float)px : close_x;
                        int32_t c0 = (int32_t)floorf(x0);
                        int32_t c1 = (int32_t)ceilf(x1) - 1;
                        for (int32_t cpx = c0; cpx <= c1 && cpx < px; cpx++) {
                            float l = (float)cpx;
                            float r = l + 1.0f;
                            float covered = fminf(x1, r) - fmaxf(x0, l);
                            if (covered > 0.0f) {
                                plane[row * px + cpx] += covered / (float)ICON_SUBROWS;
                            }
                        }
                    }
                    winding = w;
                    i = k;
                }
            }
        }
    }
    free(cross);
}

/* Accumulate stroke coverage: distance to segments, half-width falloff. */
static void icon_stroke(const IconPathSet *set, int32_t px, float *plane, float half_w) {
    for (int32_t i = 0; i < set->count; i++) {
        const IconPath *path = &set->paths[i];
        if (path->count < 1) continue;
        int32_t seg_count = path->closed ? path->count : path->count - 1;
        if (path->count == 1) seg_count = 1; /* dot: degenerate segment = round dot */
        /* segment bbox pass per pixel is wasteful; loop pixels within padded path bbox */
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
        for (int32_t j = 0; j < path->count; j++) {
            IconPt q = path->pts[j];
            if (q.x < minx) minx = q.x;
            if (q.y < miny) miny = q.y;
            if (q.x > maxx) maxx = q.x;
            if (q.y > maxy) maxy = q.y;
        }
        float pad = half_w + 1.5f;
        int32_t rx0 = (int32_t)floorf(minx - pad);
        int32_t ry0 = (int32_t)floorf(miny - pad);
        int32_t rx1 = (int32_t)ceilf(maxx + pad);
        int32_t ry1 = (int32_t)ceilf(maxy + pad);
        if (rx0 < 0) rx0 = 0;
        if (ry0 < 0) ry0 = 0;
        if (rx1 > px) rx1 = px;
        if (ry1 > px) ry1 = px;

        for (int32_t y = ry0; y < ry1; y++) {
            for (int32_t x = rx0; x < rx1; x++) {
                float cx = (float)x + 0.5f;
                float cy = (float)y + 0.5f;
                float best = 1e9f;
                for (int32_t j = 0; j < seg_count; j++) {
                    IconPt a = path->pts[j];
                    IconPt b = path->pts[(j + 1) % path->count];
                    float abx = b.x - a.x, aby = b.y - a.y;
                    float apx = cx - a.x, apy = cy - a.y;
                    float denom = abx * abx + aby * aby;
                    float t = denom == 0.0f ? 0.0f : (apx * abx + apy * aby) / denom;
                    if (t < 0.0f) t = 0.0f;
                    if (t > 1.0f) t = 1.0f;
                    float dx = apx - t * abx;
                    float dy = apy - t * aby;
                    float d2 = dx * dx + dy * dy;
                    if (d2 < best) best = d2;
                }
                float dist = sqrtf(best);
                float cov = 0.5f + (half_w - dist) / 1.0f; /* ~1px AA ramp */
                if (cov <= 0.0f) continue;
                if (cov > 1.0f) cov = 1.0f;
                float *slot = &plane[y * px + x];
                if (cov > *slot) *slot = cov;
            }
        }
    }
}

/* ---------------------------------------------------------------- parsing */

typedef struct {
    float scale;
    float tx, ty;
} IconXf;

static void icon_xf_apply(const IconXf *xf, IconPathSet *set) {
    for (int32_t i = 0; i < set->count; i++) {
        for (int32_t j = 0; j < set->paths[i].count; j++) {
            set->paths[i].pts[j].x = set->paths[i].pts[j].x * xf->scale + xf->tx;
            set->paths[i].pts[j].y = set->paths[i].pts[j].y * xf->scale + xf->ty;
        }
    }
}

/* Where a symbol's ink actually sits inside its own box, as fractions of that
 * box measured from its top edge.
 *
 * A library is not one shape: a folder fills the middle of its canvas, an open
 * folder is drawn lower and shorter, and every symbol has its own margins. One
 * constant for the whole set puts each of them somewhere slightly different
 * against the words beside it, which is exactly what a caller cannot correct
 * for. So the engine answers per symbol, the way a font answers per glyph.
 *
 * Measured off a small rasterization rather than off the path data, because ink
 * is what the STROKES cover and not where the outlines run. The answers are
 * cached: a bar redraws the same handful of symbols every frame.
 */
#define ICON_INK_PROBE 64
#define ICON_INK_CACHE 32

typedef struct {
    int64_t hash;
    float left;
    float top;
    float right;
    float bottom;
    /* How much of the ink's own box the drawing covers: a stroked symbol is
     * mostly the paper it is drawn on, a solid one is mostly ink. */
    float density;
    int32_t valid;
} IconInkEntry;

static IconInkEntry icon_ink_cache[ICON_INK_CACHE];
static int32_t icon_ink_next = 0;

static int32_t icon_ink_measure(const char *svg_utf8, IconInkEntry *out) {
    uint8_t *probe = (uint8_t *)malloc((size_t)ICON_INK_PROBE * (size_t)ICON_INK_PROBE);
    if (probe == NULL) return 0;
    if (!kira_icon_rasterize(svg_utf8, ICON_INK_PROBE, probe)) {
        free(probe);
        return 0;
    }
    int32_t first_row = -1, last_row = -1, first_col = ICON_INK_PROBE, last_col = -1;
    int64_t covered = 0;
    for (int32_t y = 0; y < ICON_INK_PROBE; y++) {
        for (int32_t x = 0; x < ICON_INK_PROBE; x++) {
            if (probe[y * ICON_INK_PROBE + x] < 16) continue;
            covered++;
            if (first_row < 0) first_row = y;
            last_row = y;
            if (x < first_col) first_col = x;
            if (x > last_col) last_col = x;
        }
    }
    free(probe);
    if (first_row < 0) return 0;
    out->left = (float)first_col / (float)ICON_INK_PROBE;
    out->top = (float)first_row / (float)ICON_INK_PROBE;
    out->right = (float)(last_col + 1) / (float)ICON_INK_PROBE;
    out->bottom = (float)(last_row + 1) / (float)ICON_INK_PROBE;
    int64_t box = (int64_t)(last_col - first_col + 1) * (int64_t)(last_row - first_row + 1);
    out->density = box > 0 ? (float)((double)covered / (double)box) : 0.0f;
    out->valid = 1;
    return 1;
}

static const IconInkEntry *icon_ink_entry(const char *svg_utf8) {
    if (svg_utf8 == NULL) return NULL;
    int64_t hash = kira_icon_hash(svg_utf8);
    for (int32_t i = 0; i < ICON_INK_CACHE; i++) {
        if (icon_ink_cache[i].valid && icon_ink_cache[i].hash == hash) {
            return &icon_ink_cache[i];
        }
    }
    IconInkEntry measured;
    memset(&measured, 0, sizeof(measured));
    if (!icon_ink_measure(svg_utf8, &measured)) return NULL;
    measured.hash = hash;
    int32_t slot = icon_ink_next;
    icon_ink_cache[slot] = measured;
    icon_ink_next = (icon_ink_next + 1) % ICON_INK_CACHE;
    return &icon_ink_cache[slot];
}

int32_t kira_icon_ink_bounds(const char *svg_utf8, double *out_top, double *out_bottom) {
    if (out_top == NULL || out_bottom == NULL) return 0;
    *out_top = 0.0;
    *out_bottom = 1.0;
    const IconInkEntry *entry = icon_ink_entry(svg_utf8);
    if (entry == NULL) return 0;
    *out_top = (double)entry->top;
    *out_bottom = (double)entry->bottom;
    return 1;
}

double kira_icon_ink_top(const char *svg_utf8) {
    const IconInkEntry *entry = icon_ink_entry(svg_utf8);
    if (entry == NULL) return 0.0;
    return (double)entry->top;
}

double kira_icon_ink_bottom(const char *svg_utf8) {
    const IconInkEntry *entry = icon_ink_entry(svg_utf8);
    if (entry == NULL) return 1.0;
    return (double)entry->bottom;
}

/* How much of its own box a symbol's drawing covers.
 *
 * A stroked symbol — a folder, a chevron — is mostly the paper inside its
 * outline, and answers low. A solid one — a cursor, a filled marker — answers
 * high. It is the difference between a symbol that HAS stems to match against
 * type and one that has none: growing the second only swells the shape. */
double kira_icon_ink_density(const char *svg_utf8) {
    const IconInkEntry *entry = icon_ink_entry(svg_utf8);
    if (entry == NULL) return 0.0;
    return (double)entry->density;
}

/* How wide a symbol's ink is against how tall it is. A folder is wider than it
 * is tall and a chevron is narrower; a caller that wants the ink itself to
 * occupy a box needs the shape of that box, not a square. */
double kira_icon_ink_aspect(const char *svg_utf8) {
    const IconInkEntry *entry = icon_ink_entry(svg_utf8);
    if (entry == NULL) return 1.0;
    float h = entry->bottom - entry->top;
    if (h <= 0.0f) return 1.0;
    return (double)((entry->right - entry->left) / h);
}

/* Rasterize so the symbol's INK fills the output exactly: no canvas margin on
 * any side.
 *
 * The margin around a symbol's drawing belongs to the canvas it was authored
 * on, not to the symbol. Left in, it is dead space that pushes the glyph in from
 * the edge a run of text starts at and holds it off the line it should sit on —
 * and it differs per symbol, so no caller can correct for it. Trimmed, a
 * symbol's box IS its ink: it starts where a letter would start, and it stands
 * on the line a letter stands on.
 */
int32_t kira_icon_rasterize_ink(const char *svg_utf8, int32_t out_w, int32_t out_h, double dilate_px, uint8_t *out_buffer) {
    if (out_buffer == NULL || out_w <= 0 || out_h <= 0) return 0;
    memset(out_buffer, 0, (size_t)out_w * (size_t)out_h);
    const IconInkEntry *entry = icon_ink_entry(svg_utf8);
    if (entry == NULL) return 0;
    float ink_h = entry->bottom - entry->top;
    if (ink_h <= 0.0f) return 0;

    /* The square the whole canvas has to be drawn at for its ink to come out
     * `out_h` tall, and where the ink sits inside it. */
    int32_t square = (int32_t)((float)out_h / ink_h + 0.5f);
    if (square <= 0) return 0;
    uint8_t *full = (uint8_t *)malloc((size_t)square * (size_t)square);
    if (full == NULL) return 0;
    if (!kira_icon_rasterize_weighted(svg_utf8, square, dilate_px, full)) {
        free(full);
        return 0;
    }
    int32_t src_x = (int32_t)(entry->left * (float)square + 0.5f);
    int32_t src_y = (int32_t)(entry->top * (float)square + 0.5f);
    for (int32_t y = 0; y < out_h; y++) {
        int32_t sy = src_y + y;
        if (sy < 0 || sy >= square) continue;
        for (int32_t x = 0; x < out_w; x++) {
            int32_t sx = src_x + x;
            if (sx < 0 || sx >= square) continue;
            out_buffer[y * out_w + x] = full[sy * square + sx];
        }
    }
    free(full);
    return 1;
}

int32_t kira_icon_rasterize(const char *svg_utf8, int32_t px, uint8_t *out_buffer) {
    return kira_icon_rasterize_weighted(svg_utf8, px, 0.0, out_buffer);
}

/* Coverage grown outward by `radius` pixels: the weight of a symbol, the way a
 * heavier cut of a typeface is a heavier stem. The artwork is drawn once at one
 * weight, so matching the words beside it means thickening what was drawn --
 * a maximum over a disc of that radius, which grows every edge outward by it and
 * leaves the shape's corners round rather than square. */
static void icon_dilate_radius(const uint8_t *src, uint8_t *dst, int32_t px, int32_t r) {
    float rr = (float)r * (float)r;
    for (int32_t y = 0; y < px; y++) {
        for (int32_t x = 0; x < px; x++) {
            int32_t best = src[y * px + x];
            for (int32_t dy = -r; dy <= r && best < 255; dy++) {
                int32_t sy = y + dy;
                if (sy < 0 || sy >= px) continue;
                for (int32_t dx = -r; dx <= r; dx++) {
                    int32_t sx = x + dx;
                    if (sx < 0 || sx >= px) continue;
                    if ((float)(dx * dx + dy * dy) > rr) continue;
                    int32_t v = src[sy * px + sx];
                    if (v > best) best = v;
                    if (best >= 255) break;
                }
            }
            dst[y * px + x] = (uint8_t)best;
        }
    }
}

/* Coverage grown outward by `radius` pixels: the WEIGHT of a symbol, the way a
 * heavier cut of a typeface is a heavier stem. The artwork is drawn once at one
 * weight, so matching the words beside it means thickening what was drawn.
 *
 * A whole-pixel grow is a maximum over a disc of that radius. A fraction of a
 * pixel is the two whole grows either side of it, mixed — a symbol's weight has
 * to follow the type's continuously, and a grow that could only step by whole
 * pixels would jump from too light to too heavy between two font sizes. */
static void icon_dilate(uint8_t *cov, int32_t px, float radius) {
    if (radius <= 0.0f || px <= 0) return;
    int32_t lo = (int32_t)radius;
    float frac = radius - (float)lo;
    size_t bytes = (size_t)px * (size_t)px;
    uint8_t *src = (uint8_t *)malloc(bytes);
    uint8_t *low = (uint8_t *)malloc(bytes);
    uint8_t *high = (uint8_t *)malloc(bytes);
    if (src == NULL || low == NULL || high == NULL) {
        free(src);
        free(low);
        free(high);
        return;
    }
    memcpy(src, cov, bytes);
    if (lo <= 0) {
        memcpy(low, src, bytes);
    } else {
        icon_dilate_radius(src, low, px, lo);
    }
    icon_dilate_radius(src, high, px, lo + 1);
    for (size_t i = 0; i < bytes; i++) {
        float mixed = (float)low[i] + ((float)high[i] - (float)low[i]) * frac;
        if (mixed < 0.0f) mixed = 0.0f;
        if (mixed > 255.0f) mixed = 255.0f;
        cov[i] = (uint8_t)(mixed + 0.5f);
    }
    free(src);
    free(low);
    free(high);
}

int32_t kira_icon_rasterize_weighted(const char *svg_utf8, int32_t px, double dilate_px, uint8_t *out_buffer) {
    if (out_buffer == NULL || px <= 0) return 0;
    memset(out_buffer, 0, (size_t)px * (size_t)px);
    if (svg_utf8 == NULL) return 0;

    const char *doc = svg_utf8;
    const char *doc_end = doc + strlen(doc);

    /* viewBox (fall back to width/height, then 24x24). */
    float vb_x = 0.0f, vb_y = 0.0f, vb_w = 24.0f, vb_h = 24.0f;
    float svg_stroke_w = 0.0f;
    int32_t svg_stroke_none = 1;
    int32_t svg_fill_none = 0;
    {
        const char *tag = strstr(doc, "<svg");
        if (tag != NULL) {
            const char *tag_end = (const char *)memchr(tag, '>', (size_t)(doc_end - tag));
            if (tag_end == NULL) tag_end = doc_end;
            int32_t len = 0;
            const char *vb = icon_attr(tag, tag_end, "viewBox", &len);
            if (vb != NULL) {
                char buf[96];
                int32_t n = len < 95 ? len : 95;
                memcpy(buf, vb, (size_t)n);
                buf[n] = 0;
                float a, b, c, d;
                if (sscanf(buf, "%f %f %f %f", &a, &b, &c, &d) == 4 ||
                    sscanf(buf, "%f,%f,%f,%f", &a, &b, &c, &d) == 4) {
                    vb_x = a;
                    vb_y = b;
                    vb_w = c;
                    vb_h = d;
                }
            } else {
                vb_w = icon_attr_float(tag, tag_end, "width", 24.0f);
                vb_h = icon_attr_float(tag, tag_end, "height", 24.0f);
            }
            const char *sv = icon_attr(tag, tag_end, "stroke", &len);
            if (sv != NULL && !(len == 4 && strncmp(sv, "none", 4) == 0)) svg_stroke_none = 0;
            svg_stroke_w = icon_attr_float(tag, tag_end, "stroke-width", 2.0f);
            svg_fill_none = icon_attr_is(tag, tag_end, "fill", "none");
        }
    }
    if (vb_w <= 0.0f || vb_h <= 0.0f) return 0;

    /* document -> pixel transform: fit, preserve aspect, center. */
    IconXf xf;
    float extent = vb_w > vb_h ? vb_w : vb_h;
    xf.scale = (float)px / extent;
    xf.tx = -vb_x * xf.scale + ((float)px - vb_w * xf.scale) / 2.0f;
    xf.ty = -vb_y * xf.scale + ((float)px - vb_h * xf.scale) / 2.0f;

    float *plane = (float *)calloc((size_t)px * (size_t)px, sizeof(float));
    if (plane == NULL) return 0;

    /* Iterate drawable elements. */
    const char *p = doc;
    while ((p = strchr(p, '<')) != NULL) {
        const char *tag = p;
        const char *tag_end = strchr(tag, '>');
        if (tag_end == NULL) break;
        p = tag_end + 1;

        const char *name = tag + 1;
        IconPathSet set;
        memset(&set, 0, sizeof(set));
        int32_t drew = 0;

        if (strncmp(name, "path", 4) == 0 && (name[4] == ' ' || name[4] == '\t' || name[4] == '/' || name[4] == '>')) {
            int32_t d_len = 0;
            const char *d = icon_attr(tag, tag_end, "d", &d_len);
            if (d != NULL && d_len > 0) {
                icon_parse_path_data(d, d_len, &set);
                drew = 1;
            }
        } else if (strncmp(name, "circle", 6) == 0 || strncmp(name, "ellipse", 7) == 0) {
            float cx = icon_attr_float(tag, tag_end, "cx", 0.0f);
            float cy = icon_attr_float(tag, tag_end, "cy", 0.0f);
            float rx = icon_attr_float(tag, tag_end, "r", -1.0f);
            float ry = rx;
            if (rx < 0.0f) {
                rx = icon_attr_float(tag, tag_end, "rx", 0.0f);
                ry = icon_attr_float(tag, tag_end, "ry", rx);
            }
            if (rx > 0.0f && ry > 0.0f) {
                IconPath *c = icon_set_begin(&set);
                if (c != NULL) {
                    for (int32_t i = 0; i < 48; i++) {
                        float t = (float)i / 48.0f * 2.0f * (float)M_PI;
                        icon_path_push(c, cx + rx * cosf(t), cy + ry * sinf(t));
                    }
                    c->closed = 1;
                    drew = 1;
                }
            }
        } else if (strncmp(name, "rect", 4) == 0) {
            float x = icon_attr_float(tag, tag_end, "x", 0.0f);
            float y = icon_attr_float(tag, tag_end, "y", 0.0f);
            float w = icon_attr_float(tag, tag_end, "width", 0.0f);
            float h = icon_attr_float(tag, tag_end, "height", 0.0f);
            float rx = icon_attr_float(tag, tag_end, "rx", 0.0f);
            if (w > 0.0f && h > 0.0f) {
                IconPath *c = icon_set_begin(&set);
                if (c != NULL) {
                    if (rx <= 0.01f) {
                        icon_path_push(c, x, y);
                        icon_path_push(c, x + w, y);
                        icon_path_push(c, x + w, y + h);
                        icon_path_push(c, x, y + h);
                    } else {
                        float mr = w < h ? w / 2.0f : h / 2.0f;
                        if (rx > mr) rx = mr;
                        for (int32_t corner = 0; corner < 4; corner++) {
                            float ccx = corner == 0 || corner == 3 ? x + rx : x + w - rx;
                            float ccy = corner < 2 ? y + rx : y + h - rx;
                            float base = (float)M_PI + (float)corner * (float)M_PI / 2.0f;
                            if (corner == 3) { ccx = x + rx; ccy = y + h - rx; }
                            if (corner == 0) { ccx = x + rx; ccy = y + rx; }
                            if (corner == 1) { ccx = x + w - rx; ccy = y + rx; }
                            if (corner == 2) { ccx = x + w - rx; ccy = y + h - rx; }
                            for (int32_t i = 0; i <= 12; i++) {
                                float t = base + (float)i / 12.0f * (float)M_PI / 2.0f;
                                icon_path_push(c, ccx + rx * cosf(t), ccy + rx * sinf(t));
                            }
                        }
                    }
                    c->closed = 1;
                    drew = 1;
                }
            }
        } else if (strncmp(name, "line", 4) == 0 && (name[4] == ' ' || name[4] == '\t')) {
            IconPath *c = icon_set_begin(&set);
            if (c != NULL) {
                icon_path_push(c, icon_attr_float(tag, tag_end, "x1", 0.0f), icon_attr_float(tag, tag_end, "y1", 0.0f));
                icon_path_push(c, icon_attr_float(tag, tag_end, "x2", 0.0f), icon_attr_float(tag, tag_end, "y2", 0.0f));
                drew = 1;
            }
        } else if (strncmp(name, "polyline", 8) == 0 || strncmp(name, "polygon", 7) == 0) {
            int32_t pts_len = 0;
            const char *pts = icon_attr(tag, tag_end, "points", &pts_len);
            if (pts != NULL) {
                IconPath *c = icon_set_begin(&set);
                if (c != NULL) {
                    const char *q = pts;
                    const char *q_end = pts + pts_len;
                    float x, y;
                    while ((q = icon_read_float(q, q_end, &x)) != NULL) {
                        if ((q = icon_read_float(q, q_end, &y)) == NULL) break;
                        icon_path_push(c, x, y);
                    }
                    if (name[4] == 'g') c->closed = 1; /* polygon */
                    drew = 1;
                }
            }
        }

        if (drew && set.count > 0) {
            icon_xf_apply(&xf, &set);

            /* paint resolution: element attribute overrides the svg default */
            int32_t fill_none = svg_fill_none;
            if (icon_attr_is(tag, tag_end, "fill", "none")) fill_none = 1;
            int32_t len = 0;
            if (icon_attr(tag, tag_end, "fill", &len) != NULL && !icon_attr_is(tag, tag_end, "fill", "none")) fill_none = 0;
            int32_t stroke_none = svg_stroke_none;
            const char *sv = icon_attr(tag, tag_end, "stroke", &len);
            if (sv != NULL) stroke_none = (len == 4 && strncmp(sv, "none", 4) == 0);
            float stroke_w = icon_attr_float(tag, tag_end, "stroke-width", svg_stroke_w);

            /* lines/polylines have no interior */
            int32_t fillable = 1;
            if (strncmp(name, "line", 4) == 0 || strncmp(name, "polyline", 8) == 0) fillable = 0;

            if (!fill_none && fillable) {
                int32_t even_odd = icon_attr_is(tag, tag_end, "fill-rule", "evenodd");
                icon_fill(&set, px, plane, even_odd);
            }
            if (!stroke_none && stroke_w > 0.0f) {
                icon_stroke(&set, px, plane, stroke_w * xf.scale / 2.0f);
            }
        }
        icon_set_free(&set);
    }

    for (int32_t i = 0; i < px * px; i++) {
        float cov = plane[i];
        if (cov < 0.0f) cov = 0.0f;
        if (cov > 1.0f) cov = 1.0f;
        out_buffer[i] = (unsigned char)(cov * 255.0f + 0.5f);
    }
    free(plane);
    icon_dilate(out_buffer, px, (float)dilate_px);
    return 1;
}
