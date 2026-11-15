/* kernel/ui/aura_cmd.c - Aura Quick Commands parser and intent validator. See kernel/include/aura_cmd.h. Pure C, no libc, bounded everywhere. */
#include <aura_cmd.h>

#define MAXT 12
#define TOKLEN 16

struct toks { char t[MAXT][TOKLEN + 1]; int n; };

static int eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void put(struct aura_intent *o, const char *s)
{
    int i = 0;
    for (; s[i] && i < AURA_PREVIEW_MAX - 1; i++) o->preview[i] = s[i];
    o->preview[i] = 0;
}
static void put_num(char *d, int cap, const char *pre, int v, const char *post)
{
    int n = 0;
    for (; pre[n] && n < cap - 1; n++) d[n] = pre[n];
    char t[12]; int k = 0; unsigned u = (unsigned)(v < 0 ? -v : v);
    if (v < 0 && n < cap - 1) d[n++] = '-';
    do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u && k < 11);
    while (k && n < cap - 1) d[n++] = t[--k];
    for (int i = 0; post[i] && n < cap - 1; i++) d[n++] = post[i];
    d[n] = 0;
}

static int fin(struct aura_intent *o, int kind, int status, int reason, int risk, int target, int akind, int arg, const char *preview)
{
    o->version = AURA_INTENT_VERSION; o->kind = (uint8_t)kind; o->status = (uint8_t)status; o->reason = (uint8_t)reason; o->risk = (uint8_t)risk;
    o->target = (uint8_t)target; o->arg_kind = (uint8_t)akind; o->arg = arg;
    put(o, preview);
    return status;
}

/* ---- tokenizer: lower-case, punctuation dropped, ';' and '&' become the word "and"; returns an AR_* reason, or 0 ---- */
static int tokenize(const char *text, size_t len, struct toks *tk)
{
    tk->n = 0;
    if (!text || len == 0) return AR_EMPTY;
    if (len > AURA_CMD_MAX) return AR_TOO_LONG;
    for (size_t i = 0; i < len; i++) if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e) return AR_BAD_ENCODING;
    int cur = 0;                                           /* length of the token being built, 0 = none open */
    for (size_t i = 0; i <= len; i++) {
        char c = i < len ? text[i] : ' ';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        int sep = c == ' ', conj = c == ';' || c == '&';
        int drop = c == '.' || c == ',' || c == '!' || c == '?' || c == ':' || c == '\'' || c == '"' || c == '(' || c == ')';
        if (sep || conj || drop) {
            if (sep || conj) {
                if (cur) { tk->t[tk->n][cur] = 0; tk->n++; cur = 0; }
                if (conj) { if (tk->n >= MAXT) return AR_TOO_LONG; tk->t[tk->n][0] = 'a'; tk->t[tk->n][1] = 'n'; tk->t[tk->n][2] = 'd'; tk->t[tk->n][3] = 0; tk->n++; }
            }
            continue;                                      /* dropped punctuation joins neighbouring characters: "don't" -> "dont" */
        }
        if (!cur && tk->n >= MAXT) return AR_TOO_LONG;
        if (cur >= TOKLEN) return AR_TOO_LONG;
        tk->t[tk->n][cur++] = c;
    }
    if (tk->n == 0) return AR_EMPTY;
    return 0;
}

static int has(const struct toks *tk, int lo, int hi, const char *w) { for (int i = lo; i < hi; i++) if (eq(tk->t[i], w)) return 1; return 0; }
static int has_any(const struct toks *tk, int lo, int hi, const char *const *ws) { for (int k = 0; ws[k]; k++) if (has(tk, lo, hi, ws[k])) return 1; return 0; }

/* A phrase ("go to") at position pos: returns the number of tokens it takes, or 0. Never reads past hi. */
static int ph(const struct toks *tk, int pos, int hi, const char *phrase)
{
    int p = pos;
    while (*phrase) {
        char w[TOKLEN + 1]; int n = 0;
        while (*phrase && *phrase != ' ' && n < TOKLEN) w[n++] = *phrase++;
        w[n] = 0;
        while (*phrase == ' ') phrase++;
        if (p >= hi || !eq(tk->t[p], w)) return 0;
        p++;
    }
    return p - pos;
}
static int first_of(const struct toks *tk, int pos, int hi, const char *const *phrases)
{
    int best = 0;
    for (int k = 0; phrases[k]; k++) { int n = ph(tk, pos, hi, phrases[k]); if (n > best) best = n; }
    return best;
}

