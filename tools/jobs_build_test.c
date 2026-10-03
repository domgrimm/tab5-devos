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

    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}
