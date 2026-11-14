/* kernel/ui/ui_proto.c - the EL0 UI protocol v1: decoding, validation and the compositor's model of client surfaces. See kernel/include/ui_proto.h.
 * Pure C, no libc, no device, no scheduler. */
#include <ui_proto.h>

const uint16_t UIP_SIZE_W[UIP_SIZE_CLASSES] = { 256, 480, 720 };
const uint16_t UIP_SIZE_H[UIP_SIZE_CLASSES] = { 160, 320, 420 };

/* ---- little-endian helpers ---- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static int printable(const uint8_t *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) if (s[i] < 0x20 || s[i] > 0x7e) return 0;
    return 1;
}

/* ---- decoding ---- */
int uip_decode(const uint8_t *b, uint32_t len, struct uip_msg *out)
{
    if (!b || !out) return UIP_EBADARG;
    if (len == 0) return UIP_ETRUNC;
    if (len > UIP_MSG_MAX) return UIP_EBADLEN;
    if (b[0] != UIP_VERSION) return UIP_EVERSION;
    if (len < UIP_HDR) return UIP_ETRUNC;
    struct uip_msg m;
    uint8_t *z = (uint8_t *)&m;
    for (uint32_t i = 0; i < sizeof m; i++) z[i] = 0;
    m.type = b[1]; m.seq = rd16(b + 2); m.handle = rd32(b + 4);
    switch (m.type) {
    case UIP_CREATE_SURFACE: {
        if (len < 10) return UIP_ETRUNC;
        uint32_t tl = b[9];
        if (tl > UIP_TITLE_MAX) return UIP_EBADLEN;
        if (len < 10 + tl) return UIP_ETRUNC;
        if (len > 10 + tl) return UIP_EBADLEN;
        if (m.handle != 0 || b[8] >= UIP_SIZE_CLASSES) return UIP_EBADARG;
        if (!printable(b + 10, tl)) return UIP_EBADTEXT;
        m.a = b[8]; m.tlen = (uint8_t)tl;
        for (uint32_t i = 0; i < tl; i++) m.text[i] = (char)b[10 + i];
        break;
    }
    case UIP_SET_TEXT: {
        if (len < 11) return UIP_ETRUNC;
        uint32_t tl = b[10];
        if (tl > UIP_TEXT_MAX) return UIP_EBADLEN;
        if (len < 11 + tl) return UIP_ETRUNC;
        if (len > 11 + tl) return UIP_EBADLEN;
        if (b[8] >= UIP_WIDGETS || b[9] >= UIP_STYLES) return UIP_EBADARG;
        if (!printable(b + 11, tl)) return UIP_EBADTEXT;
        m.a = b[8]; m.b = b[9]; m.tlen = (uint8_t)tl;
        for (uint32_t i = 0; i < tl; i++) m.text[i] = (char)b[11 + i];
        break;
    }
    case UIP_SET_RECT:
        if (len < 18) return UIP_ETRUNC;
        if (len > 18) return UIP_EBADLEN;
        if (b[8] >= UIP_WIDGETS || b[9] >= UIP_ROLES) return UIP_EBADARG;
        m.a = b[8]; m.b = b[9];
        m.x = (int16_t)rd16(b + 10); m.y = (int16_t)rd16(b + 12); m.w = rd16(b + 14); m.h = rd16(b + 16);
        if (m.w == 0 || m.h == 0) return UIP_EBADARG;             /* a zero-size rectangle is never valid */
        break;
    case UIP_COMMIT: case UIP_DESTROY_SURFACE: case UIP_REQUEST_FOCUS:
        if (len > UIP_HDR) return UIP_EBADLEN;
        break;
    default: return UIP_ETYPE;                                    /* includes the compositor-to-client types: a client cannot send those */
    }
    *out = m;
    return UIP_OK;
}