/* ---- word lists ---- */
static const char *const INJECT_WORDS[] = { "ignore", "disregard", "jailbreak", "override", "sudo", "root", "admin", "administrator", "password", "bypass", "chmod", "syscall", "svc", "rm", 0 };
static const char *const DESTRUCTIVE[] = { "delete", "remove", "erase", "wipe", "format", "uninstall", "destroy", "shred", "install", "download", 0 };
static const char *const MESSAGING[] = { "send", "message", "messages", "sms", "email", "mail", "call", "dial", "whatsapp", "tweet", "post", "share", 0 };
static const char *const TIMERS[] = { "timer", "stopwatch", "alarm", "remind", "reminder", "countdown", 0 };
static const char *const HARDWARE[] = { "brightness", "brighter", "dim", "dimmer", "volume", "mute", "wifi", "bluetooth", "airplane", "battery", "flashlight", "torch", 0 };
static const char *const NOTES[] = { "note", "notes", "notebook", "memo", 0 };
static const char *const CONJ[] = { "and", "then", "also", "plus", "after", 0 };
static const char *const LEAD[] = { "please", "hey aura", "hi aura", "aura", "ok", "okay", "can you", "could you", "would you", "will you", "i want to", "i would like to", "id like to", "i need to", "lets", 0 };
static const char *const TRAIL[] = { "please", "now", "thanks", "thank you", 0 };
static const char *const OPEN_VERBS[] = { "open", "go to", "goto", "show me", "show", "launch", "take me to", "switch to", "navigate to", "view", 0 };
static const char *const SET_VERBS[] = { "set", "change", "switch", "make", "turn", "use", "select", "adjust", 0 };

static const char *const SCREEN_NAME[ASC_COUNT] = { "Home", "Spaces", "Index", "Settings", "Pages" };
static const char *const SPACE_NAME[4] = { "Study", "Work", "Projects", "Personal" };
static const char *const THEME_NAME[3] = { "Auto", "Light", "Dark" };

static int screen_of(const char *w)
{
    if (eq(w, "home") || eq(w, "start")) return ASC_HOME;
    if (eq(w, "spaces")) return ASC_SPACES;
    if (eq(w, "index") || eq(w, "search") || eq(w, "launcher")) return ASC_INDEX;
    if (eq(w, "settings") || eq(w, "preferences") || eq(w, "options")) return ASC_SETTINGS;
    if (eq(w, "pages") || eq(w, "processes")) return ASC_PAGES;
    return -1;
}
static int space_of(const char *w)
{
    if (eq(w, "study")) return 0;
    if (eq(w, "work")) return 1;
    if (eq(w, "projects")) return 2;
    if (eq(w, "personal")) return 3;
    return -1;
}
static int is_prefix_of_known(const char *w)               /* a non-empty prefix of a known target name, but not a name itself */
{
    static const char *const K[] = { "home", "spaces", "index", "search", "settings", "preferences", "pages", "processes", "study", "work", "projects", "personal", "launcher", "options", 0 };
    int n = slen(w);
    if (n == 0) return 0;
    for (int k = 0; K[k]; k++) {
        int i = 0; while (i < n && K[k][i] == w[i]) i++;
        if (i == n && n < slen(K[k])) return 1;
    }
    return 0;
}

