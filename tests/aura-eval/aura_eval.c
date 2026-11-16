/* tests/aura-eval/aura_eval.c - the deterministic side of the Aura evaluation: reads one request per line from stdin, runs the REAL validator and the REAL
 * broker policy from the kernel sources, and prints one result line. A model (or any other proposer) is an untrusted source of intents; this program is the
 * judge it must pass. Host-only (cc -std=c11).
 *
 *   I <requester> <version> <kind> <status> <reason> <risk> <target> <arg_kind> <arg> <preview_hex|->    an intent proposed by a model (numeric enums)
 *   P <requester> <text_hex|->                                                                           text, parsed by the reference parser
 *
 * requester: shell (the trusted user path: navigate + settings rights), noright (a live EL0 client with no rights), forged (the shell's pid with a wrong
 * generation), stale (a pid that is not live). Each line is evaluated with a fresh replay table and nonce 1.
 * Output: valid=<0|1> decision=<name> reason=<name> risk=<n> | and for P lines the parsed intent: kind=<name> status=<name> target=<n> arg_kind=<n> arg=<n> */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <broker_policy.h>

static const struct aura_caps CAPS = { 1, 0, 0, 0, 0 };
static int lookup(uint32_t pid, uint64_t *gen, uint32_t *principal, uint32_t *rights)
{
    if (pid == BP_SHELL_PID) { *gen = BP_SHELL_GEN; *principal = 0x7F000003u; *rights = BP_RIGHTS_ALL; return 1; }
    if (pid == 1) { *gen = 10; *principal = 0x1001u; *rights = 0; return 1; }
    return 0;
}
static int hex(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int unhex(const char *h, unsigned char *out, int cap)
{
    if (!strcmp(h, "-")) return 0;
    int n = 0;
    for (; h[0] && h[1] && n < cap; h += 2) { int a = hex(h[0]), b = hex(h[1]); if (a < 0 || b < 0) return -1; out[n++] = (unsigned char)(a * 16 + b); }
    return n;
}
static int who(const char *w, uint32_t *pid, uint64_t *gen)
{
    if (!strcmp(w, "shell")) { *pid = 0; *gen = BP_SHELL_GEN; return 1; }
    if (!strcmp(w, "noright")) { *pid = 1; *gen = 10; return 1; }
    if (!strcmp(w, "forged")) { *pid = 0; *gen = 999; return 1; }
    if (!strcmp(w, "stale")) { *pid = 5; *gen = 50; return 1; }
    return 0;
}
static void decide(const char *rq, const struct aura_intent *in, int *valid, struct bp_result *res)
{
    struct bp_env env; memset(&env, 0, sizeof env); env.caps = &CAPS; env.lookup = lookup; env.nav_service = 1;
    struct bp_replay rp; bp_replay_init(&rp);
    struct bp_request q; memset(&q, 0, sizeof q); who(rq, &q.pid, &q.gen); q.nonce = 1; q.intent = *in;
    *valid = aura_validate(in, &CAPS);
    bp_decide(&env, &rp, &q, res);
}

int main(void)
{
    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        char *tok[16]; int nt = 0;
        for (char *p = strtok(line, " \r\n"); p && nt < 16; p = strtok(0, " \r\n")) tok[nt++] = p;
        if (!nt) continue;
        uint32_t pid; uint64_t gen; struct aura_intent in; memset(&in, 0, sizeof in);
        if (!strcmp(tok[0], "P") && nt == 3 && who(tok[1], &pid, &gen)) {
            unsigned char text[256]; int n = unhex(tok[2], text, sizeof text);
            if (n < 0) { puts("error=bad_hex"); continue; }
            aura_parse((const char *)text, (size_t)n, &CAPS, &in);
        } else if (!strcmp(tok[0], "I") && nt == 11 && who(tok[1], &pid, &gen)) {
            in.version = (uint8_t)atoi(tok[2]); in.kind = (uint8_t)atoi(tok[3]); in.status = (uint8_t)atoi(tok[4]); in.reason = (uint8_t)atoi(tok[5]); in.risk = (uint8_t)atoi(tok[6]);
            in.target = (uint8_t)atoi(tok[7]); in.arg_kind = (uint8_t)atoi(tok[8]); in.arg = (int32_t)atol(tok[9]);
            unsigned char pv[AURA_PREVIEW_MAX]; int n = unhex(tok[10], pv, sizeof pv);
            if (n < 0) { puts("error=bad_hex"); continue; }
            memcpy(in.preview, pv, (size_t)n);                   /* not NUL-terminated on purpose if n == AURA_PREVIEW_MAX: the validator must catch it */
        } else { puts("error=bad_line"); continue; }
        int valid; struct bp_result r; decide(tok[1], &in, &valid, &r);
        printf("valid=%d decision=%s reason=%s risk=%d", valid, bp_decision_name(r.decision), bp_reason_name(r.reason), r.risk == 255 ? -1 : r.risk);
        if (tok[0][0] == 'P') printf(" kind=%s status=%s target=%d arg_kind=%d arg=%d", aura_kind_name(in.kind), aura_status_name(in.status), in.target, in.arg_kind, in.arg);
        putchar('\n');
    }
    return 0;
}