/* ---- encoders ---- */
static uint32_t hdr(uint8_t *b, uint32_t cap, uint32_t total, uint8_t type, uint16_t seq, uint32_t handle)
{
    if (!b || cap < total || total > UIP_MSG_MAX) return 0;
    b[0] = UIP_VERSION; b[1] = type; wr16(b + 2, seq); wr32(b + 4, handle);
    return total;
}
uint32_t uip_encode_status(uint8_t *b, uint32_t cap, uint16_t seq, uint32_t handle, int16_t code, uint8_t req_type)
{
    if (!hdr(b, cap, 11, UIP_STATUS, seq, handle)) return 0;
    wr16(b + 8, (uint16_t)code); b[10] = req_type;
    return 11;
}
uint32_t uip_encode_key(uint8_t *b, uint32_t cap, uint16_t seq, uint32_t handle, uint16_t code, uint16_t mods, uint8_t down, uint8_t repeat)
{
    if (!hdr(b, cap, 14, UIP_EVENT_KEY, seq, handle)) return 0;
    wr16(b + 8, code); wr16(b + 10, mods); b[12] = down; b[13] = repeat;
    return 14;
}
uint32_t uip_encode_pointer(uint8_t *b, uint32_t cap, uint16_t seq, uint32_t handle, uint8_t kind, uint8_t buttons, int16_t x, int16_t y)
{
    if (!hdr(b, cap, 14, UIP_EVENT_POINTER, seq, handle)) return 0;
    b[8] = kind; b[9] = buttons; wr16(b + 10, (uint16_t)x); wr16(b + 12, (uint16_t)y);
    return 14;
}
uint32_t uip_encode_close(uint8_t *b, uint32_t cap, uint16_t seq, uint32_t handle) { return hdr(b, cap, UIP_HDR, UIP_CLOSE_REQUEST, seq, handle); }
uint32_t uip_encode_simple(uint8_t *b, uint32_t cap, uint8_t type, uint16_t seq, uint32_t handle) { return hdr(b, cap, UIP_HDR, type, seq, handle); }

static uint32_t cstr_len(const char *s, uint32_t max) { uint32_t n = 0; while (s && s[n] && n < max + 1) n++; return n; }

uint32_t uip_encode_create(uint8_t *b, uint32_t cap, uint16_t seq, uint8_t size_class, const char *title)
{
    uint32_t tl = cstr_len(title, UIP_TITLE_MAX);
    if (tl > UIP_TITLE_MAX || !hdr(b, cap, 10 + tl, UIP_CREATE_SURFACE, seq, 0)) return 0;
    b[8] = size_class; b[9] = (uint8_t)tl;
    for (uint32_t i = 0; i < tl; i++) b[10 + i] = (uint8_t)title[i];
    return 10 + tl;
}
uint32_t uip_encode_set_text(uint8_t *b, uint32_t cap, uint16_t seq, uint32_t handle, uint8_t widget, uint8_t style, const char *text)
{
    uint32_t tl = cstr_len(text, UIP_TEXT_MAX);
    if (tl > UIP_TEXT_MAX || !hdr(b, cap, 11 + tl, UIP_SET_TEXT, seq, handle)) return 0;
    b[8] = widget; b[9] = style; b[10] = (uint8_t)tl;
    for (uint32_t i = 0; i < tl; i++) b[11 + i] = (uint8_t)text[i];
    return 11 + tl;
}
uint32_t uip_encode_set_rect(uint8_t *b, uint32_t cap, uint16_t seq, uint32_t handle, uint8_t widget, uint8_t role, int16_t x, int16_t y, uint16_t w, uint16_t h)
{
    if (!hdr(b, cap, 18, UIP_SET_RECT, seq, handle)) return 0;
    b[8] = widget; b[9] = role; wr16(b + 10, (uint16_t)x); wr16(b + 12, (uint16_t)y); wr16(b + 14, w); wr16(b + 16, h);
    return 18;
}

/* ---- the model ---- */
static void zero(void *p, uint32_t n) { uint8_t *q = p; for (uint32_t i = 0; i < n; i++) q[i] = 0; }

void uip_init(struct uip_state *st, int16_t rx, int16_t ry, int16_t rw, int16_t rh)
{
    zero(st, sizeof *st);
    st->focus = -1;
    st->rx = rx; st->ry = ry; st->rw = rw; st->rh = rh;
}