/* ---- the grammars. Each takes the token range [lo, hi) and returns 1 if it produced the final intent in *o, 0 if it does not apply. ---- */
static int open_screen(const struct toks *tk, int lo, int hi, const struct aura_caps *caps, struct aura_intent *o)
{
    int p = lo, n = first_of(tk, p, hi, OPEN_VERBS);
    if (!n) return 0;
    p += n;
    if (p < hi && eq(tk->t[p], "the")) p++;
    if (p >= hi) { fin(o, AK_CLARIFY, AS_CLARIFY, AR_MISSING_ARG, 0, 0, AA_NONE, 0, "Open what?"); return 1; }
    int sc = screen_of(tk->t[p]);
    int sp = space_of(tk->t[p]);
    if (sc < 0 && sp < 0) {
        if (is_prefix_of_known(tk->t[p]) && p + 1 == hi) { fin(o, AK_CLARIFY, AS_CLARIFY, AR_AMBIGUOUS, 0, 0, AA_NONE, 0, "Which one do you mean?"); return 1; }
        return 0;                                          /* an unknown target: the domain scan or UNKNOWN decides */
    }
    p++;
    if (sc >= 0 && p < hi && eq(tk->t[p], "screen")) p++;                          /* "home screen", "settings screen" */
    if (sc == ASC_SETTINGS && p < hi && eq(tk->t[p], "app")) p++;
    if (sp >= 0 && p < hi && eq(tk->t[p], "space")) p++;
    if (p != hi) return 0;                                  /* leftover words: not an exact command */
    if (sp >= 0) { char b[AURA_PREVIEW_MAX]; int i = 0; const char *pre = "Select Space: "; for (; pre[i]; i++) b[i] = pre[i]; for (int k = 0; SPACE_NAME[sp][k]; k++) b[i++] = SPACE_NAME[sp][k]; b[i] = 0;
                   fin(o, AK_SELECT_SPACE, AS_ACTIONABLE, AR_NONE, 0, sp, AA_NONE, 0, b); return 1; }
    char b[AURA_PREVIEW_MAX]; int i = 0; const char *pre = "Open "; for (; pre[i]; i++) b[i] = pre[i]; for (int k = 0; SCREEN_NAME[sc][k]; k++) b[i++] = SCREEN_NAME[sc][k]; b[i] = 0;
    if (sc == ASC_PAGES && !caps->pages) { fin(o, AK_UNSUPPORTED, AS_UNSUPPORTED, AR_NO_PAGES, 0, 0, AA_NONE, 0, "Unsupported: Pages is not available yet"); return 1; }
    fin(o, AK_OPEN_SCREEN, AS_ACTIONABLE, AR_NONE, 0, sc, AA_NONE, 0, b);
    return 1;
}

static int select_space(const struct toks *tk, int lo, int hi, struct aura_intent *o)
{
    int p = lo, n = first_of(tk, p, hi, OPEN_VERBS);
    if (!n) { static const char *const V[] = { "select", "use", "pick", 0 }; n = first_of(tk, p, hi, V); }
    p += n;
    if (p < hi && eq(tk->t[p], "the")) p++;
    if (p < hi && eq(tk->t[p], "space")) p++;
    if (p >= hi) return 0;
    int sp = space_of(tk->t[p]);
    if (sp < 0) return 0;
    p++;
    if (p < hi && eq(tk->t[p], "space")) p++;
    if (p != hi) return 0;
    char b[AURA_PREVIEW_MAX]; int i = 0; const char *pre = "Select Space: "; for (; pre[i]; i++) b[i] = pre[i]; for (int k = 0; SPACE_NAME[sp][k]; k++) b[i++] = SPACE_NAME[sp][k]; b[i] = 0;
    fin(o, AK_SELECT_SPACE, AS_ACTIONABLE, AR_NONE, 0, sp, AA_NONE, 0, b);
    return 1;
}

static int theme(const struct toks *tk, int lo, int hi, struct aura_intent *o)
{
    int has_theme = has(tk, lo, hi, "theme"), dark = has(tk, lo, hi, "dark"), light = has(tk, lo, hi, "light"), autom = has(tk, lo, hi, "auto") || has(tk, lo, hi, "automatic");
    int mode = has(tk, lo, hi, "mode");
    if (!has_theme && !(mode && (dark || light))) return 0;
    if (dark + light + autom > 1) { fin(o, AK_CLARIFY, AS_CLARIFY, AR_AMBIGUOUS, 0, 0, AA_NONE, 0, "Which one do you mean?"); return 1; }
    int p = lo, n = first_of(tk, p, hi, SET_VERBS);
    if (!n) { static const char *const V[] = { "enable", 0 }; n = first_of(tk, p, hi, V); }
    p += n;
    if (p < hi && eq(tk->t[p], "on")) p++;
    if (p < hi && eq(tk->t[p], "the")) p++;
    if (!has_theme && p < hi && eq(tk->t[p], "to")) p++;                           /* "switch to dark mode" */
    int v = -1;
    if (has_theme) {
        if (p < hi && eq(tk->t[p], "theme")) p++; else return 0;
        if (p < hi && (eq(tk->t[p], "to") || eq(tk->t[p], "as"))) p++;
        if (p >= hi) { fin(o, AK_CLARIFY, AS_CLARIFY, AR_MISSING_ARG, 0, 0, AA_NONE, 0, "That command needs a value"); return 1; }
        if (eq(tk->t[p], "dark")) v = 2; else if (eq(tk->t[p], "light")) v = 1; else if (eq(tk->t[p], "auto") || eq(tk->t[p], "automatic")) v = 0; else return 0;
        p++;
        if (p != hi) return 0;
    } else {
        if (p < hi && (eq(tk->t[p], "dark") || eq(tk->t[p], "light"))) { v = eq(tk->t[p], "dark") ? 2 : 1; p++; } else return 0;
        if (p < hi && eq(tk->t[p], "mode")) p++; else return 0;
        if (p != hi) return 0;
    }
    char b[AURA_PREVIEW_MAX]; int i = 0; const char *pre = "Change theme to "; for (; pre[i]; i++) b[i] = pre[i]; for (int k = 0; THEME_NAME[v][k]; k++) b[i++] = THEME_NAME[v][k]; b[i] = 0;
    fin(o, AK_SET_SETTING, AS_NEEDS_APPROVAL, AR_NONE, 1, ASET_THEME, AA_ABS, v, b);
    return 1;
}

