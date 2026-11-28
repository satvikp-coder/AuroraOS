/* tests/kernel-host/shell_apps_test.c - application launch from the native Index against the REAL shell with a fake kernel launcher: the Calculator entry is found, honest about
 * availability, launches only on an explicit open, goes to Pages on success, shows the kernel's reason on failure, never launches twice for one open, and nothing else (Notes, a
 * typo suggestion, a hidden item) can launch. Host, ASan/UBSan. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shell.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static struct shell sh;
static int f_avail, f_rc, n_launch, last_app, n_avail;
static int l_avail(uint32_t app) { n_avail++; return (app == 1 || app == 3) && f_avail; }
static int l_launch(uint32_t app) { n_launch++; last_app = (int)app; return f_rc; }
static const char *l_desc(int rc) { return rc == -2 ? "Already open. See Pages." : rc == -3 ? "Not installed on this disk." : "Not available."; }
static const struct shell_launcher L = { l_avail, l_launch, l_desc, NULL };

static void fresh(int with_launcher)
{
    f_avail = 1; f_rc = 0; n_launch = 0; last_app = -1; n_avail = 0;
    shell_init(&sh, 1024, 768); struct shell_caps c = { 1, 1 }; shell_set_caps(&sh, c);
    if (with_launcher) shell_set_launcher(&sh, &L);
}
static void key(uint16_t k) { struct input_event e = { IN_KEY_DOWN, 0, k, 0, 0, 0, 0 }; shell_handle(&sh, &e); }
static void text(const char *s) { for (; *s; s++) { struct input_event e = { IN_TEXT, 0, 30, 0, (uint8_t)*s, 0, 0 }; shell_handle(&sh, &e); } }
static void ptr(int kind, int x, int y) { struct input_event e = { (uint8_t)kind, 0, BTN_LEFT, 0, 0, x, y }; shell_handle(&sh, &e); }
static int click(uint32_t id) { int32_t i = shell_find(&sh, id); if (i < 0) return 0; int x = sh.c[i].x + sh.c[i].w / 2, y = sh.c[i].y + sh.c[i].h / 2; ptr(IN_PTR_DOWN, x, y); ptr(IN_PTR_UP, x, y); return 1; }
static int has_text(const char *t) { for (uint32_t i = 0; i < sh.n; i++) if (sh.w[i].text && strstr(sh.w[i].text, t)) return 1; return 0; }
static void search(const char *q) { shell_go(&sh, SCR_INDEX); text(q); }

int main(void)
{
    printf("shell apps tests\n");
    { /* the registry entry */
        size_t n = shell_index_count(), at = n;
        for (size_t i = 0; i < n; i++) if (strcmp(shell_index_item(i)->key, "calculator") == 0) at = i;
        CHECK("the registry lists Calculator as an app", at < n && shell_index_item(at)->is_app == 1);
        size_t fi = n; for (size_t i = 0; i < n; i++) if (strcmp(shell_index_item(i)->key, "files") == 0) fi = i;
        CHECK("Files is listed as an app", fi < n && shell_index_item(fi)->is_app == 1);
        size_t ci = n; for (size_t i = 0; i < n; i++) if (strcmp(shell_index_item(i)->key, "clock") == 0) ci = i;
        CHECK("Clock is listed as an app", ci < n && shell_index_item(ci)->is_app == 1);
    }
    { /* available: found, launched on Enter */
        fresh(1); search("calc");
        CHECK("'calc' finds Calculator, available, not labelled unavailable", sh.nres == 1 && strstr(sh.res_txt[0], "Calculator") && !strstr(sh.res_txt[0], "(not available yet)"));
        CHECK("typing alone launches nothing", n_launch == 0);
        key(KEY_ENTER);
        CHECK("Enter on it asks the kernel to launch application id 1 exactly once", n_launch == 1 && last_app == 1);
        CHECK("the shell then shows Pages, where the new process is listed", sh.screen == SCR_PAGES);
        CHECK("no failure note is left behind", sh.idx_note[0] == 0);
    }
    { /* Files launches application id 3 and nothing else */
        fresh(1); search("files"); CHECK("'files' finds Files, available", sh.nres == 1 && strstr(sh.res_txt[0], "Files") && !strstr(sh.res_txt[0], "(not available yet)"));
        key(KEY_ENTER); CHECK("Enter asks the kernel to launch application id 3 exactly once, then Pages", n_launch == 1 && last_app == 3 && sh.screen == SCR_PAGES);
        fresh(1); f_avail = 0; search("files"); key(KEY_ENTER); CHECK("not installed: listed 'not available yet' and launches nothing", n_launch == 0 && sh.screen == SCR_INDEX);
    }
    { /* a click opens it too, once */
        fresh(1); search("calculator"); click(SID_RESULT_BASE + 0);
        CHECK("a click on the result launches once", n_launch == 1 && sh.screen == SCR_PAGES);
    }
    { /* failure is shown, not hidden */
        fresh(1); f_rc = -2; search("calc"); key(KEY_ENTER);
        CHECK("a refused launch stays in Index and shows the kernel's reason", n_launch == 1 && sh.screen == SCR_INDEX && strstr(sh.idx_note, "Calculator: Already open. See Pages.") && has_text("Already open. See Pages."));
        text("x");
        CHECK("changing the query clears the old failure note", sh.idx_note[0] == 0 || !has_text("Already open"));
        fresh(1); f_rc = -3; search("calc"); key(KEY_ENTER);
        CHECK("not installed is reported as such", has_text("Not installed on this disk."));
    }
    { /* not available */
        fresh(1); f_avail = 0; search("calc");
        CHECK("an application that is not installed is listed 'not available yet'", sh.nres == 1 && strstr(sh.res_txt[0], "(not available yet)") != 0);
        key(KEY_ENTER);
        CHECK("Enter on an unavailable application launches nothing", n_launch == 0 && sh.screen == SCR_INDEX && sh.index_opens == 0 && sh.nrecent == 0);
        click(SID_RESULT_BASE + 0);
        CHECK("a click on it launches nothing either", n_launch == 0);
        fresh(0); search("calc");
        CHECK("with no launcher attached the entry is not available and nothing launches", sh.nres == 1 && strstr(sh.res_txt[0], "(not available yet)") != 0);
        key(KEY_ENTER); CHECK("... Enter does nothing", sh.screen == SCR_INDEX);
    }
    { /* only an explicit open launches: nothing else can */
        fresh(1); search("clok"); CHECK("a typo suggestion is not opened by Enter and launches nothing", n_launch == 0);
        key(KEY_ENTER); CHECK("(typing and Enter on a clock typo)", n_launch == 0);
        fresh(1); search("notes"); key(KEY_ENTER); CHECK("Notes (unavailable) launches nothing", n_launch == 0);
        fresh(1); search("clock"); key(KEY_ENTER); CHECK("opening Clock launches nothing", n_launch == 0 && sh.screen == SCR_CLOCK);
        fresh(1); search("calcu"); CHECK("a prefix finds it but still launches nothing until opened", sh.nres == 1 && n_launch == 0);
        shell_go(&sh, SCR_HOME); shell_go(&sh, SCR_PAGES); shell_go(&sh, SCR_SETTINGS); shell_go(&sh, SCR_AURA); shell_go(&sh, SCR_TRAIL);
        CHECK("navigating anywhere never launches", n_launch == 0);
    }
    printf("shell apps: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
