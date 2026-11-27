/* tests/kernel-host/speech_kw_test.c - host tests for kernel/include/speech_kw.h */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <speech_kw.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

int main(void)
{
    struct skw_table tbl;
    struct skw_match m;
    skw_table_init(&tbl);

    struct skw_vec v_home  = { { 60, 25, 80, 30 } };
    struct skw_vec v_notes = { { 70, 65, 45, 65 } };
    struct skw_vec v_timer = { { 75, 95, 25, 95 } };
    struct skw_vec v_neg   = { { -1, 10, 10, 10 } };

    /* Bad args */
    CHECK("null table on add refused", skw_add_template(0, SKW_CMD_HOME, &v_home) == SKW_E_ARG);
    CHECK("null vec on add refused", skw_add_template(&tbl, SKW_CMD_HOME, 0) == SKW_E_ARG);
    CHECK("CMD_NONE on add refused", skw_add_template(&tbl, SKW_CMD_NONE, &v_home) == SKW_E_ARG);
    CHECK("CMD_COUNT on add refused", skw_add_template(&tbl, SKW_CMD_COUNT, &v_home) == SKW_E_ARG);
    CHECK("negative feature refused", skw_add_template(&tbl, SKW_CMD_HOME, &v_neg) == SKW_E_ARG);
    CHECK("empty table returns NO_MATCH", skw_recognize(&tbl, &v_home, &m) == SKW_E_NO_MATCH && m.cmd == SKW_CMD_NONE);

    /* Register 3 keyword templates */
    CHECK("add HOME", skw_add_template(&tbl, SKW_CMD_HOME, &v_home) == SKW_OK);
    CHECK("add OPEN_NOTES", skw_add_template(&tbl, SKW_CMD_OPEN_NOTES, &v_notes) == SKW_OK);
    CHECK("add CANCEL_TIMER", skw_add_template(&tbl, SKW_CMD_CANCEL_TIMER, &v_timer) == SKW_OK);
    CHECK("count is 3", tbl.count == 3);

    /* Exact matches */
    CHECK("exact HOME", skw_recognize(&tbl, &v_home, &m) == SKW_OK && m.cmd == SKW_CMD_HOME && m.best_dist == 0);
    CHECK("exact OPEN_NOTES", skw_recognize(&tbl, &v_notes, &m) == SKW_OK && m.cmd == SKW_CMD_OPEN_NOTES && m.best_dist == 0);
    CHECK("exact CANCEL_TIMER", skw_recognize(&tbl, &v_timer, &m) == SKW_OK && m.cmd == SKW_CMD_CANCEL_TIMER && m.best_dist == 0);

    /* Noisy match within threshold and margin */
    struct skw_vec noisy_home = { { 63, 27, 78, 32 } }; /* L1 dist = 9 */
    CHECK("noisy HOME accepted", skw_recognize(&tbl, &noisy_home, &m) == SKW_OK && m.cmd == SKW_CMD_HOME && m.best_dist == 9);

    /* Silence (energy_q8 == 0) */
    struct skw_vec silence = { { 0, 10, 10, 10 } };
    CHECK("silence rejected", skw_recognize(&tbl, &silence, &m) == SKW_E_SILENCE && m.cmd == SKW_CMD_NONE);

    /* Out-of-vocabulary / far noise */
    struct skw_vec noise = { { 120, 5, 120, 5 } };
    CHECK("far noise rejected", skw_recognize(&tbl, &noise, &m) == SKW_E_NO_MATCH && m.cmd == SKW_CMD_NONE);

    /* Ambiguous tie between HOME and OPEN_NOTES */
    struct skw_vec mid = { { 65, 45, 62, 48 } }; /* dist to home = 5+20+18+18=61; let's test closer pair */
    struct skw_table amb;
    skw_table_init(&amb);
    struct skw_vec a1 = { { 50, 50, 50, 50 } };
    struct skw_vec a2 = { { 50, 54, 50, 50 } };
    struct skw_vec q  = { { 50, 52, 50, 50 } }; /* dist 2 to both -> margin 0 */
    skw_add_template(&amb, SKW_CMD_HOME, &a1);
    skw_add_template(&amb, SKW_CMD_OPEN_NOTES, &a2);
    CHECK("ambiguous tie rejected", skw_recognize(&amb, &q, &m) == SKW_E_AMBIGUOUS && m.cmd == SKW_CMD_NONE);
    CHECK("unused mid check", mid.v[0] == 65);

    /* Table capacity */
    for (uint32_t i = 3; i < SKW_MAX_TEMPLATES; i++) {
        CHECK("fill slot", skw_add_template(&tbl, SKW_CMD_HOME, &v_home) == SKW_OK);
    }
    CHECK("full table returns SKW_E_FULL", skw_add_template(&tbl, SKW_CMD_HOME, &v_home) == SKW_E_FULL);

    /* Null out / null table / negative query */
    CHECK("null out rejected", skw_recognize(&tbl, &v_home, 0) == SKW_E_ARG);
    CHECK("null table on recognize rejected", skw_recognize(0, &v_home, &m) == SKW_E_ARG);
    CHECK("negative query rejected", skw_recognize(&tbl, &v_neg, &m) == SKW_E_ARG);

    /* Command names */
    CHECK("cmd name home", strcmp(skw_cmd_name(SKW_CMD_HOME), "home") == 0);
    CHECK("cmd name open_notes", strcmp(skw_cmd_name(SKW_CMD_OPEN_NOTES), "open_notes") == 0);
    CHECK("cmd name cancel_timer", strcmp(skw_cmd_name(SKW_CMD_CANCEL_TIMER), "cancel_timer") == 0);
    CHECK("cmd name none", strcmp(skw_cmd_name(SKW_CMD_NONE), "none") == 0);

    printf("speech_kw: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
