/*
 * kernel/ui/gfx_bench.c - TEST-BUILD ONLY (AURORA_BENCH=1): CPU drawing-time baseline for the native renderer.
 *
 * Time source: the ARM generic counter (CNTPCT_EL0, read through timer_count()) at CNTFRQ_EL0 Hz (timer_freq()). Ticks are converted
 * to microseconds with 64-bit integer math: us = ticks * 1,000,000 / freq. Under QEMU/TCG the counter follows the host clock, so the
 * numbers are host-emulation times for this machine configuration, NOT a prediction for real hardware.
 *
 * Method: WARMUP untimed iterations, then SAMPLES timed iterations of each case; results are printed ONCE per case after timing (never
 * per sample), as: bench: <name> median_us=<m> p95_us=<p> min_us=<a> max_us=<b> n=<N>. Median and p95 come from the sorted samples
 * (p95 = sample at index ceil(0.95*N)-1). Timed region = drawing into the surface only (gfx_present is a barrier on ramfb and is timed
 * separately as "present"). No QMP, screendump or serial output happens inside a timed region.
 */
#include <console.h>
#include <gfx.h>
#include <input.h>
#include <timer.h>
#include <ui_scene.h>

#define WARMUP 5
#define SAMPLES 40

static uint64_t us_of(uint64_t ticks) { return ticks * 1000000u / timer_freq(); }

static void sort_u64(uint64_t *a, int n) { for (int i = 1; i < n; i++) { uint64_t v = a[i]; int j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; } }

static void report(const char *name, uint64_t *t, int n)
{
    sort_u64(t, n);
    int p95 = (n * 95 + 99) / 100 - 1;
    console_puts("bench: "); console_puts(name);
    console_puts(" median_us="); console_dec(us_of(t[n / 2]));
    console_puts(" p95_us="); console_dec(us_of(t[p95]));
    console_puts(" min_us="); console_dec(us_of(t[0]));
    console_puts(" max_us="); console_dec(us_of(t[n - 1]));
    console_puts(" n="); console_dec((uint64_t)n); console_puts("\n");
}

typedef void (*case_fn)(const struct gfx_surface *s);

static void run_case(const char *name, const struct gfx_surface *s, case_fn fn)
{
    static uint64_t t[SAMPLES];
    for (int i = 0; i < WARMUP; i++) fn(s);
    for (int i = 0; i < SAMPLES; i++) { uint64_t a = timer_count(); fn(s); t[i] = timer_count() - a; }
    report(name, t, SAMPLES);
}

static void c_clear(const struct gfx_surface *s) { gfx_clear(s, 0x00EEF3F8u); }
static void c_fill(const struct gfx_surface *s) { struct gfx_rect r = { 256, 192, 512, 384 }; gfx_fill_rect(s, r, 0x003478F6u); }
static void c_round(const struct gfx_surface *s) { struct gfx_rect r = { 256, 192, 512, 384 }; gfx_fill_round_rect(s, r, 24, 0x003478F6u); }
static void c_round_border(const struct gfx_surface *s) { struct gfx_rect r = { 256, 192, 512, 384 }; gfx_fill_round_rect(s, r, 24, 0x00FFFFFFu); gfx_stroke_round_rect(s, r, 24, 2, 0x00175CD3u); }
static void c_text(const struct gfx_surface *s)
{
    const struct gfx_font *f = gfx_font_find(400, 16);
    static const char line[] = "AuroraOS Tier 0 Clear: the quick brown fox jumps over 13 lazy dogs";
    for (int i = 0; i < 20; i++) gfx_draw_text(s, f, 20, 20 + i * 24, line, sizeof line - 1, 0x00142136u, 0);
}
static void c_present(const struct gfx_surface *s) { (void)s; gfx_present(0); }

static struct ui_widget W[100];
static struct ui_ctrl C[100];
static char labels[100][12];
static int NW;
static struct ui_ctx FX;

/* Damaged-region experiment: redraw only one widget's 110x56 neighbourhood instead of the whole frame. */
static void c_damage(const struct gfx_surface *s)
{
    struct gfx_rect d = { W[NW / 2].c.x - 6, W[NW / 2].c.y - 6, 110, 56 };
    gfx_fill_rect(s, d, UI_COL_CANVAS);
    ui_scene_draw(s, &W[NW / 2], 1, 0, 0, 0, 0);
}


static void build_scene(int n)
{
    NW = n;
    for (int i = 0; i < n; i++) {
        labels[i][0] = 'I'; labels[i][1] = 't'; labels[i][2] = 'e'; labels[i][3] = 'm'; labels[i][4] = ' ';
        labels[i][5] = (char)('0' + (i / 10) % 10); labels[i][6] = (char)('0' + i % 10); labels[i][7] = 0;
        W[i].c = (struct ui_ctrl){ 16 + (i % 10) * 100, 16 + (i / 10) * 48, 92, 40, 12, (uint32_t)(i + 1), UI_VISIBLE | UI_ENABLED | UI_FOCUSABLE, (uint8_t)(i % 3) };
        W[i].kind = UI_BUTTON; W[i].active = (i % 7) == 0; W[i].text = labels[i]; W[i].fill = UI_COL_SURFACE; W[i].border = UI_COL_MUTED; W[i].border_w = 1; W[i].ink = UI_COL_INK; W[i].font_size = 14;
        C[i] = W[i].c;
    }
    ui_init(&FX, C, (uint32_t)n);
    ui_focus_set(&FX, n / 2);
}
static void c_scene(const struct gfx_surface *s) { gfx_clear(s, UI_COL_CANVAS); ui_scene_draw(s, W, (uint32_t)NW, &FX, 512, 384, 1); }

/* The same pipeline an input event takes in the demo: normalised key event -> focus -> redraw -> present (CPU part only). */
static void c_event(const struct gfx_surface *s)
{
    struct input_event e = { IN_KEY_DOWN, 0, KEY_TAB, 0, 0, 0, 0 };
    (void)ui_key(&FX, &e);
    gfx_clear(s, UI_COL_CANVAS); ui_scene_draw(s, W, (uint32_t)NW, &FX, 512, 384, 1); gfx_present(0);
}

void gfx_bench_run(void)
{
    const struct gfx_surface *s = gfx_get_surface();
    if (!s) { console_puts("bench: no display\n"); return; }
    console_puts("bench: counter_hz="); console_dec(timer_freq()); console_puts(" warmup="); console_dec(WARMUP); console_puts(" samples="); console_dec(SAMPLES); console_puts("\n");
    console_puts("bench: framebuffer_bytes="); console_dec(s->size_bytes); console_puts(" scene_state_bytes_100="); console_dec(sizeof W + sizeof C + sizeof labels + sizeof FX); console_puts("\n");
    run_case("clear_1024x768", s, c_clear);
    run_case("fill_rect_512x384", s, c_fill);
    run_case("round_rect_512x384_r24", s, c_round);
    run_case("round_rect_border_512x384", s, c_round_border);
    run_case("text_20_lines_x_66_chars", s, c_text);
    run_case("present", s, c_present);
    build_scene(10);  run_case("scene_10_controls_full_frame", s, c_scene);
    build_scene(50);  run_case("scene_50_controls_full_frame", s, c_scene);
    build_scene(100); run_case("scene_100_controls_full_frame", s, c_scene);
    run_case("damage_one_widget_100_scene", s, c_damage);
    run_case("event_to_present_100_controls", s, c_event);
    build_scene(10);  run_case("event_to_present_10_controls", s, c_event);
    console_puts("bench: done\n");
}
