/* Host test for the Jobs Builder core (components/devos_jobs/jobs_build.c):
 * load, row flattening, trigger edits, action argument edits, add/delete/move
 * steps, custom-node detection and the round trip through jobs_serialize.
 *
 *   gcc -O2 -Icomponents/devos_jobs -Icomponents/devos_actions -Icomponents/devos_err \
 *       tools/jobs_build_test.c components/devos_jobs/jobs_model.c \
 *       components/devos_jobs/jobs_parse.c components/devos_jobs/jobs_validate.c \
 *       components/devos_jobs/jobs_serialize.c components/devos_jobs/jobs_build.c \
 *       components/devos_actions/devos_actions.c -o /tmp/jobs_build_test && /tmp/jobs_build_test
 */
#include "jobs_build.h"
#include "devos_actions.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static const devos_action_param_t PING_P[] = {
    { .name = "host", .type = DEVOS_VAL_STR, .required = true, .expression = true },
    { .name = "timeout", .type = DEVOS_VAL_DURATION, .expression = true, .min = 200, .max = 30000 },
};
static const devos_action_out_t PING_O[] = { { .name = "ok", .type = DEVOS_VAL_BOOL } };
static const devos_action_descriptor_t PING = {
    .id = "network.ping", .schema_version = 1, .params = PING_P, .param_count = 2,
    .outs = PING_O, .out_count = 1,
};
static const devos_action_param_t LOG_P[] = {
    { .name = "message", .type = DEVOS_VAL_STR, .required = true, .expression = true, .max_len = 512 },
};
static const devos_action_out_t LOG_O[] = { { .name = "recorded", .type = DEVOS_VAL_BOOL } };
static const devos_action_descriptor_t LOG = {
    .id = "system.log", .schema_version = 1, .params = LOG_P, .param_count = 1, .outs = LOG_O, .out_count = 1,
};
static const devos_action_param_t HTTP_P[] = {
    { .name = "method", .type = DEVOS_VAL_STR, .choices = "GET|POST" },
    { .name = "url", .type = DEVOS_VAL_STR, .required = true, .expression = true },
    { .name = "bearer_token", .type = DEVOS_VAL_STR, .credential = true },
};
static const devos_action_out_t HTTP_O[] = { { .name = "status", .type = DEVOS_VAL_INT } };
static const devos_action_descriptor_t HTTP = {
    .id = "http.request", .schema_version = 1, .params = HTTP_P, .param_count = 3,
    .outs = HTTP_O, .out_count = 1,
};

static const char *SRC =
    "version 1;\n"
    "job \"nas\" {\n"
    "    trigger every 5m;\n"
    "    network.ping(host: \"nas.local\", timeout: 3s) as nas;\n"
    "    if !nas.ok {\n"
    "        system.log(message: \"offline\");\n"
    "    }\n"
    "}\n";

