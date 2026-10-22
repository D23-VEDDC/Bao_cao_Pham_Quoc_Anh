#include "pcb_preprocess.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SW 160
#define SH 120
#define COUNT (SW * SH)

static const char *TAG = "pcb_preprocess";

/* The thresholds and edge refinement follow the PCB demo supplied by the
 * user. Detection operates on the RGB frame already decoded for AI, so the
 * camera JPEG is never decoded a second time. */
static void close_mask(const uint8_t *src, uint8_t *tmp, uint8_t *dst)
{
    for (int y = 0; y < SH; ++y) for (int x = 0; x < SW; ++x) {
        uint8_t hit = 0;
        for (int dy = -2; dy <= 2 && !hit; ++dy) {
            int yy = y + dy;
            if (yy < 0 || yy >= SH) continue;
            for (int dx = -2; dx <= 2; ++dx) {
                int xx = x + dx;
                if (xx >= 0 && xx < SW && src[yy * SW + xx]) { hit = 1; break; }
            }
        }
        tmp[y * SW + x] = hit;
    }
    for (int y = 0; y < SH; ++y) for (int x = 0; x < SW; ++x) {
        uint8_t full = 1;
        for (int dy = -2; dy <= 2 && full; ++dy) {
            int yy = y + dy;
            if (yy < 0 || yy >= SH) { full = 0; break; }
            for (int dx = -2; dx <= 2; ++dx) {
                int xx = x + dx;
                if (xx < 0 || xx >= SW || !tmp[yy * SW + xx]) { full = 0; break; }
            }
        }
        dst[y * SW + x] = full;
    }
}

static int low_bin(const uint16_t *hist, int trim)
{
    int sum = 0;
    for (int i = 0; i < 512; ++i) { sum += hist[i]; if (sum > trim) return i; }
    return 511;
}

static int high_bin(const uint16_t *hist, int trim)
{
    int sum = 0;
    for (int i = 511; i >= 0; --i) { sum += hist[i]; if (sum > trim) return i; }
    return 0;
}

static uint32_t strongest_four(const uint16_t *hist)
{
    uint16_t top[4] = {0};
    for (int i = 0; i < 512; ++i) for (int j = 0; j < 4; ++j) {
        if (hist[i] <= top[j]) continue;
        for (int k = 3; k > j; --k) top[k] = top[k - 1];
        top[j] = hist[i];
        break;
    }
    return (uint32_t)top[0] + top[1] + top[2] + top[3];
}

static void estimate_rotation(const uint8_t *rgb, const uint16_t *labels,
                              uint16_t selected_label,
                              float *best_c, float *best_s, float *best_deg)
{
    uint32_t best_score = 0;
    for (int step = -20; step <= 20; ++step) {
        float deg = (float)step;
        float rad = deg * 0.01745329252f;
        float c = cosf(rad), s = sinf(rad);
        uint16_t uh[512] = {0}, vh[512] = {0};
        for (int y = 1; y < SH - 1; ++y) for (int x = 1; x < SW - 1; ++x) {
            if (labels) {
                int i = y * SW + x;
                if (labels[i] != selected_label ||
                    (labels[i - 1] == selected_label && labels[i + 1] == selected_label &&
                     labels[i - SW] == selected_label && labels[i + SW] == selected_label)) continue;
            }
            int p = (y * SW + x) * 3 + 1;
            int gx = (int)rgb[p + 3] - (int)rgb[p - 3];
            int gy = (int)rgb[p + SW * 3] - (int)rgb[p - SW * 3];
            int u = (int)floorf(x * c + y * s) + 256;
            int v = (int)floorf(-x * s + y * c) + 256;
            if (u >= 0 && u < 512) uh[u] += (uint16_t)fabsf(c * gx + s * gy);
            if (v >= 0 && v < 512) vh[v] += (uint16_t)fabsf(-s * gx + c * gy);
        }
        uint32_t score = strongest_four(uh) + strongest_four(vh);
        if (score > best_score) {
            best_score = score;
            *best_c = c; *best_s = s; *best_deg = deg;
        }
        if ((step & 7) == 0) vTaskDelay(1);
    }
}

