/* Tests for the assistant's pure core: strict JSON and schema validation, the SSE lexer with events split
 * at every boundary, stream assembly (thinking + signature, parallel tool calls from input_json_delta
 * fragments, invalid input), what each stop reason allows, the request body's shape, the echo rule after
 * a server-side fallback, and history trimming. Called from test_main.c. */
#include "as_conv.h"
#include "as_json.h"
#include "as_oai.h"
#include "as_sse.h"
#include "as_tools.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int *g_checks, *g_fails;
#define CHECK(c) do { (*g_checks)++; if (!(c)) { (*g_fails)++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- helpers ---- */

static aj_t *parse(const char *s) { return aj_parse(s, strlen(s), NULL, 0); }

static void ev(ab_t *b, const char *type, const char *json)
{
    ab_fmt(b, "event: %s\r\ndata: %s\r\n\r\n", type, json);
}

/* feeds `s` to a fresh message in chunks of `chunk` bytes (0: all at once) */
static void run_stream(as_msg_t *m, const char *s, size_t chunk)
{
    as_sse_t sse;
    as_sse_init(&sse);
    as_msg_init(m, NULL);
    size_t n = strlen(s);
    if (!chunk) chunk = n;
    for (size_t i = 0; i < n; i += chunk) as_sse_feed(&sse, s + i, i + chunk > n ? n - i : chunk, as_msg_sse, m);
    as_sse_free(&sse);
}

/* A turn: summarized thinking with a signature, a sentence, then two parallel tool calls whose inputs
 * arrive as fragments. */
static char *turn_tools(void)
{
    ab_t b;
    ab_init(&b);
    ab_puts(&b, ": a comment line the lexer must skip\n");
    ev(&b, "message_start", "{\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
                            "\"model\":\"claude-opus-5\",\"content\":[],\"usage\":{\"input_tokens\":1200,\"output_tokens\":1,"
                            "\"cache_read_input_tokens\":800}}}");
    ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}}");
    ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"Check the battery \"}}");
    ev(&b, "ping", "{\"type\":\"ping\"}");
    ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"and the alerts.\"}}");
    ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"c2lnLTE=\"}}");
    ev(&b, "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":0}");
    ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}");
    ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Looking at \\u00e9lan \"}}");
    ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"the robot.\"}}");
    ev(&b, "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":1}");
    ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":2,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_A\",\"name\":\"robot_overview\",\"input\":{}}}");
    ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\"}}");
    ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\"}}");
    ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"}\"}}");
    ev(&b, "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":2}");
    ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":3,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_B\",\"name\":\"read_topics\",\"input\":{}}}");
    ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":3,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"names\\\": [\\\"/Catalyst/\"}}");
    ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":3,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"Arm/State\\\", \\\"/FMSInfo/OpMode\\\"]\"}}");
    ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":3,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"}\"}}");
    ev(&b, "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":3}");
    ev(&b, "message_delta", "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":212}}");
    ev(&b, "message_stop", "{\"type\":\"message_stop\"}");
    return ab_take(&b);
}

/* ---- tests ---- */

static void json(void)
{
    static const char *good[] = { "{}", "[]", "0", "-0.5e+3", "\"\\u00e9\\ud83d\\ude00\"", "{\"a\":[1,2,{\"b\":null}]}", " true ",
                                  "\"\\/\\b\\f\\n\\r\\t\"" };
    static const char *bad[] = { "", "{\"a\":1,}", "[1,]", "[01]", "{a:1}", "\"x", "\"a\tb\"", "{\"a\":1} x", "tru", "\"\\x\"",
                                 "[1 2]", "{\"a\" 1}", "1.", "-", "{\"a\":\"b\"", "nul" };
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) {
        aj_t *v = parse(good[i]);
        CHECK(v != NULL);
        aj_free(v);
    }
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        char err[64] = "";
        aj_t *v = aj_parse(bad[i], strlen(bad[i]), err, sizeof err);
        CHECK(v == NULL && err[0]);
        aj_free(v);
    }
    /* numbers and strings round-trip unchanged; escapes come back out */
    aj_t *v = parse("{\"n\":1.50,\"e\":2E3,\"s\":\"a\\\"b\\u0001\\u00e9\",\"z\":[true,false,null]}");
    char *s = aj_dump(v);
    CHECK(s && !strcmp(s, "{\"n\":1.50,\"e\":2E3,\"s\":\"a\\\"b\\u0001\xc3\xa9\",\"z\":[true,false,null]}"));
    CHECK(fabs(aj_getn(v, "n", 0) - 1.5) < 1e-12);
    free(s);
    aj_free(v);
    /* a lone surrogate becomes U+FFFD rather than invalid UTF-8 */
    v = parse("\"\\ud800x\"");
    CHECK(v && !strcmp(v->s, "\xef\xbf\xbdx"));
    aj_free(v);
    char enc[64];
    as_urlencode("src/main java&q=ü", enc, sizeof enc);
    CHECK(!strcmp(enc, "src/main%20java%26q%3D%C3%BC"));
}