int uip_slot_of(uint32_t handle)
{
    if ((handle >> 24) != 0 || (handle & 0xff) >= UIP_MAX_SURFACES || (handle >> 8) == 0) return -1;     /* serial is 16 bits and never 0 */
    return (int)(handle & 0xff);
}

uint32_t uip_handle_of(const struct uip_state *st, int slot)
{
    if (!st || slot < 0 || slot >= UIP_MAX_SURFACES || !st->s[slot].live) return 0;
    return ((uint32_t)st->s[slot].serial << 8) | (uint32_t)slot;
}

const struct uip_surface *uip_surface(const struct uip_state *st, int slot)
{
    return (st && slot >= 0 && slot < UIP_MAX_SURFACES && st->s[slot].live) ? &st->s[slot] : 0;
}

static void z_remove(struct uip_state *st, int slot)
{
    uint8_t j = 0;
    for (uint8_t i = 0; i < st->nz; i++) if (st->z[i] != slot) st->z[j++] = st->z[i];
    st->nz = j;
}
static void z_push(struct uip_state *st, int slot) { z_remove(st, slot); if (st->nz < UIP_MAX_SURFACES) st->z[st->nz++] = (int8_t)slot; }

static void destroy_slot(struct uip_state *st, int slot)
{
    struct uip_surface *s = &st->s[slot];
    if (!s->live) return;
    uint8_t pid = s->owner_pid;
    uint16_t serial = s->serial;                        /* the serial survives: the next user of the slot gets a different handle */
    zero(s, sizeof *s);
    s->serial = serial;
    if (pid >= 1 && pid <= UIP_MAX_CLIENTS && st->c[pid - 1].nsurf) st->c[pid - 1].nsurf--;
    if (st->focus == slot) st->focus = -1;
    z_remove(st, slot);
}

static void drop_instance(struct uip_state *st, uint8_t pid, uint64_t gen)
{
    for (int i = 0; i < UIP_MAX_SURFACES; i++)
        if (st->s[i].live && st->s[i].owner_pid == pid && st->s[i].owner_gen == gen) destroy_slot(st, i);
}

void uip_client_gone(struct uip_state *st, uint8_t pid, uint64_t gen)
{
    if (pid < 1 || pid > UIP_MAX_CLIENTS) return;
    struct uip_client *c = &st->c[pid - 1];
    if (!c->used || c->gen != gen) return;              /* a stale notification must not touch a newer instance */
    drop_instance(st, pid, gen);
    zero(c, sizeof *c);
}

void uip_reply_consumed(struct uip_state *st, uint8_t pid, uint64_t gen)
{
    if (pid < 1 || pid > UIP_MAX_CLIENTS) return;
    struct uip_client *c = &st->c[pid - 1];
    if (c->used && c->gen == gen && c->outstanding) c->outstanding--;
}

static struct uip_surface *find(struct uip_state *st, const struct uip_caller *w, uint32_t handle)
{
    int slot = uip_slot_of(handle);
    if (slot < 0) return 0;
    struct uip_surface *s = &st->s[slot];
    if (!s->live || s->serial != (uint16_t)(handle >> 8)) return 0;
    if (s->owner_pid != w->pid || s->owner_gen != w->gen || s->owner_principal != w->principal) return 0;
    return s;
}

static void copy_text(char *d, const char *s, uint8_t n) { for (uint8_t i = 0; i < n; i++) d[i] = s[i]; d[n] = 0; }