static float edge_score(const uint8_t *rgb, int side, int candidate,
                        int other_lo, int other_hi, float c, float s)
{
    int16_t differences[45];
    int count = 0, sum = 0;
    bool u_side = side < 2;
    int inside = (side == 0 || side == 2) ? 3 : -3;
    float span = (float)(other_hi - other_lo);
    for (int i = 0; i < 45; ++i) {
        float other = other_lo + span * (0.2f + 0.6f * i / 44.0f);
        float ui = u_side ? candidate + inside : other;
        float vi = u_side ? other : candidate + inside;
        float uo = u_side ? candidate - inside : other;
        float vo = u_side ? other : candidate - inside;
        int xi = (int)roundf(ui * c - vi * s);
        int yi = (int)roundf(ui * s + vi * c);
        int xo = (int)roundf(uo * c - vo * s);
        int yo = (int)roundf(uo * s + vo * c);
        if (xi < 0 || xi >= SW || yi < 0 || yi >= SH ||
            xo < 0 || xo >= SW || yo < 0 || yo >= SH) continue;
        int difference = (int)rgb[(yi * SW + xi) * 3 + 1] -
                         (int)rgb[(yo * SW + xo) * 3 + 1];
        differences[count++] = (int16_t)difference;
        sum += difference;
    }
    if (count < 30) return -1e6f;
    for (int i = 1; i < count; ++i) {
        int16_t value = differences[i];
        int j = i;
        while (j > 0 && differences[j - 1] > value) {
            differences[j] = differences[j - 1]; --j;
        }
        differences[j] = value;
    }
    return differences[count / 2] + 0.25f * sum / count;
}

static void refine_edges(const uint8_t *rgb, float c, float s, int search_radius,
                         int *lu, int *hu, int *lv, int *hv)
{
    int bounds[4] = {*lu, *hu, *lv, *hv};
    for (int side = 0; side < 4; ++side) {
        int original = bounds[side];
        int other_lo = side < 2 ? bounds[2] : bounds[0];
        int other_hi = side < 2 ? bounds[3] : bounds[1];
        float best_score = 5.0f;
        int best = original;
        for (int candidate = original - search_radius;
             candidate <= original + search_radius; ++candidate) {
            if (candidate < -32 || candidate > 192) continue;
            if ((side == 0 && candidate >= bounds[1] - 40) ||
                (side == 1 && candidate <= bounds[0] + 40) ||
                (side == 2 && candidate >= bounds[3] - 30) ||
                (side == 3 && candidate <= bounds[2] + 30)) continue;
            float score = edge_score(rgb, side, candidate, other_lo, other_hi, c, s);
            score -= 0.2f * abs(candidate - original);
            if (score > best_score) { best_score = score; best = candidate; }
        }
        bounds[side] = best;
    }
    *lu = bounds[0]; *hu = bounds[1]; *lv = bounds[2]; *hv = bounds[3];
}

/* The yellow board is a distinct substrate, so its component boundary also
 * gives us the four physical edges. A rotated rectangle leaves wedges of the
 * camera mat in a perspective view. Fit each edge on the middle 70% of the
 * opposite axis, excluding corners and isolated highlight pixels. */
static bool fit_line(const float *axis, const float *edge, int count,
                     float *slope, float *intercept)
{
    if (count < 20) return false;
    float m = 0, b = 0;
    for (int pass = 0; pass < 2; ++pass) {
        float sum_t = 0, sum_e = 0;
        int kept = 0;
        for (int i = 0; i < count; ++i) {
            if (pass && fabsf(edge[i] - (m * axis[i] + b)) > 3.5f) continue;
            sum_t += axis[i]; sum_e += edge[i]; ++kept;
        }
        if (kept < 20) return false;
        float mean_t = sum_t / kept, mean_e = sum_e / kept;
        float covariance = 0, variance = 0;
        for (int i = 0; i < count; ++i) {
            if (pass && fabsf(edge[i] - (m * axis[i] + b)) > 3.5f) continue;
            float dt = axis[i] - mean_t;
            covariance += dt * (edge[i] - mean_e);
            variance += dt * dt;
        }
        if (variance < 1.0f) return false;
        m = covariance / variance;
        b = mean_e - m * mean_t;
    }
    if (fabsf(m) > 0.45f) return false;
    *slope = m; *intercept = b;
    return true;
}