static void schema(void)
{
    const as_tooldef_t *t = as_tool_find("set_tunable");
    CHECK(t != NULL);
    char err[200];
    aj_t *in = parse("{\"key\":\"/Catalyst/Shooter/TargetRPS\",\"value\":70,\"reason\":\"spin-up is slow\"}");
    CHECK(as_tool_check(t, in, err, sizeof err));
    aj_free(in);
    in = parse("{\"key\":\"/Catalyst/X1/Turret/UseV8\",\"value\":false,\"reason\":\"a\"}");
    CHECK(as_tool_check(t, in, err, sizeof err));
    aj_free(in);
    in = parse("{\"key\":\"k\",\"value\":\"70\",\"reason\":\"r\"}");
    CHECK(!as_tool_check(t, in, err, sizeof err) && strstr(err, "value"));
    aj_free(in);
    in = parse("{\"key\":\"k\",\"value\":1}");
    CHECK(!as_tool_check(t, in, err, sizeof err) && strstr(err, "reason"));
    aj_free(in);
    in = parse("{\"key\":\"k\",\"value\":1,\"reason\":\"r\",\"force\":true}");
    CHECK(!as_tool_check(t, in, err, sizeof err) && strstr(err, "force"));
    aj_free(in);
    t = as_tool_find("create_work_order");
    in = parse("{\"title\":\"t\",\"body\":\"b\",\"kind\":\"feature\",\"priority\":\"high\"}");
    CHECK(!as_tool_check(t, in, err, sizeof err) && strstr(err, "kind"));
    aj_free(in);
    t = as_tool_find("revert_snapshot");
    in = parse("{\"id\":2.5,\"reason\":\"r\"}");
    CHECK(!as_tool_check(t, in, err, sizeof err));
    aj_free(in);
    in = parse("{\"id\":0,\"reason\":\"r\"}");
    CHECK(!as_tool_check(t, in, err, sizeof err));
    aj_free(in);
    t = as_tool_find("propose_patch");
    in = parse("{\"title\":\"x\",\"summary\":\"y\",\"edits\":[{\"path\":\"a.java\",\"old\":\"1\"}]}");
    CHECK(!as_tool_check(t, in, err, sizeof err) && strstr(err, "edits[0]"));
    aj_free(in);
    /* every tool's schema parses, and the tools array says eager_input_streaming for each */
    ab_t b;
    ab_init(&b);
    as_tools_json(&b);
    aj_t *tools = parse(b.p);
    CHECK(tools && tools->n == AS_NTOOLS);
    for (int i = 0; tools && i < tools->n; i++) {
        CHECK(aj_is(aj_get(tools->kid[i], "eager_input_streaming"), AJ_TRUE));
        CHECK(aj_is(aj_get(tools->kid[i], "input_schema"), AJ_OBJ));
    }
    aj_free(tools);
    ab_free(&b);
}

typedef struct {
    ab_t log;
    int n;
} evlog_t;

static void log_event(void *user, const char *event, const char *data, size_t len)
{
    evlog_t *l = user;
    ab_fmt(&l->log, "<%s|%.*s>", event, (int)len, data);
    l->n++;
}

static void sse(void)
{
    const char *stream = ": hello\r\n"
                         "event: one\r\ndata: a\r\ndata:  b\r\n\r\n"
                         "data: plain\n\n"
                         "event: three\rdata: {\"x\":1}\r\r"
                         "id: 7\nretry: 100\nevent: four\ndata\n\n"
                         "event: dropped\n\n"
                         "data: last\r\n\r\n";
    evlog_t whole = { 0 };
    ab_init(&whole.log);
    as_sse_t s;
    as_sse_init(&s);
    as_sse_feed(&s, stream, strlen(stream), log_event, &whole);
    as_sse_free(&s);
    CHECK(whole.n == 5);
    CHECK(whole.log.p && !strcmp(whole.log.p, "<one|a\n b><message|plain><three|{\"x\":1}><four|><message|last>"));
    /* the same events whatever the chunking, including a split between CR and LF */
    size_t n = strlen(stream);
    for (size_t chunk = 1; chunk <= 9; chunk++) {
        for (size_t off = 0; off < chunk; off++) {
            evlog_t l = { 0 };
            ab_init(&l.log);
            as_sse_init(&s);
            as_sse_feed(&s, stream, off, log_event, &l);
            for (size_t i = off; i < n; i += chunk) as_sse_feed(&s, stream + i, i + chunk > n ? n - i : chunk, log_event, &l);
            as_sse_free(&s);
            CHECK(l.log.p && !strcmp(l.log.p, whole.log.p));
            ab_free(&l.log);
        }
    }
    ab_free(&whole.log);
}