int main(void)
{
    devos_actions_register(&PING);
    devos_actions_register(&LOG);
    devos_actions_register(&HTTP);

    jobs_build_t b;
    memset(&b, 0, sizeof(b));
    CHECK(jobs_build_load(&b, SRC, strlen(SRC)));
    CHECK(b.diag[0] == '\0');

    /* rows: ping, if, then system.log (depth 1) */
    jobs_build_row_t rows[JOBS_BUILD_ROWS];
    int n = jobs_build_rows(&b, rows, JOBS_BUILD_ROWS);
    CHECK(n == 3);
    CHECK(rows[0].node->kind == JN_ACTION && rows[0].depth == 0);
    CHECK(rows[1].node->kind == JN_IF && rows[1].depth == 0);
    CHECK(rows[2].node->kind == JN_ACTION && rows[2].depth == 1 && strcmp(rows[2].branch, "if") == 0);

    /* trigger card: change kind + value */
    CHECK(jobs_build_set_trigger_kind(&b, JTRIG_DAILY));
    CHECK(jobs_build_set_trigger_time(&b, "07:30"));
    CHECK(jobs_build_trigger(&b)->sub == JTRIG_DAILY && strcmp(jobs_build_trigger(&b)->u.str.s, "07:30") == 0);
    CHECK(jobs_build_set_trigger_kind(&b, JTRIG_EVERY));
    CHECK(jobs_build_set_trigger_duration(&b, 120000));
    CHECK(jobs_build_trigger(&b)->u.i == 120000);

    /* trigger card: an event trigger (topic + optional where) round-trips */
    CHECK(jobs_build_set_trigger_kind(&b, JTRIG_EVENT));
    CHECK(strcmp(jobs_build_trigger_event_topic(jobs_build_trigger(&b)), "system.boot") == 0);
    CHECK(jobs_build_set_trigger_event(&b, "network.wifi_connected"));
    CHECK(strcmp(jobs_build_trigger_event_topic(jobs_build_trigger(&b)), "network.wifi_connected") == 0);
    CHECK(jobs_build_set_trigger_where(&b, "contains(event.payload, \"home\")"));
    CHECK(strstr(jobs_build_trigger_where_text(jobs_build_trigger(&b)), "contains") != NULL);
    CHECK(jobs_build_revalidate(&b));
    {
        char ev[1024];
        size_t el = jobs_build_source(&b, ev, sizeof(ev));
        CHECK(strstr(ev, "trigger event \"network.wifi_connected\"") != NULL);
        CHECK(strstr(ev, "where") != NULL);
        jobs_build_t e2;
        memset(&e2, 0, sizeof(e2));
        CHECK(jobs_build_load(&e2, ev, el));
        CHECK(jobs_build_trigger(&e2)->sub == JTRIG_EVENT);
        jobs_build_free(&e2);
    }
    CHECK(jobs_build_set_trigger_where(&b, ""));       /* clearing the filter is fine */
    CHECK(jobs_build_revalidate(&b));
    CHECK(jobs_build_set_trigger_kind(&b, JTRIG_EVERY));   /* back for the rest of the test */
    CHECK(jobs_build_set_trigger_duration(&b, 120000));

    /* inspector: edit a literal argument */
    CHECK(jobs_build_arg_text(rows[0].node, "host") && strcmp(jobs_build_arg_text(rows[0].node, "host"), "nas.local") == 0);
    CHECK(jobs_build_arg_is_simple(rows[0].node, "host"));
    CHECK(jobs_build_set_arg_str(&b, rows[0].node, "host", "router.local"));
    CHECK(strcmp(jobs_build_arg_text(rows[0].node, "host"), "router.local") == 0);
    CHECK(jobs_build_set_arg_duration(&b, rows[0].node, "timeout", 5000));
    CHECK(jobs_build_arg_duration(rows[0].node, "timeout") == 5000);

    /* custom node: a compound expression is not simple */
    {
        const char *C =
            "version 1;\njob \"c\" {\n trigger manual;\n"
            " system.log(message: \"x\");\n if 1 < 2 { system.log(message: \"y\"); }\n}\n";
        jobs_build_t c;
        memset(&c, 0, sizeof(c));
        CHECK(jobs_build_load(&c, C, strlen(C)));
        /* the if condition (1 < 2) is a binary expression -> custom card */
        jobs_build_row_t r[JOBS_BUILD_ROWS];
        int m = jobs_build_rows(&c, r, JOBS_BUILD_ROWS);
        CHECK(m == 3 && r[1].node->kind == JN_IF);
        jobs_build_free(&c);
    }

    /* add an action built from its schema (enum + numeric defaults filled) */
    const jobs_node_t *body = b.ast->root->c;
    CHECK(jobs_build_add_action(&b, body, "http.request"));
    CHECK(jobs_build_revalidate(&b));                 /* structurally valid */
    n = jobs_build_rows(&b, rows, JOBS_BUILD_ROWS);
    CHECK(rows[n - 1].node->kind == JN_ACTION && strcmp(rows[n - 1].node->u.str.s, "http.request") == 0);
    CHECK(strcmp(jobs_build_arg_text(rows[n - 1].node, "method"), "GET") == 0);
    CHECK(jobs_build_arg_secret_name(rows[n - 1].node, "bearer_token") == NULL);

    /* credential argument -> secret("name") */
    CHECK(jobs_build_set_arg_secret(&b, rows[n - 1].node, "bearer_token", "tok"));
    CHECK(strcmp(jobs_build_arg_secret_name(rows[n - 1].node, "bearer_token"), "tok") == 0);
    CHECK(jobs_build_revalidate(&b));

    /* move + delete a step */
    CHECK(jobs_build_move(&b, body, rows[n - 1].node, -1));   /* http.request up one */
    CHECK(jobs_build_revalidate(&b));
    n = jobs_build_rows(&b, rows, JOBS_BUILD_ROWS);
    CHECK(rows[1].node->kind == JN_ACTION && strcmp(rows[1].node->u.str.s, "http.request") == 0);
    CHECK(jobs_build_delete(&b, rows[1].block, rows[1].node)); /* delete it again */
    CHECK(jobs_build_revalidate(&b));
    CHECK(jobs_build_rows(&b, rows, JOBS_BUILD_ROWS) == n - 1);

    /* conditions and advanced expressions (editable in the Builder) */
    {
        const char *C =
            "version 1;\njob \"c\" {\n trigger manual;\n"
            " set x = 1;\n network.ping(host: \"a\") as p;\n"
            " if p.ok { system.log(message: \"ok\"); }\n}\n";
        jobs_build_t c;
        memset(&c, 0, sizeof(c));
        CHECK(jobs_build_load(&c, C, strlen(C)));
        jobs_build_row_t r[JOBS_BUILD_ROWS];
        int m = jobs_build_rows(&c, r, JOBS_BUILD_ROWS);
        CHECK(m == 4);
        CHECK(r[0].node->kind == JN_SET);
        CHECK(jobs_build_set_set_value(&c, r[0].node, "json_get(\"{}\", \"n\")"));
        CHECK(strstr(jobs_build_expr_text(jobs_build_set_value(r[0].node)), "json_get") != NULL);
        CHECK(r[2].node->kind == JN_IF);
        CHECK(jobs_build_set_if_cond(&c, r[2].node, "p.ok && x > 1"));
        CHECK(strstr(jobs_build_expr_text(jobs_build_if_cond(r[2].node)), "p.ok") != NULL);
        CHECK(jobs_build_revalidate(&c));
        /* an expression-capable argument can hold a full expression (a call) */
        CHECK(r[3].node->kind == JN_ACTION);
        CHECK(jobs_build_set_arg_expr(&c, r[3].node, "message", "json_get(\"{}\", \"x\")"));
        CHECK(jobs_build_arg_is_expr(r[3].node, "message"));
        CHECK(jobs_build_revalidate(&c));
        /* move_to reorders within a block (keeps definitions before uses) */
        CHECK(jobs_build_block_count(c.ast->root->c) == 3);
        CHECK(jobs_build_move_to(&c, c.ast->root->c, r[0].node, 1));
        CHECK(jobs_build_revalidate(&c));
        jobs_build_free(&c);
    }

    /* opaque repeat: it appears as a single row (its body is not flattened),
     * and editing a sibling preserves the repeat source exactly */
    {
        const char *R =
            "version 1;\njob \"r\" {\n trigger manual;\n"
            " system.log(message: \"before\");\n"
            " repeat 3 as i {\n system.log(message: \"x\");\n system.log(message: \"y\");\n }\n"
            " system.log(message: \"after\");\n}\n";
        jobs_build_t rb;
        memset(&rb, 0, sizeof(rb));
        CHECK(jobs_build_load(&rb, R, strlen(R)));
        jobs_build_row_t rr[JOBS_BUILD_ROWS];
        int rn = jobs_build_rows(&rb, rr, JOBS_BUILD_ROWS);
        CHECK(rn == 3);                                  /* before, repeat, after - body hidden */
        CHECK(rr[1].node->kind == JN_REPEAT && rr[1].depth == 0);
        CHECK(rr[1].node->count == 3);
        CHECK(jobs_build_set_repeat(&rb, rr[1].node, 5, "k"));
        CHECK(rr[1].node->count == 5 && strcmp(rr[1].node->u.str.s, "k") == 0);
        CHECK(jobs_build_revalidate(&rb));
        CHECK(jobs_build_set_arg_str(&rb, rr[0].node, "message", "start"));
        CHECK(jobs_build_revalidate(&rb));
        char ro[1024];
        jobs_build_source(&rb, ro, sizeof(ro));
        CHECK(strstr(ro, "repeat 5 as k {") != NULL);
        CHECK(strstr(ro, "system.log(message: \"x\");") != NULL);
        CHECK(strstr(ro, "system.log(message: \"y\");") != NULL);
        CHECK(strstr(ro, "\"before\"") == NULL && strstr(ro, "\"start\"") != NULL);
        CHECK(jobs_build_delete(&rb, rr[1].block, rr[1].node));
        CHECK(jobs_build_revalidate(&rb));
        char ro2[1024];
        jobs_build_source(&rb, ro2, sizeof(ro2));
        CHECK(strstr(ro2, "repeat") == NULL && strstr(ro2, "\"start\"") != NULL && strstr(ro2, "\"after\"") != NULL);
        jobs_build_free(&rb);
    }

    /* job input parameters and calls survive a Builder load/serialize */
    {
        const char *P =
            "version 1;\njob \"Sub\"(host: string) {\n trigger manual;\n"
            " network.ping(host: host) as p;\n return p.ok;\n}\n";
        jobs_build_t pb;
        memset(&pb, 0, sizeof(pb));
        CHECK(jobs_build_load(&pb, P, strlen(P)));
        char po[1024];
        jobs_build_source(&pb, po, sizeof(po));
        CHECK(strstr(po, "job \"Sub\"(host: string)") != NULL);
        CHECK(strstr(po, "return p.ok;") != NULL);
        jobs_build_free(&pb);

        const char *C =
            "version 1;\njob \"Main\" {\n trigger manual;\n"
            " run \"Sub\"(host: \"nas\") as ok;\n"
            " if ok { system.log(message: \"up\"); }\n}\n";
        memset(&pb, 0, sizeof(pb));
        CHECK(jobs_build_load(&pb, C, strlen(C)));
        jobs_build_row_t cr[JOBS_BUILD_ROWS];
        int cn = jobs_build_rows(&pb, cr, JOBS_BUILD_ROWS);
        CHECK(cn == 3 && cr[0].node->kind == JN_RUN && cr[1].node->kind == JN_IF &&
              cr[2].node->kind == JN_ACTION && cr[2].depth == 1);
        jobs_build_source(&pb, po, sizeof(po));
        CHECK(strstr(po, "run \"Sub\"(host: \"nas\") as ok;") != NULL);
        jobs_build_free(&pb);
    }

    /* the serialized draft is a valid, stable definition */
    char out1[2048], out2[2048];
    size_t l1 = jobs_build_source(&b, out1, sizeof(out1));
    CHECK(l1 > 0 && l1 < sizeof(out1));
    jobs_build_t b2;
    memset(&b2, 0, sizeof(b2));
    CHECK(jobs_build_load(&b2, out1, l1));
    size_t l2 = jobs_build_source(&b2, out2, sizeof(out2));
    CHECK(l2 == l1 && strcmp(out1, out2) == 0);       /* idempotent */
    jobs_build_free(&b2);
    jobs_build_free(&b);

    /* P2: add control statements, bind an output, rename, set the policy */
    {
        const char *CP2 = "version 1;\njob \"Cap\" {\n trigger manual;\n system.log(message: \"x\");\n}\n";
        jobs_build_t cb;
        memset(&cb, 0, sizeof(cb));
        CHECK(jobs_build_load(&cb, CP2, strlen(CP2)));
        jobs_node_t *body = jobs_build_trigger(&cb) ? cb.ast->root->c : NULL;
        CHECK(body != NULL);
        int before = jobs_build_block_count(body);
        CHECK(jobs_build_add_if(&cb, body, "true"));
        CHECK(jobs_build_add_set(&cb, body, "n", "42"));
        CHECK(jobs_build_add_wait(&cb, body, 250));
        CHECK(jobs_build_add_repeat(&cb, body, 3, "i"));
        CHECK(jobs_build_add_run(&cb, body, "Echo", "out"));
        CHECK(jobs_build_block_count(body) == before + 5);
        /* the new run statement carries its output binding */
        jobs_node_t *run = NULL;
        for (jobs_node_t *s = body->a; s; s = s->next) if (s->kind == JN_RUN) run = s;
        CHECK(run && jobs_build_output(run) && strcmp(jobs_build_output(run), "out") == 0);
        CHECK(jobs_build_set_output(&cb, run, "reply") &&
              strcmp(jobs_build_output(run), "reply") == 0);
        CHECK(jobs_build_set_job_name(&cb, "Renamed"));
        CHECK(strcmp(cb.ast->root->u.str.s, "Renamed") == 0);
        CHECK(jobs_build_set_policy(&cb, 5000, 1, 1000));
        CHECK(jobs_build_policy_timeout(cb.ast->root->b) == 5000);
        CHECK(jobs_build_policy_overlap(cb.ast->root->b) == 1);
        CHECK(jobs_build_policy_cooldown(cb.ast->root->b) == 1000);
        /* the result still validates and round-trips */
        CHECK(jobs_build_revalidate(&cb));
        char co[2048];
        size_t cl = jobs_build_source(&cb, co, sizeof(co));
        CHECK(cl > 0 && strstr(co, "if true {") != NULL);
        CHECK(strstr(co, "set n = 42;") != NULL);
        CHECK(strstr(co, "wait 250ms;") != NULL);
        CHECK(strstr(co, "repeat 3 as i {") != NULL);
        CHECK(strstr(co, "run \"Echo\" as reply;") != NULL);
        CHECK(strstr(co, "job \"Renamed\"") != NULL);
        CHECK(strstr(co, "policy(timeout: 5s") != NULL);
        jobs_build_free(&cb);
    }

    /* P2: indent a step into the preceding if/repeat, outdent it back */
    {
        const char *CB =
            "version 1;\njob \"IO\" {\n trigger manual;\n"
            " system.log(message: \"a\");\n"
            " if true {\n  system.log(message: \"in\");\n }\n"
            " system.log(message: \"b\");\n}\n";
        jobs_build_t ib;
        memset(&ib, 0, sizeof(ib));
        CHECK(jobs_build_load(&ib, CB, strlen(CB)));
        jobs_node_t *body = ib.ast->root->c;
        /* indent the trailing log into the if above it */
        jobs_node_t *trail = NULL;
        for (jobs_node_t *st = body->a; st; st = st->next) trail = st;
        CHECK(trail && jobs_build_indent(&ib, body, trail));
        CHECK(jobs_build_revalidate(&ib));
        char io[1024];
        jobs_build_source(&ib, io, sizeof(io));
        CHECK(strstr(io, "if true {") != NULL);
        /* the trailing log now lives inside the if body (depth 1) */
        jobs_build_row_t ir[JOBS_BUILD_ROWS];
        int in = jobs_build_rows(&ib, ir, JOBS_BUILD_ROWS);
        bool deep = false;
        for (int k = 0; k < in; k++)
            if (ir[k].depth == 1 && ir[k].node->kind == JN_ACTION) deep = true;
        CHECK(deep);
        /* outdent it back to the top level */
        jobs_node_t *moved = NULL, *mvblock = NULL;
        for (int k = 0; k < in; k++)
            if (ir[k].depth == 1 && ir[k].node->kind == JN_ACTION &&
                ir[k].block != body) { moved = (jobs_node_t *)ir[k].node; mvblock = (jobs_node_t *)ir[k].block; }
        CHECK(moved && mvblock && jobs_build_outdent(&ib, mvblock, moved));
        CHECK(jobs_build_revalidate(&ib));
        /* indenting the first statement, or outdenting top level, refuses */
        CHECK(!jobs_build_indent(&ib, body, body->a));
        CHECK(!jobs_build_outdent(&ib, body, body->a));
        jobs_build_free(&ib);
    }

    /* weekday selector: set, canonicalize, clear, refuse off-weekdays */
    {
        const char *CW = "version 1;\njob \"W\" {\n trigger weekdays \"08:00\";\n system.log(message: \"x\");\n}\n";
        jobs_build_t wb;
        memset(&wb, 0, sizeof(wb));
        CHECK(jobs_build_load(&wb, CW, strlen(CW)));
        CHECK(strcmp(jobs_build_trigger_days_text(jobs_build_trigger(&wb)), "") == 0);
        CHECK(jobs_build_trigger_days_mask(jobs_build_trigger(&wb)) == 0x3E);
        CHECK(jobs_build_set_trigger_days(&wb, "wed,MON"));
        CHECK(strcmp(jobs_build_trigger_days_text(jobs_build_trigger(&wb)), "Mon,Wed") == 0);
        CHECK(!jobs_build_set_trigger_days(&wb, "Funday"));
        char wo[512];
        CHECK(jobs_build_source(&wb, wo, sizeof(wo)) > 0);
        CHECK(strstr(wo, "days \"Mon,Wed\"") != NULL);
        CHECK(jobs_build_set_trigger_days(&wb, ""));
        CHECK(strcmp(jobs_build_trigger_days_text(jobs_build_trigger(&wb)), "") == 0);
        CHECK(jobs_build_set_trigger_kind(&wb, JTRIG_DAILY));
        CHECK(!jobs_build_set_trigger_days(&wb, "Mon"));
        jobs_build_free(&wb);
    }

    /* event trigger: debounce + include_retained are Builder-editable and
     * survive serialize -> parse (the scheduler reads them as literals) */
    {
        const char *CE = "version 1;\njob \"E\" {\n trigger event \"system.boot\";\n"
                         " system.log(message: \"x\");\n}\n";
        jobs_build_t eb;
        memset(&eb, 0, sizeof(eb));
        CHECK(jobs_build_load(&eb, CE, strlen(CE)));
        CHECK(jobs_build_trigger_debounce(jobs_build_trigger(&eb)) == 0);
        CHECK(!jobs_build_trigger_retained(jobs_build_trigger(&eb)));
        CHECK(jobs_build_set_trigger_debounce(&eb, 5000));
        CHECK(jobs_build_set_trigger_retained(&eb, true));
        CHECK(jobs_build_trigger_debounce(jobs_build_trigger(&eb)) == 5000);
        CHECK(jobs_build_trigger_retained(jobs_build_trigger(&eb)));
        char eo[512];
        CHECK(jobs_build_source(&eb, eo, sizeof(eo)) > 0);
        CHECK(strstr(eo, "debounce") != NULL && strstr(eo, "include_retained") != NULL);
        CHECK(jobs_build_revalidate(&eb));
        /* clearing removes the argument again, keeping the source clean */
        CHECK(jobs_build_set_trigger_debounce(&eb, 0));
        CHECK(jobs_build_set_trigger_retained(&eb, false));
        CHECK(jobs_build_source(&eb, eo, sizeof(eo)) > 0);
        CHECK(strstr(eo, "debounce") == NULL && strstr(eo, "include_retained") == NULL);
        /* and the setter refuses on a non-event trigger */
        CHECK(jobs_build_set_trigger_kind(&eb, JTRIG_MANUAL));
        CHECK(!jobs_build_set_trigger_debounce(&eb, 1000));
        jobs_build_free(&eb);
    }

    /* a `run` step's callee and output are settable from the Builder (the run
     * inspector), and the output survives serialize -> parse */
    {
        const char *CR = "version 1;\njob \"R\" {\n trigger manual;\n"
                         " run \"other\" as out;\n}\n";
        jobs_build_t rb;
        memset(&rb, 0, sizeof(rb));
        CHECK(jobs_build_load(&rb, CR, strlen(CR)));
        jobs_build_row_t rr[JOBS_BUILD_ROWS];
        int rn = jobs_build_rows(&rb, rr, JOBS_BUILD_ROWS);
        CHECK(rn == 1 && rr[0].node->kind == JN_RUN);
        const jobs_node_t *run = rr[0].node;
        CHECK(strcmp(jobs_build_output(run), "out") == 0);
        CHECK(jobs_build_set_run(&rb, run, "second job"));
        CHECK(jobs_build_set_output(&rb, run, "r2"));
        char ro[512];
        CHECK(jobs_build_source(&rb, ro, sizeof(ro)) > 0);
        CHECK(strstr(ro, "run \"second job\" as r2") != NULL);
        CHECK(jobs_build_revalidate(&rb));
        jobs_build_free(&rb);
    }

    /* the form re-commits every field on each defocus; the arena and string
     * pool must not fill up, or the setter starts failing and the value
     * silently stops changing after a handful of edits */
    {
        static char big[4096];
        snprintf(big, sizeof(big),
                 "version 1;\njob \"S\" {\n trigger manual;\n"
                 " network.ping(host: \"nas.local\", timeout: 3s) as r;\n"
                 "}\n");
        jobs_build_t sb;
        memset(&sb, 0, sizeof(sb));
        CHECK(jobs_build_load(&sb, big, strlen(big)));
        jobs_build_row_t sr[JOBS_BUILD_ROWS];
        CHECK(jobs_build_rows(&sb, sr, JOBS_BUILD_ROWS) == 1);
        const jobs_node_t *act = sr[0].node;
        bool okall = true;
        for (int i = 0; i < 400; i++) {
            char host[64];
            snprintf(host, sizeof(host), "host-%d.local", i);
            /* what app_jobs form_commit does per defocus: every parameter */
            if (!jobs_build_set_arg_str(&sb, act, "host", host)) okall = false;
            if (!jobs_build_set_arg_duration(&sb, act, "timeout", 1000 + i)) okall = false;
            if (!jobs_build_set_output(&sb, act, "r")) okall = false;
            const char *cur = jobs_build_arg_text(act, "host");
            if (!cur || strcmp(cur, host) != 0) { okall = false; break; }
        }
        CHECK(okall);                             /* 400 commits of 2 fields still land */
        CHECK(jobs_build_revalidate(&sb));
        char so[1024];
        CHECK(jobs_build_source(&sb, so, sizeof(so)) > 0);
        CHECK(strstr(so, "host-399.local") != NULL);
        CHECK(strstr(so, "1399ms") != NULL);
        jobs_build_free(&sb);
    }

    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}
