/* Host test for the Jobs language v1: model, parser, validator, serializer
 * (components/devos_jobs). Pure logic - no LVGL, no network. Fake action
 * schemas are registered through the same devos_actions API providers use.
 *
 *   gcc -O2 -Icomponents/devos_jobs -Icomponents/devos_actions -Icomponents/devos_err \
 *       tools/jobs_parse_test.c \
 *       components/devos_jobs/jobs_model.c components/devos_jobs/jobs_parse.c \
 *       components/devos_jobs/jobs_validate.c components/devos_jobs/jobs_serialize.c \
 *       components/devos_actions/devos_actions.c \
 *       -o /tmp/jobs_parse_test && /tmp/jobs_parse_test
 */
#include "devos_actions.h"
#include "jobs_model.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- fake schemas (the shape real providers will register in Phase 2) ---- */
static const devos_action_param_t PING_P[] = {
    { .name = "host", .type = DEVOS_VAL_STR, .required = true, .expression = true },
    { .name = "timeout", .type = DEVOS_VAL_DURATION, .expression = true, .max = 60000, .min = 1 },
};
static const devos_action_out_t PING_O[] = {
    { .name = "ok", .type = DEVOS_VAL_BOOL }, { .name = "ip", .type = DEVOS_VAL_STR },
    { .name = "latency_ms", .type = DEVOS_VAL_INT }, { .name = "error_code", .type = DEVOS_VAL_STR },
};
static const devos_action_descriptor_t PING = {
    .id = "network.ping", .schema_version = 1, .provider_uid = "netdiag",
    .params = PING_P, .param_count = 2, .outs = PING_O, .out_count = 4,
    .effect = DEVOS_EFFECT_NET_SEND, .recommended_timeout_ms = 5000,
};
static const devos_action_param_t HTTP_P[] = {
    { .name = "method", .type = DEVOS_VAL_STR, .expression = true },
    { .name = "url", .type = DEVOS_VAL_STR, .required = true, .expression = true, .max_len = 1024 },
    { .name = "timeout", .type = DEVOS_VAL_DURATION, .expression = true, .min = 1, .max = 120000 },
    { .name = "max_body", .type = DEVOS_VAL_INT, .expression = true, .min = 1, .max = 65536 },
    { .name = "bearer_token", .type = DEVOS_VAL_STR, .credential = true },
    { .name = "body", .type = DEVOS_VAL_STR, .expression = true },
};
static const devos_action_out_t HTTP_O[] = {
    { .name = "ok", .type = DEVOS_VAL_BOOL }, { .name = "status", .type = DEVOS_VAL_INT },
    { .name = "body", .type = DEVOS_VAL_STR }, { .name = "truncated", .type = DEVOS_VAL_BOOL },
    { .name = "duration_ms", .type = DEVOS_VAL_INT }, { .name = "error", .type = DEVOS_VAL_STR },
};
static const devos_action_descriptor_t HTTP = {
    .id = "http.request", .schema_version = 1, .provider_uid = "rest",
    .params = HTTP_P, .param_count = 6, .outs = HTTP_O, .out_count = 6,
    .effect = DEVOS_EFFECT_NET_SEND, .recommended_timeout_ms = 15000,
};
static const devos_action_param_t NOTIFY_P[] = {
    { .name = "message", .type = DEVOS_VAL_STR, .required = true, .expression = true, .max_len = 512 },
    { .name = "level", .type = DEVOS_VAL_STR, .choices = "info|warning|error" },
};
static const devos_action_out_t NOTIFY_O[] = { { .name = "queued", .type = DEVOS_VAL_BOOL } };
static const devos_action_descriptor_t NOTIFY = {
    .id = "system.notify", .schema_version = 1, .provider_uid = "jobs",
    .params = NOTIFY_P, .param_count = 2, .outs = NOTIFY_O, .out_count = 1,
    .effect = DEVOS_EFFECT_MUTATE,
};
static const devos_action_param_t LOG_P[] = {
    { .name = "message", .type = DEVOS_VAL_STR, .required = true, .expression = true, .max_len = 512 },
};
static const devos_action_out_t LOG_O[] = { { .name = "recorded", .type = DEVOS_VAL_BOOL } };
static const devos_action_descriptor_t LOG = {
    .id = "system.log", .schema_version = 1, .provider_uid = "jobs",
    .params = LOG_P, .param_count = 1, .outs = LOG_O, .out_count = 1,
    .effect = DEVOS_EFFECT_READ,
};