static void assembly(void)
{
    char *s = turn_tools();
    /* whole, byte by byte, and in odd chunks: the same message */
    size_t chunks[] = { 0, 1, 3, 7, 64 };
    for (size_t k = 0; k < sizeof chunks / sizeof *chunks; k++) {
        as_msg_t m;
        run_stream(&m, s, chunks[k]);
        CHECK(m.started && m.done && !m.error);
        CHECK(!strcmp(m.model, "claude-opus-5") && m.input_tokens == 1200 && m.output_tokens == 212 && m.cache_read == 800);
        CHECK(m.n == 4);
        CHECK(!strcmp(aj_gets(m.b[0].block, "thinking"), "Check the battery and the alerts."));
        CHECK(!strcmp(aj_gets(m.b[0].block, "signature"), "c2lnLTE="));
        CHECK(!strcmp(aj_gets(m.b[1].block, "text"), "Looking at \xc3\xa9lan the robot."));
        CHECK(as_conv_next(&m) == AS_NEXT_TOOLS);
        as_call_t calls[4];
        int nc = as_conv_calls(&m, calls, 4);
        CHECK(nc == 2);
        CHECK(nc == 2 && !strcmp(calls[0].name, "robot_overview") && calls[0].input && calls[0].input->n == 0);
        CHECK(nc == 2 && !strcmp(calls[1].id, "toolu_B") && calls[1].input &&
              aj_get(calls[1].input, "names")->n == 2 &&
              !strcmp(aj_get(calls[1].input, "names")->kid[0]->s, "/Catalyst/Arm/State"));
        as_msg_free(&m);
    }
    free(s);

    /* input that isn't valid JSON: no input, the raw text kept, an INVALID_JSON result for it */
    ab_t b;
    ab_init(&b);
    ev(&b, "message_start", "{\"type\":\"message_start\",\"message\":{\"model\":\"claude-opus-5\",\"usage\":{\"input_tokens\":5}}}");
    ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_X\",\"name\":\"set_tunable\",\"input\":{}}}");
    ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"key\\\": \\\"/a\\\", \\\"value\\\": 7,}\"}}");
    ev(&b, "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":0}");
    ev(&b, "message_delta", "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":9}}");
    ev(&b, "message_stop", "{\"type\":\"message_stop\"}");
    as_msg_t m;
    run_stream(&m, b.p, 5);
    CHECK(as_conv_next(&m) == AS_NEXT_TOOLS);
    as_call_t c[2];
    CHECK(as_conv_calls(&m, c, 2) == 1 && c[0].input == NULL && !strcmp(c[0].raw, "{\"key\": \"/a\", \"value\": 7,}"));
    char *res = as_conv_invalid_json(c[0].raw);
    aj_t *rv = parse(res);
    CHECK(rv && !strcmp(aj_gets(rv, "INVALID_JSON"), "{\"key\": \"/a\", \"value\": 7,}"));
    aj_free(rv);
    /* it goes back in the history with an empty input, and its result is an error */
    as_hist_t h;
    as_hist_init(&h);
    as_hist_user(&h, NULL, "set it");
    as_hist_assistant(&h, as_conv_echo(&m));
    as_result_t r = { c[0].id, res, true };
    as_hist_results(&h, &r, 1);
    CHECK(aj_get(aj_at(h.m[1].content, 0), "input")->n == 0);
    const aj_t *tr = aj_at(h.m[2].content, 0);
    CHECK(!strcmp(aj_gets(tr, "type"), "tool_result") && !strcmp(aj_gets(tr, "tool_use_id"), "toolu_X") &&
          aj_is(aj_get(tr, "is_error"), AJ_TRUE) && strstr(aj_gets(tr, "content"), "INVALID_JSON"));
    as_hist_free(&h);
    free(res);
    as_msg_free(&m);
    ab_free(&b);

    /* an error event */
    ab_init(&b);
    ev(&b, "message_start", "{\"type\":\"message_start\",\"message\":{\"model\":\"claude-opus-5\",\"usage\":{}}}");
    ev(&b, "error", "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}");
    run_stream(&m, b.p, 0);
    CHECK(m.error && !strcmp(m.error_type, "overloaded_error") && as_conv_next(&m) == AS_NEXT_ERROR);
    as_msg_free(&m);
    ab_free(&b);

    /* a stream that stops before message_stop is not an answer */
    ab_init(&b);
    ev(&b, "message_start", "{\"type\":\"message_start\",\"message\":{\"model\":\"claude-opus-5\",\"usage\":{}}}");
    ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}");
    run_stream(&m, b.p, 0);
    CHECK(as_conv_next(&m) == AS_NEXT_ERROR);
    as_msg_free(&m);
    ab_free(&b);
}

/* one message ending with `stop` after a text block and (optionally) a tool call cut short */
static char *ending(const char *stop, bool tool, const char *details)
{
    ab_t b;
    ab_init(&b);
    ev(&b, "message_start", "{\"type\":\"message_start\",\"message\":{\"model\":\"claude-opus-5\",\"usage\":{\"input_tokens\":3}}}");
    ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}");
    ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Sure, \"}}");
    ev(&b, "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":0}");
    if (tool) {
        ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_T\",\"name\":\"set_tunable\",\"input\":{}}}");
        ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"key\\\": \\\"/Catalyst/Shooter/TargetRPS\\\"\"}}");
        ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\", \\\"value\\\": 70, \\\"reason\\\": \\\"x\\\"}\"}}");
        ev(&b, "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":1}");
    }
    char md[512];
    snprintf(md, sizeof md, "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"%s\"%s%s},\"usage\":{\"output_tokens\":4}}", stop,
             details ? ",\"stop_details\":" : "", details ? details : "");
    ev(&b, "message_delta", md);
    ev(&b, "message_stop", "{\"type\":\"message_stop\"}");
    return ab_take(&b);
}