static int number(const char *w, int *out)                 /* digits with an optional trailing '%'; returns 1 ok, 0 not a number, -1 too many digits */
{
    int n = slen(w);
    if (n && w[n - 1] == '%') n--;
    if (n == 0) return 0;
    long v = 0;
    for (int i = 0; i < n; i++) { if (w[i] < '0' || w[i] > '9') return 0; if (i >= 3) return -1; v = v * 10 + (w[i] - '0'); }
    *out = (int)v;
    return 1;
}

static int text_size(const struct toks *tk, int lo, int hi, struct aura_intent *o)
{
    int has_text = has(tk, lo, hi, "text") || has(tk, lo, hi, "font"), has_size = has(tk, lo, hi, "size") || has(tk, lo, hi, "scale");
    int bigger = has(tk, lo, hi, "bigger") || has(tk, lo, hi, "larger") || has(tk, lo, hi, "increase"), smaller = has(tk, lo, hi, "smaller") || has(tk, lo, hi, "decrease");
    if (!has_text) return 0;
    if (!has_size && !bigger && !smaller) return 0;
    if (bigger && smaller) { fin(o, AK_CLARIFY, AS_CLARIFY, AR_AMBIGUOUS, 0, 0, AA_NONE, 0, "Which one do you mean?"); return 1; }
    int p = lo;
    p += first_of(tk, p, hi, SET_VERBS);
    { static const char *const V[] = { "increase", "decrease", 0 }; p += first_of(tk, p, hi, V); }
    if (p < hi && eq(tk->t[p], "the")) p++;
    if (p < hi && (eq(tk->t[p], "bigger") || eq(tk->t[p], "larger") || eq(tk->t[p], "smaller"))) {          /* "bigger text" */
        int dir = eq(tk->t[p], "smaller") ? -1 : 1; p++;
        if (p < hi && eq(tk->t[p], "text")) p++; else return 0;
        if (p != hi) return 0;
        fin(o, AK_SET_SETTING, AS_NEEDS_APPROVAL, AR_NONE, 1, ASET_TEXT_SCALE, AA_REL, dir, dir > 0 ? "Make text larger" : "Make text smaller");
        return 1;
    }
    if (p < hi && (eq(tk->t[p], "text") || eq(tk->t[p], "font"))) p++; else return 0;
    if (p < hi && (eq(tk->t[p], "size") || eq(tk->t[p], "scale"))) p++;
    if (p < hi && (eq(tk->t[p], "bigger") || eq(tk->t[p], "larger") || eq(tk->t[p], "smaller"))) {          /* "make text bigger" */
        int dir = eq(tk->t[p], "smaller") ? -1 : 1; p++;
        if (p != hi) return 0;
        fin(o, AK_SET_SETTING, AS_NEEDS_APPROVAL, AR_NONE, 1, ASET_TEXT_SCALE, AA_REL, dir, dir > 0 ? "Make text larger" : "Make text smaller");
        return 1;
    }
    if (p == hi) {                                                    /* "increase the text size" / "decrease the text size" */
        if (bigger || smaller) { int dir = smaller ? -1 : 1; fin(o, AK_SET_SETTING, AS_NEEDS_APPROVAL, AR_NONE, 1, ASET_TEXT_SCALE, AA_REL, dir, dir > 0 ? "Make text larger" : "Make text smaller"); return 1; }
        fin(o, AK_CLARIFY, AS_CLARIFY, AR_MISSING_ARG, 0, 0, AA_NONE, 0, "That command needs a value");
        return 1;
    }
    if (p < hi && eq(tk->t[p], "to")) p++;
    int v = 0, r = p < hi ? number(tk->t[p], &v) : 0;
    if (r == 0) return 0;
    p++;
    if (p < hi && (eq(tk->t[p], "percent") || eq(tk->t[p], "%"))) p++;
    if (p != hi) return 0;
    if (r < 0 || v < 75 || v > 200) { fin(o, AK_SET_SETTING, AS_REJECTED, AR_RANGE, 1, ASET_TEXT_SCALE, AA_NONE, 0, "Text size must be 75 to 200 percent"); return 1; }
    char b[AURA_PREVIEW_MAX]; put_num(b, sizeof b, "Set text size to ", v, "%");
    fin(o, AK_SET_SETTING, AS_NEEDS_APPROVAL, AR_NONE, 1, ASET_TEXT_SCALE, AA_ABS, v, b);
    return 1;
}