static void register_fakes(void)
{
    devos_actions_reset();
    CHECK(devos_actions_register(&PING) == DEVOS_OK);
    CHECK(devos_actions_register(&HTTP) == DEVOS_OK);
    CHECK(devos_actions_register(&NOTIFY) == DEVOS_OK);
    CHECK(devos_actions_register(&LOG) == DEVOS_OK);
    CHECK(devos_actions_find("http.request") == &HTTP);
    CHECK(devos_actions_find("nope") == NULL);
}

static bool has_diag(const jobs_ast_t *ast, const char *needle)
{
    for (int i = 0; i < ast->diag_count; i++)
        if (strstr(ast->diag[i].msg, needle)) return true;
    return false;
}

/* Parse + validate; returns the AST (caller frees). */
static jobs_ast_t *pv(const char *src)
{
    jobs_ast_t *ast = jobs_parse(src, strlen(src), NULL);
    if (ast && ast->diag_count == 0) jobs_validate(ast);
    return ast;
}

static const char NAS[] =
    "version 1;\n"
    "job \"NAS health check\" {\n"
    "    trigger every 5m;\n"
    "    network.ping(host: \"nas.local\", timeout: 3s) as nas;\n"
    "    if !nas.ok {\n"
    "        system.notify(message: \"NAS offline\", level: \"warning\");\n"
    "    } else {\n"
    "        system.log(message: \"NAS responded in ${nas.latency_ms} ms\");\n"
    "    }\n"
    "}\n";

static const char WEB[] =
    "version 1;\n"
    "job \"Website monitor\" {\n"
    "    trigger every 2m;\n"
    "    http.request(method: \"GET\", url: \"https://example.com/health\",\n"
    "                 timeout: 10s, max_body: 4096) as response;\n"
    "    if !response.ok || response.status != 200 {\n"
    "        system.notify(message: \"Website failed: ${response.status}\", level: \"error\");\n"
    "    }\n"
    "}\n";

static void test_examples(void)
{
    jobs_ast_t *ast = pv(NAS);
    CHECK(ast && ast->diag_count == 0);
    if (ast && ast->diag_count) for (int i = 0; i < ast->diag_count; i++) printf("  nas: %s\n", ast->diag[i].msg);
    jobs_ast_free(ast);

    ast = pv(WEB);
    CHECK(ast && ast->diag_count == 0);
    if (ast && ast->diag_count) for (int i = 0; i < ast->diag_count; i++) printf("  web: %s\n", ast->diag[i].msg);
    jobs_ast_free(ast);

    /* the doorbell example (event trigger, no output) */
    const char *DOOR =
        "version 1;\n"
        "job \"Doorbell notice\" {\n"
        "    trigger event \"mqtt.message\"(topic: \"home/doorbell\", include_retained: false);\n"
        "    system.notify(message: \"Doorbell pressed\", level: \"info\");\n"
        "}\n";
    ast = pv(DOOR);
    CHECK(ast && ast->diag_count == 0);
    jobs_ast_free(ast);

    /* the README's bounded-retry example (repeat + read-only index + wait) */
    const char *RETRY =
        "version 1;\n"
        "job \"Wait for the NAS\" {\n"
        "    trigger every 1m;\n"
        "    set up = false;\n"
        "    repeat 5 as i {\n"
        "        if !up {\n"
        "            network.ping(host: \"nas.local\", timeout: 2s) as p;\n"
        "            if p.ok { set up = true; system.log(message: \"NAS up after ${i} tries\"); }\n"
        "            else { wait 2s; }\n"
        "        }\n"
        "    }\n"
        "    if !up { system.notify(message: \"NAS still offline\", level: \"warning\"); }\n"
        "}\n";
    ast = pv(RETRY);
    CHECK(ast && ast->diag_count == 0);
    if (ast && ast->diag_count) for (int i = 0; i < ast->diag_count; i++) printf("  retry: %s\n", ast->diag[i].msg);
    jobs_ast_free(ast);
}