static void stop_reasons(void)
{
    struct {
        const char *stop;
        bool tool;
        as_next_t want;
    } cases[] = {
        { "end_turn", false, AS_NEXT_DONE },         { "tool_use", true, AS_NEXT_TOOLS },
        { "refusal", true, AS_NEXT_REFUSED },        { "refusal", false, AS_NEXT_REFUSED },
        { "max_tokens", true, AS_NEXT_TRUNCATED },   { "max_tokens", false, AS_NEXT_DONE },
        { "pause_turn", false, AS_NEXT_CONTINUE },   { "stop_sequence", false, AS_NEXT_DONE },
        { "model_context_window_exceeded", false, AS_NEXT_DONE },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char *s = ending(cases[i].stop, cases[i].tool,
                         !strcmp(cases[i].stop, "refusal") ? "{\"type\":\"refusal\",\"category\":\"cyber\",\"explanation\":\"x\"}" : NULL);
        as_msg_t m;
        run_stream(&m, s, 11);
        CHECK(as_conv_next(&m) == cases[i].want);
        if (!strcmp(cases[i].stop, "refusal")) CHECK(!strcmp(aj_gets(m.stop_details, "category"), "cyber"));
        as_msg_free(&m);
        free(s);
    }
}

static void request_shape(void)
{
    char *s = turn_tools();
    as_msg_t m;
    run_stream(&m, s, 13);
    free(s);
    as_hist_t h;
    as_hist_init(&h);
    as_hist_user(&h, "[Catalyst Tab context. Team 5805. robot connected]", "Why is the arm slow?");
    as_hist_assistant(&h, as_conv_echo(&m));
    as_call_t calls[4];
    int nc = as_conv_calls(&m, calls, 4);
    as_result_t res[2] = { { calls[0].id, "{\"battery_v\":12.4}", false }, { calls[1].id, "{\"error\":\"x\"}", true } };
    as_hist_results(&h, res, nc);

    ab_t tools;
    ab_init(&tools);
    as_tools_json(&tools);
    as_req_t q = { .model = NULL, .max_tokens = AS_MAX_TOKENS, .effort = "medium", .system = "You are a pit technician.", .tools = tools.p };
    ab_t body;
    ab_init(&body);
    as_conv_request(&h, &q, &body);
    aj_t *d = parse(body.p);
    CHECK(d != NULL);
    CHECK(!strcmp(aj_gets(d, "model"), "claude-opus-5"));
    CHECK(aj_getn(d, "max_tokens", 0) == 32000);
    CHECK(aj_is(aj_get(d, "stream"), AJ_TRUE));
    CHECK(!strcmp(aj_gets(aj_get(d, "thinking"), "type"), "adaptive") && !strcmp(aj_gets(aj_get(d, "thinking"), "display"), "summarized"));
    CHECK(!aj_get(aj_get(d, "thinking"), "budget_tokens"));
    CHECK(!strcmp(aj_gets(aj_get(d, "output_config"), "effort"), "medium"));
    CHECK(!strcmp(aj_gets(d, "fallbacks"), "default"));
    CHECK(!aj_get(d, "temperature") && !aj_get(d, "top_p") && !aj_get(d, "top_k"));
    const aj_t *sys = aj_get(d, "system");
    CHECK(sys && sys->n == 1 && !strcmp(aj_gets(aj_get(sys->kid[0], "cache_control"), "type"), "ephemeral"));
    const aj_t *tl = aj_get(d, "tools");
    for (int i = 0; tl && i < tl->n; i++) CHECK(aj_is(aj_get(tl->kid[i], "eager_input_streaming"), AJ_TRUE));
    const aj_t *msgs = aj_get(d, "messages");
    CHECK(msgs && msgs->n == 3);
    /* the first message carries the context note, then the question */
    const aj_t *m0 = aj_get(msgs->kid[0], "content");
    CHECK(!strcmp(aj_gets(msgs->kid[0], "role"), "user") && m0->n == 2 && strstr(aj_gets(m0->kid[0], "text"), "Team 5805"));
    /* the assistant's blocks come back unchanged: thinking with its signature, text, both calls */
    const aj_t *a = aj_get(msgs->kid[1], "content");
    CHECK(!strcmp(aj_gets(msgs->kid[1], "role"), "assistant") && a->n == 4);
    CHECK(!strcmp(aj_gets(a->kid[0], "type"), "thinking") && !strcmp(aj_gets(a->kid[0], "signature"), "c2lnLTE=") &&
          !strcmp(aj_gets(a->kid[0], "thinking"), "Check the battery and the alerts."));
    CHECK(!strcmp(aj_gets(a->kid[3], "id"), "toolu_B") && aj_get(aj_get(a->kid[3], "input"), "names"));
    CHECK(!aj_get(a->kid[0], "cache_control"));
    /* one user message with both results, ids matching, the failure flagged, the last block cached */
    const aj_t *u = aj_get(msgs->kid[2], "content");
    CHECK(!strcmp(aj_gets(msgs->kid[2], "role"), "user") && u->n == 2);
    CHECK(!strcmp(aj_gets(u->kid[0], "tool_use_id"), "toolu_A") && !aj_get(u->kid[0], "is_error"));
    CHECK(!strcmp(aj_gets(u->kid[1], "tool_use_id"), "toolu_B") && aj_is(aj_get(u->kid[1], "is_error"), AJ_TRUE));
    CHECK(!aj_get(u->kid[0], "cache_control") && !strcmp(aj_gets(aj_get(u->kid[1], "cache_control"), "type"), "ephemeral"));
    /* the model is whatever the settings say */
    q.model = "claude-opus-5-5";
    as_conv_request(&h, &q, &body);
    aj_free(d);
    d = parse(body.p);
    CHECK(d && !strcmp(aj_gets(d, "model"), "claude-opus-5-5"));
    aj_free(d);

    char hdr[400];
    as_conv_headers("sk-test", hdr, sizeof hdr);
    CHECK(strstr(hdr, "x-api-key: sk-test\r\n") && strstr(hdr, "anthropic-version: 2023-06-01\r\n") &&
          strstr(hdr, "anthropic-beta: server-side-fallback-2026-07-01\r\n") && strstr(hdr, "content-type: application/json\r\n"));

    ab_free(&body);
    ab_free(&tools);
    as_hist_free(&h);
    as_msg_free(&m);
}