static int apply(struct uip_state *st, struct uip_client *c, const struct uip_caller *w, const struct uip_msg *m, uint32_t *handle_out)
{
    *handle_out = m->handle;
    if (m->type == UIP_CREATE_SURFACE) {
        if (c->have_seq && c->last_seq == m->seq) return UIP_EDUP;      /* a replayed CREATE creates nothing */
        c->have_seq = 1; c->last_seq = m->seq;
        if (c->nsurf >= UIP_PER_CLIENT) return UIP_EQUOTA;
        int slot = -1;
        for (int i = 0; i < UIP_MAX_SURFACES && slot < 0; i++) if (!st->s[i].live) slot = i;
        if (slot < 0) return UIP_EQUOTA;
        struct uip_surface *s = &st->s[slot];
        uint16_t serial = (uint16_t)(s->serial + 1);
        if (serial == 0) serial = 1;
        zero(s, sizeof *s);
        s->live = 1; s->serial = serial; s->owner_pid = w->pid; s->owner_gen = w->gen; s->owner_principal = w->principal;
        s->size_class = m->a; s->tlen = m->tlen; copy_text(s->title, m->text, m->tlen);
        c->nsurf++;
        z_push(st, slot);
        *handle_out = ((uint32_t)serial << 8) | (uint32_t)slot;
        return UIP_OK;
    }
    struct uip_surface *s = find(st, w, m->handle);
    if (!s) return UIP_ESTALE;                                           /* dead, stale or someone else's: the same answer */
    int slot = (int)(s - st->s);
    switch (m->type) {
    case UIP_SET_TEXT: {
        struct uip_widget *g = &s->pend[m->a];
        g->has_text = m->tlen ? 1 : 0; g->style = m->b; g->tlen = m->tlen; copy_text(g->text, m->text, m->tlen);
        return UIP_OK;
    }
    case UIP_SET_RECT: {
        int32_t sw = UIP_SIZE_W[s->size_class], sh = UIP_SIZE_H[s->size_class];
        if (m->x < 0 || m->y < 0 || (int32_t)m->x + (int32_t)m->w > sw || (int32_t)m->y + (int32_t)m->h > sh) return UIP_EBADARG;   /* rejected, never clamped */
        struct uip_widget *g = &s->pend[m->a];
        g->has_rect = 1; g->role = m->b; g->x = m->x; g->y = m->y; g->w = m->w; g->h = m->h;
        return UIP_OK;
    }
    case UIP_COMMIT:
        for (int i = 0; i < UIP_WIDGETS; i++) s->vis[i] = s->pend[i];    /* all at once: the visible model changes here and nowhere else */
        s->commits++;
        return UIP_OK;
    case UIP_DESTROY_SURFACE:
        destroy_slot(st, slot);
        return UIP_OK;
    case UIP_REQUEST_FOCUS:
        s->focus_wished = 1;                                             /* advisory: the compositor alone moves focus */
        return UIP_OK;
    default: return UIP_ETYPE;
    }
}

int uip_handle(struct uip_state *st, const struct uip_caller *who, const uint8_t *msg, uint32_t len, uint8_t *reply, uint32_t reply_cap, uint32_t *reply_len)
{
    if (reply_len) *reply_len = 0;
    if (!st || !who || (len && !msg)) return UIP_EBADARG;
    if (who->pid < 1 || who->pid > UIP_MAX_CLIENTS) return UIP_EPERM;           /* not a valid process: nobody to answer */
    uint16_t seq = len >= 4 ? rd16(msg + 2) : 0;
    uint8_t rtype = len >= 2 ? msg[1] : 0;
    int code;
    uint32_t handle = 0;
    struct uip_client *c = &st->c[who->pid - 1];
    if (!who->can_ui) {
        code = UIP_EPERM;                                                    /* no capability: no record is created and nothing is counted */
    } else {
        if (c->used && (c->gen != who->gen || c->principal != who->principal)) {   /* a NEW instance in this slot: the old one's surfaces go first */
            drop_instance(st, who->pid, c->gen);
            zero(c, sizeof *c);
        }
        if (!c->used) { c->used = 1; c->gen = who->gen; c->principal = who->principal; }
        if (c->outstanding >= UIP_MAX_OUTSTANDING) return UIP_EBUSY;         /* dropped: no reply, nothing changes */
        c->outstanding++;                                                     /* a reply follows, and stays outstanding until it is consumed */
        struct uip_msg m;
        code = uip_decode(msg, len, &m);
        if (code == UIP_OK) { code = apply(st, c, who, &m, &handle); seq = m.seq; rtype = m.type; }
    }
    if (reply && reply_len) *reply_len = uip_encode_status(reply, reply_cap, seq, handle, (int16_t)code, rtype);
    return code;
}

