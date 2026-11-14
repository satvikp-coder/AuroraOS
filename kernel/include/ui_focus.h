/*
 * kernel/include/ui_focus.h - focus, hit testing and activation rules for native controls (issue #7, stage G5-D).
 * Pure logic: no drawing, no hardware, no allocation. The caller owns the control array and draws it; this module only decides
 * WHICH control a pointer or key refers to and WHEN an activation happens.
 *
 * Rules (all tested on the host):
 *  - Z-order: higher `layer` is on top; at equal layer the later array entry is on top. The topmost VISIBLE control under the
 *    pointer absorbs the event even if it is disabled or not focusable (no click-through to a covered control).
 *  - Modal: ui_modal_push(layer) makes every control with layer < that layer unreachable by pointer AND keyboard until ui_modal_pop().
 *  - Focus: Tab/Down/Right = next, Shift+Tab/Up/Left = previous, wrapping, skipping invisible/disabled/non-focusable/unreachable controls.
 *  - Activation: Enter or Space on the focused control, or a pointer press AND release on the same enabled control.
 *  - Escape is reported to the caller (UI_ESCAPE); the caller decides what it means (e.g. close the modal).
 *  - Closing a modal restores the focus the opener had, if that control is still focusable; otherwise the first focusable one.
 * Nothing here grants authority: an activation is a UI event, never permission for a privileged action.
 */
#ifndef AURORA_UI_FOCUS_H
#define AURORA_UI_FOCUS_H
#include <stdint.h>
#include <input.h>

#define UI_VISIBLE 1u
#define UI_ENABLED 2u
#define UI_FOCUSABLE 4u

struct ui_ctrl {
    int32_t x, y, w, h;
    int32_t radius;                    /* corner radius for hit testing; 0 = rectangle */
    uint32_t id;                       /* caller's identifier, reported on activation (must be != UI_NONE) */
    uint8_t flags, layer;
};

#define UI_NONE 0xffffffffu            /* no control / no activation */
#define UI_ESCAPE 0xfffffffeu          /* Escape was pressed (returned by ui_key) */
#define UI_MODAL_DEPTH 4

struct ui_ctx {
    struct ui_ctrl *c;
    uint32_t n;
    int32_t focus;                     /* index, or -1 */
    int32_t pressed;                   /* index pressed by the pointer, or -1 */
    uint32_t modal_depth;
    uint8_t modal_layer[UI_MODAL_DEPTH];
    int32_t restore[UI_MODAL_DEPTH];
};

void ui_init(struct ui_ctx *u, struct ui_ctrl *c, uint32_t n);
/* Index of the topmost visible reachable control containing the pixel, or -1. */
int32_t ui_hit(const struct ui_ctx *u, int32_t x, int32_t y);
/* Move focus; returns the new focused index or -1. Focus never lands on an unfocusable control. */
int32_t ui_focus_next(struct ui_ctx *u, int forward);
int32_t ui_focus_set(struct ui_ctx *u, int32_t index);          /* 0 = refused (not focusable/reachable) */
/* Keyboard: feed an IN_KEY_DOWN event. Returns the id of an activated control, UI_ESCAPE, or UI_NONE. */
uint32_t ui_key(struct ui_ctx *u, const struct input_event *e);
/* Pointer: feed IN_PTR_DOWN / IN_PTR_UP. Up returns the id activated by a complete click, else UI_NONE. */
void     ui_pointer_down(struct ui_ctx *u, int32_t x, int32_t y);
uint32_t ui_pointer_up(struct ui_ctx *u, int32_t x, int32_t y);
/* Modal handling. Push fails (returns 0) when the stack is full or the layer does not exceed the current modal layer. */
int  ui_modal_push(struct ui_ctx *u, uint8_t layer);
int  ui_modal_pop(struct ui_ctx *u);
/* Call after the caller changes control flags/geometry: drops focus/press that are no longer valid. */
void ui_revalidate(struct ui_ctx *u);

#endif