static bool fit_yellow_edge(const uint16_t *labels, uint16_t selected_label,
                            int min_x, int min_y, int max_x, int max_y,
                            int side, float *slope, float *intercept)
{
    float axis[SW], edge[SW];
    int count = 0;
    bool vertical = side < 2;
    int axis_lo = vertical ? min_y : min_x;
    int axis_hi = vertical ? max_y : max_x;
    int inset = (axis_hi - axis_lo) * 15 / 100;
    for (int t = axis_lo + inset; t <= axis_hi - inset; ++t) {
        int lo = vertical ? min_x : min_y;
        int hi = vertical ? max_x : max_y;
        int first = hi + 1, last = lo - 1;
        for (int v = lo; v <= hi; ++v) {
            int p = vertical ? t * SW + v : v * SW + t;
            if (labels[p] != selected_label) continue;
            if (first > hi) first = v;
            last = v;
        }
        int span = vertical ? max_x - min_x : max_y - min_y;
        if (last - first < span * 55 / 100) continue;
        axis[count] = (float)t;
        edge[count++] = (float)((side == 0 || side == 2) ? first : last);
    }
    return fit_line(axis, edge, count, slope, intercept);
}

static bool intersect_edges(float x_slope, float x_intercept,
                            float y_slope, float y_intercept,
                            float *x, float *y)
{
    float denominator = 1.0f - x_slope * y_slope;
    if (fabsf(denominator) < 0.5f) return false;
    *x = (x_slope * y_intercept + x_intercept) / denominator;
    *y = y_slope * *x + y_intercept;
    return true;
}