int uip_place(struct uip_state *st, int slot, int16_t x, int16_t y)
{
    if (!uip_surface(st, slot)) return UIP_ESTALE;
    struct uip_surface *s = &st->s[slot];
    int32_t w = UIP_SIZE_W[s->size_class], h = UIP_SIZE_H[s->size_class];
    if ((int32_t)x < st->rx || (int32_t)y < st->ry || (int32_t)x + w > (int32_t)st->rx + st->rw || (int32_t)y + h > (int32_t)st->ry + st->rh) return UIP_EBADARG;
    s->sx = x; s->sy = y; s->placed = 1;
    return UIP_OK;
}

int uip_set_focus(struct uip_state *st, int slot)
{
    if (slot == -1) { st->focus = -1; return UIP_OK; }
    if (!uip_surface(st, slot)) return UIP_ESTALE;
    st->focus = (int8_t)slot; st->s[slot].focus_wished = 0;
    z_push(st, slot);
    return UIP_OK;
}

/* ---- routing ---- */
uint32_t uip_route_key(struct uip_state *st, uint16_t code, uint16_t mods, uint8_t down, uint8_t repeat, struct uip_target *to, uint8_t *buf, uint32_t cap)
{
    if (st->focus < 0 || !st->s[st->focus].live) return 0;
    struct uip_surface *s = &st->s[st->focus];
    uint32_t n = uip_encode_key(buf, cap, (uint16_t)++st->evseq, uip_handle_of(st, st->focus), code, mods, down, repeat);
    if (n && to) { to->pid = s->owner_pid; to->gen = s->owner_gen; }
    return n;
}

uint32_t uip_route_pointer(struct uip_state *st, int x, int y, uint8_t kind, uint8_t buttons, struct uip_target *to, uint8_t *buf, uint32_t cap)
{
    for (int k = (int)st->nz - 1; k >= 0; k--) {                        /* topmost first */
        int slot = st->z[k];
        struct uip_surface *s = &st->s[slot];
        if (!s->live || !s->placed) continue;
        int w = UIP_SIZE_W[s->size_class], h = UIP_SIZE_H[s->size_class];
        if (x < s->sx || y < s->sy || x >= s->sx + w || y >= s->sy + h) continue;
        uint32_t n = uip_encode_pointer(buf, cap, (uint16_t)++st->evseq, uip_handle_of(st, slot), kind, buttons, (int16_t)(x - s->sx), (int16_t)(y - s->sy));
        if (n && to) { to->pid = s->owner_pid; to->gen = s->owner_gen; }
        return n;
    }
    return 0;
}

uint32_t uip_route_close(struct uip_state *st, struct uip_target *to, uint8_t *buf, uint32_t cap)
{
    if (st->focus < 0 || !st->s[st->focus].live) return 0;
    struct uip_surface *s = &st->s[st->focus];
    uint32_t n = uip_encode_close(buf, cap, (uint16_t)++st->evseq, uip_handle_of(st, st->focus));
    if (n && to) { to->pid = s->owner_pid; to->gen = s->owner_gen; }
    return n;
}

/* ---- digest and invariants ---- */
static uint32_t fnv(uint32_t h, uint32_t v) { for (int i = 0; i < 4; i++) { h ^= (v >> (8 * i)) & 0xff; h *= 16777619u; } return h; }
static uint32_t fnv_widget(uint32_t h, const struct uip_widget *g)
{
    h = fnv(h, g->has_text | (g->has_rect << 8) | ((uint32_t)g->style << 16) | ((uint32_t)g->role << 24));
    h = fnv(h, (uint32_t)(uint16_t)g->x | ((uint32_t)(uint16_t)g->y << 16));
    h = fnv(h, (uint32_t)g->w | ((uint32_t)g->h << 16));
    h = fnv(h, g->tlen);
    for (uint32_t i = 0; i < g->tlen; i++) h = fnv(h, (uint8_t)g->text[i]);
    return h;
}