static void fallback_echo(void)
{
    /* thinking and a call from the declined model, its partial text, the switch, then the serving model */
    ab_t b;
    ab_init(&b);
    ev(&b, "message_start", "{\"type\":\"message_start\",\"message\":{\"model\":\"claude-opus-4-8\",\"usage\":{\"input_tokens\":9,"
                            "\"iterations\":[{\"type\":\"message\"},{\"type\":\"fallback_message\"}]}}}");
    ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}}");
    ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"old\"}}");
    ev(&b, "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":0}");
    ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_pre\",\"name\":\"get_can\",\"input\":{}}}");
    ev(&b, "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":1}");
    ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":2,\"content_block\":{\"type\":\"text\",\"text\":\"The CAN bus \"}}");
    ev(&b, "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":2}");
    ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":3,\"content_block\":{\"type\":\"fallback\",\"from\":{\"model\":\"claude-opus-5\"},\"to\":{\"model\":\"claude-opus-4-8\"}}}");
    ev(&b, "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":3}");
    ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":4,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}}");
    ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":4,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"new\"}}");
    ev(&b, "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":4}");
    ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":5,\"content_block\":{\"type\":\"text\",\"text\":\"looks busy.\"}}");
    ev(&b, "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":5}");
    ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":6,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_post\",\"name\":\"get_power\",\"input\":{}}}");
    ev(&b, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":6,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{}\"}}");
    ev(&b, "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":6}");
    ev(&b, "message_delta", "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":30}}");
    ev(&b, "message_stop", "{\"type\":\"message_stop\"}");
    as_msg_t m;
    run_stream(&m, b.p, 17);
    CHECK(m.fallback && !strcmp(m.model, "claude-opus-4-8"));
    aj_t *e = as_conv_echo(&m);
    /* text before the boundary stays; the declined model's thinking and call go; so does the marker */
    CHECK(e && e->n == 4);
    CHECK(e->n == 4 && !strcmp(aj_gets(e->kid[0], "text"), "The CAN bus "));
    CHECK(e->n == 4 && !strcmp(aj_gets(e->kid[1], "signature"), "new"));
    CHECK(e->n == 4 && !strcmp(aj_gets(e->kid[2], "text"), "looks busy."));
    CHECK(e->n == 4 && !strcmp(aj_gets(e->kid[3], "id"), "toolu_post"));
    for (int i = 0; e && i < e->n; i++) CHECK(strcmp(aj_gets(e->kid[i], "type"), "fallback") != 0);
    as_call_t c[4];
    CHECK(as_conv_calls(&m, c, 4) == 1 && !strcmp(c[0].id, "toolu_post"));
    aj_free(e);
    as_msg_free(&m);
    ab_free(&b);

    /* a fallback before any output (pre-output decline): the block comes first and nothing is dropped */
    ab_init(&b);
    ev(&b, "message_start", "{\"type\":\"message_start\",\"message\":{\"model\":\"claude-opus-4-8\",\"usage\":{}}}");
    ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"fallback\",\"from\":{\"model\":\"claude-opus-5\"},\"to\":{\"model\":\"claude-opus-4-8\"}}}");
    ev(&b, "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":0}");
    ev(&b, "content_block_start", "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"Hi\"}}");
    ev(&b, "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":1}");
    ev(&b, "message_delta", "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":1}}");
    ev(&b, "message_stop", "{\"type\":\"message_stop\"}");
    run_stream(&m, b.p, 0);
    e = as_conv_echo(&m);
    CHECK(m.fallback && e && e->n == 1 && !strcmp(aj_gets(e->kid[0], "text"), "Hi"));
    aj_free(e);
    as_msg_free(&m);
    ab_free(&b);
}