static void test_roundtrip(void)
{
    jobs_ast_t *a = pv(NAS);
    CHECK(a && a->diag_count == 0);
    if (a) {
        char out1[2048], out2[2048];
        size_t n1 = jobs_serialize(a, out1, sizeof(out1));
        CHECK(n1 > 0 && n1 < sizeof(out1));
        jobs_ast_t *b = pv(out1);
        CHECK(b && b->diag_count == 0);
        if (b) {
            size_t n2 = jobs_serialize(b, out2, sizeof(out2));
            CHECK(n2 == n1 && strcmp(out1, out2) == 0);      /* idempotent canonical form */
            jobs_ast_free(b);
        }
        jobs_ast_free(a);
    }
    /* comments are dropped by canonical format but must parse cleanly */
    const char *C =
        "version 1;\n"
        "// a leading comment\n"
        "job \"c\" { // trailing\n"
        "    trigger manual; // pick me\n"
        "    system.log(message: \"hi\"); // done\n"
        "}\n";
    jobs_ast_t *c = pv(C);
    CHECK(c && c->diag_count == 0);
    if (c) { char o[512]; CHECK(jobs_serialize(c, o, sizeof(o)) > 0); jobs_ast_free(c); }
}

static void test_parse_errors(void)
{
    struct { const char *src; const char *needle; } bad[] = {
        { "version 1;\njob \"x\" { trigger every ; }\n", "duration" },
        { "version 1;\njob \"x { trigger manual; }\n", "unterminated string" },
        { "version 2;\njob \"x\" { trigger manual; }\n", "unsupported language version" },
        { "version 1;\njob \"x\" { trigger manual; system.log(message: );\n}\n", "expression" },
        { "version 1;\njob \"x\" { trigger manual; }\n}\n", "unexpected text" },
        { "job \"x\" { trigger manual; }\n", "version" },
        { "version 1;\njob \"x\" { system.log(message: \"a\"); }\n", "trigger" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        jobs_ast_t *ast = jobs_parse(bad[i].src, strlen(bad[i].src), NULL);
        if (!has_diag(ast, bad[i].needle)) {
            printf("FAIL parse error %zu: want '%s'\n", i, bad[i].needle);
            for (int d = 0; d < ast->diag_count; d++) printf("   got: %s\n", ast->diag[d].msg);
            fails++;
        }
        checks++;
        jobs_ast_free(ast);
    }
}

static void test_validate_errors(void)
{
    struct { const char *src; const char *needle; } bad[] = {
        { "version 1;\njob \"x\" { trigger manual; foo.bar(); }\n", "unknown action" },
        { "version 1;\njob \"x\" { trigger manual; network.ping(host: \"a\", nope: 1); }\n", "unknown argument" },
        { "version 1;\njob \"x\" { trigger manual; http.request(method: \"GET\"); }\n", "needs 'url'" },
        { "version 1;\njob \"x\" { trigger manual; network.ping(host: \"a\") as r; network.ping(host: \"b\") as r; }\n", "already defined" },
        { "version 1;\njob \"x\" { trigger manual; http.request(method: \"GET\", url: \"u\") as r; if r.status == \"x\" { } }\n", "cannot compare" },
        { "version 1;\njob \"x\" { trigger manual; if true { system.log(message: \"h\") as inner; } system.log(message: \"${inner.recorded}\"); }\n", "unknown variable" },
        { "version 1;\njob \"x\" { trigger manual; system.log(message: secret(\"t\")); }\n", "cannot take a secret" },
        { "version 1;\njob \"x\" { trigger manual; http.request(method: \"GET\", url: \"u\", bearer_token: \"abc\"); }\n", "needs secret" },
        { "version 1;\njob \"x\" { trigger manual; system.notify(message: \"m\", level: \"loud\"); }\n", "must be one of" },
        { "version 1;\njob \"x\" { trigger manual; repeat 0 as i { system.log(message: \"x\"); } }\n", "between 1 and 32" },
        { "version 1;\njob \"x\" { trigger manual; repeat 99 as i { system.log(message: \"x\"); } }\n", "between 1 and 32" },
        { "version 1;\njob \"x\" { trigger manual; repeat 3 as i { set i = 5; } }\n", "read-only" },
        { "version 1;\njob \"x\" { trigger manual; repeat 3 as i { repeat 2 as i { system.log(message: \"x\"); } } }\n", "already defined" },
        { "version 1;\njob \"x\" { trigger manual; run(job: \"other\"); }\n", "job calls" },
        { "version 1;\njob \"x\" { trigger manual; set v = json_get(\"a\", i); }\n", "literal string" },
        { "version 1;\njob \"x\" { trigger daily \"25:00\"; }\n", "HH:MM" },
        { "version 1;\njob \"x\" { trigger manual; system.log(message: \"a\", message: \"b\"); }\n", "duplicate argument" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        jobs_ast_t *ast = pv(bad[i].src);
        if (!has_diag(ast, bad[i].needle)) {
            printf("FAIL validate error %zu: want '%s'\n", i, bad[i].needle);
            for (int d = 0; d < ast->diag_count; d++) printf("   got: %s\n", ast->diag[d].msg);
            fails++;
        }
        checks++;
        jobs_ast_free(ast);
    }

    /* a valid credential reference, and a policy block */
    const char *OK =
        "version 1;\n"
        "job \"Authenticated health\" {\n"
        "    trigger manual;\n"
        "    policy(timeout: 90s, overlap: \"skip\", cooldown: 15m);\n"
        "    http.request(method: \"GET\", url: \"https://example.com/private/health\",\n"
        "                 bearer_token: secret(\"health-token\"), timeout: 10s) as check;\n"
        "    system.log(message: \"HTTP status ${check.status}\");\n"
        "}\n";
    jobs_ast_t *ast = pv(OK);
    CHECK(ast && ast->diag_count == 0);
    if (ast && ast->diag_count) for (int i = 0; i < ast->diag_count; i++) printf("  ok-case: %s\n", ast->diag[i].msg);
    jobs_ast_free(ast);
}

static void test_repeat_json(void)
{
    const char *SRC =
        "version 1;\n"
        "job \"loops\" {\n"
        "    trigger manual;\n"
        "    set body = \"{\\\"temp\\\": 21, \\\"list\\\": [10, 20], \\\"nested\\\": {\\\"ok\\\": true}}\";\n"
        "    repeat 3 as i {\n"
        "        set t = json_get(body, \"temp\");\n"
        "        system.log(message: \"t=${t}\");\n"
        "    }\n"
        "}\n";
    jobs_ast_t *ast = pv(SRC);
    CHECK(ast && ast->diag_count == 0);
    if (ast && ast->diag_count) for (int i = 0; i < ast->diag_count; i++) printf("  loop: %s\n", ast->diag[i].msg);
    if (ast) {
        char out1[1024], out2[1024];
        size_t n1 = jobs_serialize(ast, out1, sizeof(out1));
        CHECK(n1 > 0 && strstr(out1, "repeat 3 as i {") != NULL);
        jobs_ast_t *b = pv(out1);
        CHECK(b && b->diag_count == 0);
        if (b) {
            size_t n2 = jobs_serialize(b, out2, sizeof(out2));
            CHECK(n2 == n1 && strcmp(out1, out2) == 0);
            jobs_ast_free(b);
        }
        jobs_ast_free(ast);
    }
}

static void test_limits(void)
{
    /* source over the cap */
    static char big[JOBS_MAX_SOURCE + 64];
    memset(big, ' ', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    memcpy(big, "version 1;\njob \"x\" { trigger manual; }", 38);
    jobs_ast_t *ast = jobs_parse(big, sizeof(big) - 1, NULL);
    CHECK(has_diag(ast, "too large"));
    jobs_ast_free(ast);
}

static void test_var_limit(void)
{
    char src[4096];
    size_t o = (size_t)snprintf(src, sizeof(src), "version 1;\njob \"x\" { trigger manual;\n");
    for (int i = 0; i < JOBS_MAX_VARS + 4 && o < sizeof(src) - 40; i++)
        o += (size_t)snprintf(src + o, sizeof(src) - o, "set v%d = 1;\n", i);
    o += (size_t)snprintf(src + o, sizeof(src) - o, "}\n");
    jobs_ast_t *ast = pv(src);
    CHECK(has_diag(ast, "too many variables"));
    jobs_ast_free(ast);
}

int main(void)
{
    register_fakes();
    test_examples();
    test_roundtrip();
    test_parse_errors();
    test_validate_errors();
    test_repeat_json();
    test_limits();
    test_var_limit();
    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}