uint32_t uip_digest(const struct uip_state *st)
{
    uint32_t h = 2166136261u;
    h = fnv(h, (uint32_t)(int32_t)st->focus); h = fnv(h, st->nz);
    for (uint8_t i = 0; i < st->nz; i++) h = fnv(h, (uint8_t)st->z[i]);
    for (int i = 0; i < UIP_MAX_SURFACES; i++) {
        const struct uip_surface *s = &st->s[i];
        h = fnv(h, s->live | ((uint32_t)s->serial << 8));
        if (!s->live) continue;
        h = fnv(h, s->owner_pid); h = fnv(h, (uint32_t)s->owner_gen); h = fnv(h, (uint32_t)(s->owner_gen >> 32)); h = fnv(h, s->owner_principal);
        h = fnv(h, s->size_class | ((uint32_t)s->placed << 8)); h = fnv(h, (uint32_t)(uint16_t)s->sx | ((uint32_t)(uint16_t)s->sy << 16));
        h = fnv(h, s->tlen); for (uint32_t k = 0; k < s->tlen; k++) h = fnv(h, (uint8_t)s->title[k]);
        h = fnv(h, s->commits);
        for (int k = 0; k < UIP_WIDGETS; k++) { h = fnv_widget(h, &s->vis[k]); h = fnv_widget(h, &s->pend[k]); }
    }
    return h;
}

static int widget_ok(const struct uip_widget *g, const struct uip_surface *s)
{
    if (g->tlen > UIP_TEXT_MAX || g->text[g->tlen] != 0) return 0;
    for (uint32_t i = 0; i < g->tlen; i++) if ((uint8_t)g->text[i] < 0x20 || (uint8_t)g->text[i] > 0x7e) return 0;
    if (g->has_rect) {
        if (g->w == 0 || g->h == 0 || g->x < 0 || g->y < 0) return 0;
        if ((int32_t)g->x + g->w > UIP_SIZE_W[s->size_class] || (int32_t)g->y + g->h > UIP_SIZE_H[s->size_class]) return 0;
    }
    return 1;
}

int uip_check_invariants(const struct uip_state *st)
{
    uint8_t count[UIP_MAX_CLIENTS] = { 0 };
    uint8_t seen[UIP_MAX_SURFACES] = { 0 };
    for (int i = 0; i < UIP_MAX_SURFACES; i++) {
        const struct uip_surface *s = &st->s[i];
        if (!s->live) continue;
        if (s->serial == 0 || s->owner_pid < 1 || s->owner_pid > UIP_MAX_CLIENTS || s->size_class >= UIP_SIZE_CLASSES || s->tlen > UIP_TITLE_MAX || s->title[s->tlen] != 0) return 0;
        const struct uip_client *c = &st->c[s->owner_pid - 1];
        if (!c->used || c->gen != s->owner_gen) return 0;                          /* every live surface belongs to the registered instance */
        count[s->owner_pid - 1]++;
        for (int k = 0; k < UIP_WIDGETS; k++) if (!widget_ok(&s->vis[k], s) || !widget_ok(&s->pend[k], s)) return 0;
    }
    for (int p = 0; p < UIP_MAX_CLIENTS; p++) if (st->c[p].nsurf != count[p] || count[p] > UIP_PER_CLIENT || st->c[p].outstanding > UIP_MAX_OUTSTANDING) return 0;
    if (st->nz > UIP_MAX_SURFACES) return 0;
    for (uint8_t i = 0; i < st->nz; i++) {
        int sl = st->z[i];
        if (sl < 0 || sl >= UIP_MAX_SURFACES || !st->s[sl].live || seen[sl]) return 0;
        seen[sl] = 1;
    }
    for (int i = 0; i < UIP_MAX_SURFACES; i++) if (st->s[i].live != seen[i]) return 0;      /* z holds exactly the live slots, once each */
    if (st->focus != -1 && (st->focus < 0 || st->focus >= UIP_MAX_SURFACES || !st->s[st->focus].live)) return 0;
    return 1;
}