/* an exchange: question, then `rounds` of (thinking + tool call → a big result), then an answer */
static void add_exchange(as_hist_t *h, int k, int rounds, size_t result_bytes)
{
    char q[64], id[32];
    snprintf(q, sizeof q, "question %d", k);
    as_hist_user(h, k == 0 ? "[context note: team 5805]" : NULL, q);
    char *big = malloc(result_bytes + 1);
    memset(big, 'x', result_bytes);
    big[result_bytes] = 0;
    for (int r = 0; r < rounds; r++) {
        snprintf(id, sizeof id, "toolu_%d_%d", k, r);
        aj_t *c = aj_new(AJ_ARR);
        aj_t *t = parse("{\"type\":\"thinking\",\"thinking\":\"hmm\",\"signature\":\"sig\"}");
        aj_push(c, t);
        aj_t *u = parse("{\"type\":\"tool_use\",\"name\":\"get_can\",\"input\":{}}");
        aj_set(u, "id", aj_new_str(id));
        aj_push(c, u);
        as_hist_assistant(h, c);
        as_result_t res = { id, big, false };
        as_hist_results(h, &res, 1);
    }
    aj_t *c = aj_new(AJ_ARR);
    aj_push(c, parse("{\"type\":\"thinking\",\"thinking\":\"done\",\"signature\":\"sig2\"}"));
    aj_push(c, parse("{\"type\":\"text\",\"text\":\"answer\"}"));
    as_hist_assistant(h, c);
    free(big);
}

static bool pairs_intact(const as_hist_t *h)
{
    /* alternating roles from a user message, and every tool_use answered in the very next message */
    if (!h->n || !h->m[0].user) return false;
    for (int i = 0; i < h->n; i++) {
        if (i && h->m[i].user == h->m[i - 1].user) return false;
        if (h->m[i].user) continue;
        const aj_t *c = h->m[i].content;
        for (int k = 0; k < c->n; k++) {
            if (strcmp(aj_gets(c->kid[k], "type"), "tool_use") != 0) continue;
            const char *id = aj_gets(c->kid[k], "id");
            if (i + 1 >= h->n) return false;
            bool found = false;
            const aj_t *u = h->m[i + 1].content;
            for (int j = 0; j < u->n && !found; j++) {
                const char *tid = aj_gets(u->kid[j], "tool_use_id");
                found = tid && !strcmp(tid, id);
            }
            if (!found) return false;
        }
    }
    return true;
}

static void trimming(void)
{
    as_hist_t h;
    as_hist_init(&h);
    for (int k = 0; k < 12; k++) add_exchange(&h, k, 3, 20000);
    /* the exchange in progress: a question and one round, no answer yet */
    as_hist_user(&h, NULL, "question now");
    aj_t *c = aj_new(AJ_ARR);
    aj_push(c, parse("{\"type\":\"thinking\",\"thinking\":\"current\",\"signature\":\"cur\"}"));
    aj_push(c, parse("{\"type\":\"tool_use\",\"id\":\"toolu_now\",\"name\":\"get_power\",\"input\":{}}"));
    as_hist_assistant(&h, c);
    as_result_t res = { "toolu_now", "{}", false };
    as_hist_results(&h, &res, 1);

    as_req_t q = { .max_tokens = AS_MAX_TOKENS, .effort = "medium", .system = "s", .tools = "[]" };
    ab_t body;
    ab_init(&body);
    as_conv_request(&h, &q, &body);
    CHECK(body.n > 700000);
    int before = h.n;
    int dropped = as_conv_trim(&h, &q, AS_TRIM_BYTES);
    as_conv_request(&h, &q, &body);
    CHECK(dropped > 0 && h.n < before);
    CHECK(body.n <= AS_TRIM_BYTES);
    CHECK(pairs_intact(&h));
    aj_t *pd = body.p ? parse(body.p) : NULL;
    CHECK(pd != NULL);
    aj_free(pd);
    /* the first message still has the context note, followed by that exchange's question */
    CHECK(!strcmp(aj_gets(aj_at(h.m[0].content, 0), "text"), "[context note: team 5805]"));
    char want[32];
    snprintf(want, sizeof want, "question %d", dropped);
    CHECK(!strcmp(aj_gets(aj_at(h.m[0].content, 1), "text"), want));
    /* thinking is gone before the current exchange, and kept in it */
    int thinking_before = 0, cur = -1;
    for (int i = 0; i < h.n; i++) if (h.m[i].exchange) cur = i;
    for (int i = 0; i < cur; i++)
        for (int k = 0; k < h.m[i].content->n; k++) thinking_before += !strcmp(aj_gets(h.m[i].content->kid[k], "type"), "thinking");
    CHECK(thinking_before == 0);
    CHECK(!strcmp(aj_gets(aj_at(h.m[cur + 1].content, 0), "signature"), "cur"));
    /* the exchange in progress is never dropped, even when it alone is over the limit */
    int n = h.n;
    as_conv_trim(&h, &q, 1000);
    CHECK(h.m[0].exchange && !strcmp(aj_gets(aj_at(h.m[0].content, 1), "text"), "question now") && h.n == 3 && n > 3);
    CHECK(!strcmp(aj_gets(aj_at(h.m[0].content, 0), "text"), "[context note: team 5805]"));
    CHECK(pairs_intact(&h));
    ab_free(&body);

    /* a refused question leaves the history as it was before it */
    as_hist_user(&h, NULL, "refused one");
    as_hist_rollback(&h);
    CHECK(h.n == 3);
    as_hist_free(&h);
}

/* ---- OpenAI: Chat Completions chunks become the same message; the history becomes its request ---- */

static void chunk(ab_t *b, const char *json) { ab_fmt(b, "data: %s\n\n", json); }