static bool fit_yellow_quad(const uint16_t *labels, uint16_t selected_label,
                            pcb_box_t *box)
{
    int min_x = SW, min_y = SH, max_x = -1, max_y = -1;
    for (int p = 0; p < COUNT; ++p) {
        if (labels[p] != selected_label) continue;
        int x = p % SW, y = p / SW;
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    if (max_x - min_x < 40 || max_y - min_y < 30) return false;
    float slope[4], intercept[4];
    for (int side = 0; side < 4; ++side) {
        if (!fit_yellow_edge(labels, selected_label, min_x, min_y, max_x, max_y,
                             side, &slope[side], &intercept[side])) return false;
    }
    /* Include the complete edge pixel while excluding the surrounding mat. */
    intercept[0] -= 0.5f;
    intercept[1] += 0.5f;
    intercept[2] -= 0.5f;
    intercept[3] += 0.5f;
    float corners[4][2];
    const int xs[4] = {0, 1, 1, 0};
    const int ys[4] = {2, 2, 3, 3};
    for (int i = 0; i < 4; ++i) {
        float x, y;
        if (!intersect_edges(slope[xs[i]], intercept[xs[i]],
                             slope[ys[i]], intercept[ys[i]], &x, &y)) return false;
        corners[i][0] = x * 4.0f;
        corners[i][1] = y * 4.0f;
        if (corners[i][0] < 0 || corners[i][0] >= 640 ||
            corners[i][1] < 0 || corners[i][1] >= 480) return false;
    }
    float top = hypotf(corners[1][0] - corners[0][0], corners[1][1] - corners[0][1]);
    float bottom = hypotf(corners[2][0] - corners[3][0], corners[2][1] - corners[3][1]);
    float left = hypotf(corners[3][0] - corners[0][0], corners[3][1] - corners[0][1]);
    float right = hypotf(corners[2][0] - corners[1][0], corners[2][1] - corners[1][1]);
    if (fminf(top, bottom) < 160 || fminf(left, right) < 120 ||
        fmaxf(top, bottom) / fminf(top, bottom) > 1.4f ||
        fmaxf(left, right) / fminf(left, right) > 1.4f) return false;
    memcpy(box->corners, corners, sizeof(corners));
    box->angle_deg = atan2f(corners[1][1] - corners[0][1],
                            corners[1][0] - corners[0][0]) * 57.2957795f;
    return true;
}

bool pcb_detect_rgb(const uint8_t *source, int width, int height, pcb_box_t *box)
{
    if (!source || !box || width != 640 || height != 480) return false;
    memset(box, 0, sizeof(*box));
    uint8_t *rgb = heap_caps_malloc(COUNT * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *mask = heap_caps_malloc(COUNT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *tmp = heap_caps_malloc(COUNT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *labels = heap_caps_calloc(COUNT, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *queue = heap_caps_malloc(COUNT * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool found = false;
    if (!rgb || !mask || !tmp || !labels || !queue) goto done;

    for (int y = 0; y < SH; ++y) for (int x = 0; x < SW; ++x) {
        int sums[3] = {0};
        for (int dy = 0; dy < 4; ++dy) for (int dx = 0; dx < 4; ++dx) {
            const uint8_t *p = source + ((size_t)(y * 4 + dy) * width + x * 4 + dx) * 3;
            for (int ch = 0; ch < 3; ++ch) sums[ch] += p[ch];
        }
        int i = (y * SW + x) * 3;
        for (int ch = 0; ch < 3; ++ch) rgb[i + ch] = (uint8_t)(sums[ch] / 16);
    }

    int best_count = 0;
    uint16_t best_label = 0;
    bool yellow_selected = false;
    /* Try the yellow substrate first. The dark green camera mat can itself
     * satisfy the original green threshold, so combining both colors in one
     * mask would let the mat swallow a yellow PCB. If no sizeable yellow
     * rectangle is present, use the original green detector unchanged. */
    for (int color = 0; color < 2; ++color) {
        for (int p = 0; p < COUNT; ++p) {
            int i = p * 3;
            int r = rgb[i], g = rgb[i + 1], b = rgb[i + 2];
            bool yellow = r >= 75 && g >= 85 && g - b >= 8 &&
                          r >= g - 32 && r <= g + 80;
            bool green = g - b > 14 && g - r > 48 && g > 80;
            mask[p] = (color == 0 ? yellow : green) ? 1 : 0;
        }
        close_mask(mask, tmp, mask);
        memset(labels, 0, COUNT * sizeof(*labels));
        best_count = 0;
        best_label = 0;
        uint16_t next_label = 0;
        int best_min_x = SW, best_min_y = SH, best_max_x = -1, best_max_y = -1;
        for (int start = 0; start < COUNT; ++start) {
            if (!mask[start] || labels[start]) continue;
            ++next_label;
            int head = 0, tail = 0;
            int min_x = SW, min_y = SH, max_x = -1, max_y = -1;
            queue[tail++] = (uint16_t)start;
            labels[start] = next_label;
            while (head < tail) {
                int p = queue[head++];
                int x = p % SW, y = p / SW;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
#define ENQUEUE(n) do { if (mask[(n)] && !labels[(n)]) { labels[(n)] = next_label; queue[tail++] = (uint16_t)(n); } } while (0)
                if (x > 0) ENQUEUE(p - 1);
                if (x + 1 < SW) ENQUEUE(p + 1);
                if (y > 0) ENQUEUE(p - SW);
                if (y + 1 < SH) ENQUEUE(p + SW);
#undef ENQUEUE
            }
            if (tail > best_count) {
                best_count = tail;
                best_label = next_label;
                best_min_x = min_x; best_min_y = min_y;
                best_max_x = max_x; best_max_y = max_y;
            }
        }
        ESP_LOGI(TAG, "%s candidate: pixels=%d bounds=(%d,%d)-(%d,%d)",
                 color == 0 ? "yellow" : "green", best_count,
                 best_min_x, best_min_y, best_max_x, best_max_y);
        if (color == 0) {
            int box_w = best_max_x - best_min_x + 1;
            int box_h = best_max_y - best_min_y + 1;
            if (best_count >= 600 && best_count <= COUNT * 9 / 10 &&
                box_w >= 40 && box_h >= 30 &&
                box_w <= SW * 9 / 10 && box_h <= SH * 9 / 10 &&
                best_count * 100 >= box_w * box_h * 45) {
                yellow_selected = true;
                break;
            }
        }
    }
    if (best_count < 600 || best_count > COUNT * 9 / 10) {
        ESP_LOGW(TAG, "PCB mask rejected: color=%s largest=%d pixels",
                 yellow_selected ? "yellow" : "green", best_count);
        goto done;
    }

    float c = 1, s = 0;
    estimate_rotation(rgb, labels, best_label,
                      &c, &s, &box->angle_deg);
    int trim = best_count * 3 / 1000;
    uint16_t uh[512] = {0}, vh[512] = {0};
    for (int p = 0; p < COUNT; ++p) {
        if (labels[p] != best_label) continue;
        float x = p % SW, y = p / SW;
        int u = (int)floorf(x * c + y * s) + 256;
        int v = (int)floorf(-x * s + y * c) + 256;
        if (u >= 0 && u < 512) ++uh[u];
        if (v >= 0 && v < 512) ++vh[v];
    }
    int lu = low_bin(uh, trim) - 256, hu = high_bin(uh, trim) - 255;
    int lv = low_bin(vh, trim) - 256, hv = high_bin(vh, trim) - 255;
    if (hu - lu < 40 || hv - lv < 30) goto done;
    refine_edges(rgb, c, s, yellow_selected ? 10 : 40,
                 &lu, &hu, &lv, &hv);
    const float u[4] = {lu, hu, hu, lu};
    const float v[4] = {lv, lv, hv, hv};
    for (int i = 0; i < 4; ++i) {
        box->corners[i][0] = 4.0f * (u[i] * c - v[i] * s);
        box->corners[i][1] = 4.0f * (u[i] * s + v[i] * c);
        if (box->corners[i][0] < -2.0f || box->corners[i][0] > width + 1.0f ||
            box->corners[i][1] < -2.0f || box->corners[i][1] > height + 1.0f)
            goto done;
    }
    if (yellow_selected && fit_yellow_quad(labels, best_label, box)) {
        ESP_LOGI(TAG, "Yellow PCB perspective corners fitted to substrate edges");
    }
    box->foreground_pixels = best_count;
    ESP_LOGI(TAG, "PCB detected: color=%s pixels=%d angle=%.1f bounds=(%d,%d)-(%d,%d)",
             yellow_selected ? "yellow" : "green", best_count,
             (double)box->angle_deg, lu, lv, hu, hv);
    found = true;
done:
    heap_caps_free(queue);
    heap_caps_free(labels);
    heap_caps_free(tmp);
    heap_caps_free(mask);
    heap_caps_free(rgb);
    return found;
}

void pcb_normalize_rgb(const uint8_t *source, int source_width, int source_height,
                       const pcb_box_t *box, uint8_t *destination,
                       int destination_width, int destination_height)
{
    const float (*p)[2] = box->corners;
    float horizontal = hypotf(p[1][0] - p[0][0], p[1][1] - p[0][1]);
    float vertical = hypotf(p[3][0] - p[0][0], p[3][1] - p[0][1]);
    /* The supplied demo keeps near-square boards in their detected orientation.
     * Otherwise tiny perspective changes could cause alternating 90-degree
     * rotations between LEARN and CHECK. */
    bool near_square = fmaxf(horizontal, vertical) / fminf(horizontal, vertical) <= 1.16f;
    bool rotate = !near_square && horizontal < vertical;
    const float *tl = rotate ? p[3] : p[0];
    const float *tr = rotate ? p[0] : p[1];
    const float *br = rotate ? p[1] : p[2];
    const float *bl = rotate ? p[2] : p[3];
    float bw = (hypotf(tr[0] - tl[0], tr[1] - tl[1]) +
                hypotf(br[0] - bl[0], br[1] - bl[1])) * 0.5f;
    float bh = (hypotf(bl[0] - tl[0], bl[1] - tl[1]) +
                hypotf(br[0] - tr[0], br[1] - tr[1])) * 0.5f;
    float scale = fminf((destination_width - 32.0f) / bw,
                        (destination_height - 24.0f) / bh);
    int ow = (int)roundf(bw * scale), oh = (int)roundf(bh * scale);
    if (ow > destination_width - 16) ow = destination_width - 16;
    if (oh > destination_height - 16) oh = destination_height - 16;
    int ox = (destination_width - ow) / 2, oy = (destination_height - oh) / 2;
    for (int y = 0; y < destination_height; ++y) {
        for (int x = 0; x < destination_width; ++x) {
            uint8_t *dst = destination + ((size_t)y * destination_width + x) * 3;
            dst[0] = 22; dst[1] = 34; dst[2] = 29;
            if (x < ox || x >= ox + ow || y < oy || y >= oy + oh) continue;
            float u = ((float)(x - ox) + 0.5f) / ow;
            float v = ((float)(y - oy) + 0.5f) / oh;
            float sx = tl[0] * (1-u) * (1-v) + tr[0] * u * (1-v) +
                       br[0] * u * v + bl[0] * (1-u) * v;
            float sy = tl[1] * (1-u) * (1-v) + tr[1] * u * (1-v) +
                       br[1] * u * v + bl[1] * (1-u) * v;
            if (sx < 0 || sy < 0 || sx >= source_width - 1 || sy >= source_height - 1) continue;
            int ix = (int)sx, iy = (int)sy;
            float fx = sx - ix, fy = sy - iy;
            const uint8_t *a = source + ((size_t)iy * source_width + ix) * 3;
            const uint8_t *b = a + (size_t)source_width * 3;
            for (int ch = 0; ch < 3; ++ch) {
                float top = a[ch] * (1-fx) + a[ch+3] * fx;
                float bottom = b[ch] * (1-fx) + b[ch+3] * fx;
                dst[ch] = (uint8_t)roundf(top * (1-fy) + bottom * fy);
            }
        }
        if ((y & 31) == 31) vTaskDelay(1);
    }
}