/* strip filler words at the start and end; the remaining range is what the grammars see */
static void strip(const struct toks *tk, int *lo, int *hi)
{
    for (int again = 1; again;) {
        again = 0;
        int n = first_of(tk, *lo, *hi, LEAD);
        if (n && *lo + n < *hi) { *lo += n; again = 1; }
        n = 0;
        for (int k = 0; TRAIL[k]; k++) {
            int w = 1; for (int i = 0; TRAIL[k][i]; i++) if (TRAIL[k][i] == ' ') w++;
            if (*hi - w > *lo && ph(tk, *hi - w, *hi, TRAIL[k]) == w && w > n) n = w;
        }
        if (n) { *hi -= n; again = 1; }
    }
}

int aura_parse(const char *text, size_t len, const struct aura_caps *caps_in, struct aura_intent *o)
{
    static const struct aura_caps none = { 0, 0, 0, 0, 0 };
    const struct aura_caps *caps = caps_in ? caps_in : &none;
    struct toks tk;
    fin(o, AK_UNKNOWN, AS_REJECTED, AR_NOT_UNDERSTOOD, 0, 0, AA_NONE, 0, "Not a command I know");
    int e = tokenize(text, len, &tk);
    if (e == AR_EMPTY) return fin(o, AK_CLARIFY, AS_CLARIFY, AR_EMPTY, 0, 0, AA_NONE, 0, "Say or type a command");
    if (e == AR_TOO_LONG) return fin(o, AK_UNKNOWN, AS_REJECTED, AR_TOO_LONG, 0, 0, AA_NONE, 0, "Command too long");
    if (e == AR_BAD_ENCODING) return fin(o, AK_UNKNOWN, AS_REJECTED, AR_BAD_ENCODING, 0, 0, AA_NONE, 0, "Plain ASCII text only");
    int lo = 0, hi = tk.n;
    /* text that tries to instruct the assistant, names an address or a path, is refused as a whole (it may even contain a valid command) */
    for (int i = 0; i < hi; i++) {
        const char *w = tk.t[i];
        int bad = 0;
        for (int k = 0; INJECT_WORDS[k]; k++) if (eq(w, INJECT_WORDS[k])) bad = 1;
        if (w[0] == '0' && w[1] == 'x') bad = 1;
        for (int j = 0; w[j]; j++) if (w[j] == '/' || w[j] == '\\' || w[j] == '$' || w[j] == '`' || w[j] == '|' || w[j] == '<' || w[j] == '>') bad = 1;
        if (i + 1 < hi && ((eq(w, "previous") && (eq(tk.t[i + 1], "instructions") || eq(tk.t[i + 1], "rules") || eq(tk.t[i + 1], "prompt"))) ||
                           (eq(w, "you") && (eq(tk.t[i + 1], "are") || eq(tk.t[i + 1], "must") || eq(tk.t[i + 1], "will"))) ||
                           (eq(w, "act") && eq(tk.t[i + 1], "as")) || (eq(w, "system") && (eq(tk.t[i + 1], "prompt") || eq(tk.t[i + 1], "instructions"))))) bad = 1;
        if (bad) return fin(o, AK_REFUSED, AS_REJECTED, AR_INJECTION, 3, 0, AA_NONE, 0, "Refused: not a command");
    }
    if (has_any(&tk, lo, hi, DESTRUCTIVE)) return fin(o, AK_DESTRUCTIVE, AS_REJECTED, AR_NEEDS_BROKER, 3, 0, AA_NONE, 0, "Not done: needs broker and trusted approval");
    strip(&tk, &lo, &hi);
    if (has_any(&tk, lo, hi, CONJ)) return fin(o, AK_CLARIFY, AS_CLARIFY, AR_MULTIPLE, 0, 0, AA_NONE, 0, "One command at a time");
    if (open_screen(&tk, lo, hi, caps, o)) return o->status;
    if (select_space(&tk, lo, hi, o)) return o->status;
    if (theme(&tk, lo, hi, o)) return o->status;
    if (text_size(&tk, lo, hi, o)) return o->status;
    if (has_any(&tk, lo, hi, MESSAGING)) return fin(o, AK_UNSUPPORTED, AS_UNSUPPORTED, AR_NO_MESSAGING, 3, 0, AA_NONE, 0, "Unsupported: no messaging service");
    if (has_any(&tk, lo, hi, TIMERS)) return fin(o, AK_UNSUPPORTED, AS_UNSUPPORTED, AR_NO_TIMER, 0, 0, AA_NONE, 0, "Unsupported: no timer service");
    if (has_any(&tk, lo, hi, HARDWARE)) return fin(o, AK_UNSUPPORTED, AS_UNSUPPORTED, AR_NO_HARDWARE, 1, 0, AA_NONE, 0, "Unsupported: no driver for that");
    if (has_any(&tk, lo, hi, NOTES)) return fin(o, AK_UNSUPPORTED, AS_UNSUPPORTED, AR_NO_NOTES, 0, 0, AA_NONE, 0, "Unsupported: no Notes app");
    return fin(o, AK_UNKNOWN, AS_REJECTED, AR_NOT_UNDERSTOOD, 0, 0, AA_NONE, 0, "Not a command I know");
}