static void run_oai(as_msg_t *m, const char *s, size_t step)
{
    as_sse_t sse;
    as_oai_t o;
    as_sse_init(&sse);
    as_msg_init(m, NULL);
    as_oai_init(&o, m);
    size_t n = strlen(s);
    if (!step) step = n;
    for (size_t i = 0; i < n; i += step) as_sse_feed(&sse, s + i, i + step > n ? n - i : step, as_oai_sse, &o);
    as_oai_end(&o);
    as_sse_free(&sse);
}

static void openai(void)
{
    /* text, then two parallel tool calls whose arguments arrive in pieces, then usage and [DONE] */
    ab_t b;
    ab_init(&b);
    chunk(&b, "{\"id\":\"chatcmpl-1\",\"object\":\"chat.completion.chunk\",\"model\":\"gpt-4o-mini-2024-07-18\","
              "\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\",\"refusal\":null},\"finish_reason\":null}],\"usage\":null}");
    chunk(&b, "{\"id\":\"chatcmpl-1\",\"model\":\"gpt-4o-mini-2024-07-18\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Checking \\u00e9\"},\"finish_reason\":null}]}");
    chunk(&b, "{\"id\":\"chatcmpl-1\",\"model\":\"gpt-4o-mini-2024-07-18\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"lan.\"},\"finish_reason\":null}]}");
    chunk(&b, "{\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_A\",\"type\":\"function\","
              "\"function\":{\"name\":\"robot_overview\",\"arguments\":\"\"}}]},\"finish_reason\":null}]}");
    chunk(&b, "{\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{}\"}}]},\"finish_reason\":null}]}");
    chunk(&b, "{\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":\"call_B\",\"type\":\"function\","
              "\"function\":{\"name\":\"read_topics\",\"arguments\":\"{\\\"names\\\": [\\\"/Catalyst/\"}}]},\"finish_reason\":null}]}");
    chunk(&b, "{\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":1,\"function\":{\"arguments\":\"Arm/State\\\"]}\"}}]},\"finish_reason\":null}]}");
    chunk(&b, "{\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}");
    chunk(&b, "{\"id\":\"chatcmpl-1\",\"choices\":[],\"usage\":{\"prompt_tokens\":1500,\"completion_tokens\":60,"
              "\"prompt_tokens_details\":{\"cached_tokens\":1024}}}");
    ab_puts(&b, "data: [DONE]\n\n");
    char *s = ab_take(&b);
    size_t steps[] = { 0, 1, 7 };
    for (int k = 0; k < 3; k++) {
        as_msg_t m;
        run_oai(&m, s, steps[k]);
        CHECK(m.started && m.done && !m.error);
        CHECK(!strcmp(m.model, "gpt-4o-mini-2024-07-18"));
        CHECK(!strcmp(m.stop_reason, "tool_use"));
        CHECK(m.n == 3);
        CHECK(!strcmp(aj_gets(m.b[0].block, "text"), "Checking \xc3\xa9lan."));
        as_call_t c[4];
        int nc = as_conv_calls(&m, c, 4);
        CHECK(nc == 2);
        CHECK(nc == 2 && !strcmp(c[0].id, "call_A") && !strcmp(c[0].name, "robot_overview") && c[0].input && c[0].input->n == 0);
        CHECK(nc == 2 && !strcmp(c[1].name, "read_topics") && c[1].input && !strcmp(aj_at(aj_get(c[1].input, "names"), 0)->s, "/Catalyst/Arm/State"));
        CHECK(as_conv_next(&m) == AS_NEXT_TOOLS);
        CHECK(m.input_tokens == 1500 - 1024 && m.cache_read == 1024 && m.output_tokens == 60);
        as_msg_free(&m);
    }
    free(s);

    /* a plain answer ends the turn; a length cut with a call is truncated; no finish_reason is cut off */
    ab_init(&b);
    chunk(&b, "{\"id\":\"c2\",\"model\":\"gpt-4o-mini\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"All good.\"},\"finish_reason\":\"stop\"}]}");
    ab_puts(&b, "data: [DONE]\n\n");
    s = ab_take(&b);
    as_msg_t m;
    run_oai(&m, s, 0);
    CHECK(m.done && as_conv_next(&m) == AS_NEXT_DONE && !strcmp(m.stop_reason, "end_turn"));
    as_msg_free(&m);
    free(s);

    ab_init(&b);
    chunk(&b, "{\"id\":\"c3\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_Z\",\"function\":{\"name\":\"get_can\",\"arguments\":\"{\\\"x\"}}]},\"finish_reason\":\"length\"}]}");
    s = ab_take(&b); /* and the body just ends: finish_reason came, [DONE] didn't */
    run_oai(&m, s, 0);
    CHECK(m.done && as_conv_next(&m) == AS_NEXT_TRUNCATED);
    CHECK(m.n == 1 && m.b[0].bad_input);
    as_msg_free(&m);
    free(s);

    ab_init(&b);
    chunk(&b, "{\"id\":\"c4\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Half an ans\"},\"finish_reason\":null}]}");
    s = ab_take(&b);
    run_oai(&m, s, 0);
    CHECK(!m.done && as_conv_next(&m) == AS_NEXT_ERROR);
    as_msg_free(&m);
    free(s);

    ab_init(&b);
    chunk(&b, "{\"error\":{\"message\":\"The server had an error\",\"type\":\"server_error\",\"code\":null}}");
    s = ab_take(&b);
    run_oai(&m, s, 0);
    CHECK(m.error && !strcmp(m.error_type, "server_error") && as_conv_next(&m) == AS_NEXT_ERROR);
    as_msg_free(&m);
    free(s);

    ab_init(&b);
    chunk(&b, "{\"id\":\"c5\",\"choices\":[{\"index\":0,\"delta\":{\"refusal\":\"I can't help with that.\"},\"finish_reason\":\"stop\"}]}");
    ab_puts(&b, "data: [DONE]\n\n");
    s = ab_take(&b);
    run_oai(&m, s, 0);
    CHECK(as_conv_next(&m) == AS_NEXT_REFUSED);
    as_msg_free(&m);
    free(s);

    /* the request: system first, tools as functions, a tool turn as tool_calls + role "tool" messages,
     * thinking left out, errors said in the text */
    as_hist_t h;
    as_hist_init(&h);
    as_hist_user(&h, "[context note]", "why did we brown out?");
    aj_t *a = parse("[{\"type\":\"thinking\",\"thinking\":\"hm\",\"signature\":\"x\"},{\"type\":\"text\",\"text\":\"Looking.\"},"
                    "{\"type\":\"tool_use\",\"id\":\"call_A\",\"name\":\"get_power\",\"input\":{}},"
                    "{\"type\":\"tool_use\",\"id\":\"call_B\",\"name\":\"read_topics\",\"input\":{\"names\":[\"/a\"]}}]");
    as_hist_assistant(&h, a);
    as_result_t res[2] = { { "call_A", "{\"battery\":11.2}", false }, { "call_B", "{\"error\":\"no such topic\"}", true } };
    as_hist_results(&h, res, 2);
    ab_t tools;
    ab_init(&tools);
    as_tools_json(&tools);
    as_req_t q = { .model = "gpt-4o-mini", .system = "You are the pit technician.", .tools = tools.p };
    ab_t body;
    ab_init(&body);
    as_oai_request(&h, &q, &body);
    aj_t *d = parse(body.p);
    CHECK(d != NULL);
    CHECK(!strcmp(aj_gets(d, "model"), "gpt-4o-mini"));
    CHECK(aj_is(aj_get(d, "stream"), AJ_TRUE));
    CHECK(aj_is(aj_get(aj_get(d, "stream_options"), "include_usage"), AJ_TRUE));
    CHECK(!aj_get(d, "thinking") && !aj_get(d, "max_tokens"));
    const aj_t *tl = aj_get(d, "tools");
    CHECK(tl && tl->n == AS_NTOOLS);
    CHECK(!strcmp(aj_gets(aj_at(tl, 0), "type"), "function"));
    CHECK(aj_get(aj_get(aj_at(tl, 0), "function"), "parameters") != NULL);
    const aj_t *ms = aj_get(d, "messages");
    CHECK(ms && ms->n == 5);
    CHECK(!strcmp(aj_gets(aj_at(ms, 0), "role"), "system"));
    CHECK(!strcmp(aj_gets(aj_at(ms, 1), "role"), "user"));
    CHECK(!strcmp(aj_gets(aj_at(ms, 1), "content"), "[context note]\n\nwhy did we brown out?"));
    const aj_t *am = aj_at(ms, 2);
    CHECK(!strcmp(aj_gets(am, "role"), "assistant") && !strcmp(aj_gets(am, "content"), "Looking."));
    const aj_t *tc = aj_get(am, "tool_calls");
    CHECK(tc && tc->n == 2);
    CHECK(!strcmp(aj_gets(aj_at(tc, 1), "id"), "call_B"));
    CHECK(!strcmp(aj_gets(aj_get(aj_at(tc, 1), "function"), "arguments"), "{\"names\":[\"/a\"]}"));
    CHECK(!strcmp(aj_gets(aj_at(ms, 3), "role"), "tool") && !strcmp(aj_gets(aj_at(ms, 3), "tool_call_id"), "call_A"));
    CHECK(!strcmp(aj_gets(aj_at(ms, 4), "content"), "ERROR: {\"error\":\"no such topic\"}"));
    CHECK(!strstr(body.p, "thinking"));
    aj_free(d);
    char hdr[400];
    as_oai_headers("sk-test", hdr, sizeof hdr);
    CHECK(strstr(hdr, "Authorization: Bearer sk-test\r\n") != NULL);
    char em[256], code[48];
    as_oai_error("{\"error\":{\"message\":\"You exceeded your current quota\",\"type\":\"insufficient_quota\",\"code\":\"insufficient_quota\"}}",
                 em, sizeof em, code, sizeof code);
    CHECK(!strcmp(code, "insufficient_quota") && strstr(em, "quota"));
    ab_free(&body);
    ab_free(&tools);
    as_hist_free(&h);
}

void test_assist(int *checks, int *fails)
{
    g_checks = checks;
    g_fails = fails;
    json();
    schema();
    sse();
    assembly();
    stop_reasons();
    request_shape();
    fallback_echo();
    trimming();
    openai();
}