/* ---- the independent validator: its own canonical previews and its own tables ---- */
static int str_eq_n(const char *a, const char *b) { int i = 0; for (; i < AURA_PREVIEW_MAX; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; } return 0; }

int aura_validate(const struct aura_intent *x, const struct aura_caps *caps_in)
{
    static const struct aura_caps none = { 0, 0, 0, 0, 0 };
    const struct aura_caps *caps = caps_in ? caps_in : &none;
    if (!x || x->version != AURA_INTENT_VERSION || x->kind == AK_NONE || x->kind >= AK_COUNT || x->status >= AS_COUNT || x->reason >= AR_COUNT || x->arg_kind > AA_REL || x->risk > 3) return 0;
    int terminated = 0; for (int i = 0; i < AURA_PREVIEW_MAX; i++) if (x->preview[i] == 0) { terminated = 1; break; }
    if (!terminated) return 0;
    for (int i = 0; x->preview[i]; i++) if ((unsigned char)x->preview[i] < 0x20 || (unsigned char)x->preview[i] > 0x7e) return 0;
    char want[AURA_PREVIEW_MAX];
    switch (x->kind) {
    case AK_OPEN_SCREEN: {
        static const char *const N[] = { "Open Home", "Open Spaces", "Open Index", "Open Settings", "Open Pages" };
        if (x->target >= 5 || x->arg_kind != AA_NONE || x->arg != 0) return 0;
        if (x->target == 4 && !caps->pages) return 0;                       /* opening Pages without a Pages screen is never valid */
        return x->status == AS_ACTIONABLE && x->reason == AR_NONE && x->risk == 0 && str_eq_n(x->preview, N[x->target]);
    }
    case AK_SELECT_SPACE: {
        static const char *const N[] = { "Select Space: Study", "Select Space: Work", "Select Space: Projects", "Select Space: Personal" };
        if (x->target >= 4 || x->arg_kind != AA_NONE || x->arg != 0) return 0;
        return x->status == AS_ACTIONABLE && x->reason == AR_NONE && x->risk == 0 && str_eq_n(x->preview, N[x->target]);
    }
    case AK_SET_SETTING:
        if (x->status == AS_REJECTED) return x->reason == AR_RANGE && x->target == 2 && x->arg_kind == AA_NONE && x->arg == 0 && x->risk == 1 && str_eq_n(x->preview, "Text size must be 75 to 200 percent");
        if (x->status != AS_NEEDS_APPROVAL || x->reason != AR_NONE || x->risk != 1) return 0;                 /* a setting change is never actionable on its own */
        if (x->target == 1) {
            static const char *const N[] = { "Change theme to Auto", "Change theme to Light", "Change theme to Dark" };
            return x->arg_kind == AA_ABS && x->arg >= 0 && x->arg <= 2 && str_eq_n(x->preview, N[x->arg]);
        }
        if (x->target == 2) {
            if (x->arg_kind == AA_REL) return (x->arg == 1 && str_eq_n(x->preview, "Make text larger")) || (x->arg == -1 && str_eq_n(x->preview, "Make text smaller"));
            if (x->arg_kind != AA_ABS || x->arg < 75 || x->arg > 200) return 0;
            put_num(want, sizeof want, "Set text size to ", x->arg, "%");
            return str_eq_n(x->preview, want);
        }
        return 0;
    case AK_UNSUPPORTED: {
        if (x->status != AS_UNSUPPORTED || x->target != 0 || x->arg_kind != AA_NONE || x->arg != 0) return 0;
        switch (x->reason) {
        case AR_NO_TIMER: return x->risk == 0 && str_eq_n(x->preview, "Unsupported: no timer service");
        case AR_NO_HARDWARE: return x->risk == 1 && str_eq_n(x->preview, "Unsupported: no driver for that");
        case AR_NO_NOTES: return x->risk == 0 && str_eq_n(x->preview, "Unsupported: no Notes app");
        case AR_NO_MESSAGING: return x->risk == 3 && str_eq_n(x->preview, "Unsupported: no messaging service");
        case AR_NO_PAGES: return !caps->pages && x->risk == 0 && str_eq_n(x->preview, "Unsupported: Pages is not available yet");
        default: return 0;
        }
    }
    case AK_DESTRUCTIVE: return x->status == AS_REJECTED && x->reason == AR_NEEDS_BROKER && x->risk == 3 && x->target == 0 && x->arg_kind == AA_NONE && x->arg == 0 && str_eq_n(x->preview, "Not done: needs broker and trusted approval");
    case AK_REFUSED: return x->status == AS_REJECTED && x->reason == AR_INJECTION && x->risk == 3 && x->target == 0 && x->arg_kind == AA_NONE && x->arg == 0 && str_eq_n(x->preview, "Refused: not a command");
    case AK_CLARIFY:
        if (x->status != AS_CLARIFY || x->risk != 0 || x->target != 0 || x->arg_kind != AA_NONE || x->arg != 0) return 0;
        switch (x->reason) {
        case AR_EMPTY: return str_eq_n(x->preview, "Say or type a command");
        case AR_MULTIPLE: return str_eq_n(x->preview, "One command at a time");
        case AR_AMBIGUOUS: return str_eq_n(x->preview, "Which one do you mean?");
        case AR_MISSING_ARG: return str_eq_n(x->preview, "Open what?") || str_eq_n(x->preview, "That command needs a value");
        default: return 0;
        }
    case AK_UNKNOWN:
        if (x->status != AS_REJECTED || x->risk != 0 || x->target != 0 || x->arg_kind != AA_NONE || x->arg != 0) return 0;
        if (x->reason == AR_NOT_UNDERSTOOD) return str_eq_n(x->preview, "Not a command I know");
        if (x->reason == AR_TOO_LONG) return str_eq_n(x->preview, "Command too long");
        if (x->reason == AR_BAD_ENCODING) return str_eq_n(x->preview, "Plain ASCII text only");
        return 0;
    default: return 0;
    }
}

static const char *const KN[AK_COUNT] = { "none", "open_screen", "select_space", "set_setting", "unsupported", "destructive", "clarify", "unknown", "refused" };
static const char *const SN[AS_COUNT] = { "actionable", "needs_approval", "unsupported", "clarify", "rejected" };
static const char *const RN[AR_COUNT] = { "none", "empty", "too_long", "bad_encoding", "injection", "multiple", "ambiguous", "missing_arg", "range", "not_understood", "no_timer", "no_hardware", "no_notes", "no_messaging", "no_pages", "needs_broker" };
const char *aura_kind_name(uint8_t k) { return k < AK_COUNT ? KN[k] : "?"; }
const char *aura_status_name(uint8_t s) { return s < AS_COUNT ? SN[s] : "?"; }
const char *aura_reason_name(uint8_t r) { return r < AR_COUNT ? RN[r] : "?"; }
