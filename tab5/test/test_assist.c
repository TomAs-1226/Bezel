/* Tests for the assistant's pure core: strict JSON and schema validation, the SSE lexer with events split
 * at every boundary, stream assembly (thinking + signature, parallel tool calls from input_json_delta
 * fragments, invalid input), what each stop reason allows, the request body's shape, the echo rule after
 * a server-side fallback, and history trimming. Called from test_main.c. */
#include "as_conv.h"
#include "as_diag.h"
#include "as_json.h"
#include "as_mem.h"
#include "as_oai.h"
#include "as_prompts.h"
#include "as_sse.h"
#include "as_tools.h"
#include "assist.h"

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
    int tech_tools = 0;
    for (int i = 0; i < AS_NTOOLS; i++) tech_tools += as_tool_for(&AS_TOOLS[i], AS_MEM_ASSIST);
    CHECK(tools && tools->n == tech_tools && tech_tools == AS_NTOOLS - 5); /* all but the companion's persona and her pet */
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
    CHECK(!aj_get(d, "reasoning_effort"));   /* an older model 400s on a parameter it never had */
    CHECK(aj_is(aj_get(d, "stream"), AJ_TRUE));
    CHECK(aj_is(aj_get(aj_get(d, "stream_options"), "include_usage"), AJ_TRUE));
    CHECK(!aj_get(d, "thinking") && !aj_get(d, "max_tokens"));
    const aj_t *tl = aj_get(d, "tools");
    CHECK(tl && tl->n == AS_NTOOLS - 1); /* the technician's: all but the companion's persona */
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

/* the desk: what the UI posts is what the tools read, a copy each way; the tools are there and read-only */
static void desk(void)
{
    CHECK(assist_desk_get(AS_DESK_MATCHES) == NULL);
    assist_desk_post(AS_DESK_MATCHES, "next match: Q12 today 14:05");
    char *t = assist_desk_get(AS_DESK_MATCHES);
    CHECK(t && !strcmp(t, "next match: Q12 today 14:05"));
    free(t);
    assist_desk_post(AS_DESK_MATCHES, "next match: Q13 today 14:20");
    t = assist_desk_get(AS_DESK_MATCHES);
    CHECK(t && strstr(t, "Q13"));
    free(t);
    assist_desk_post(AS_DESK_MATCHES, "");
    CHECK(assist_desk_get(AS_DESK_MATCHES) == NULL);
    CHECK(assist_desk_get(AS_DESK_BATTERIES) == NULL);
    CHECK(assist_desk_get((as_desk_t)AS_DESK_N) == NULL);
    const as_tooldef_t *m = as_tool_find("get_matches"), *b = as_tool_find("get_batteries");
    CHECK(m && m->kind == AS_T_READ);
    CHECK(b && b->kind == AS_T_READ);
}

/* Which reasoning_effort goes with which model, and that the request carries it.
 *
 * This is worth a test because getting it wrong costs the TOOLS, not a little speed: on Chat
 * Completions gpt-6-sol calls functions only at effort "none", gpt-6-astra not at all, and a model
 * older than the parameter refuses the whole request. */
static void oai_effort(void)
{
    CHECK(as_oai_effort("gpt-5", "minimal") && !strcmp(as_oai_effort("gpt-5", "minimal"), "minimal"));
    CHECK(!strcmp(as_oai_effort("gpt-5-mini", "low"), "low"));
    CHECK(!strcmp(as_oai_effort("gpt-5-nano", ""), "medium"));          /* nothing asked for: the default */
    CHECK(!strcmp(as_oai_effort("gpt-5", NULL), "medium"));
    CHECK(!strcmp(as_oai_effort("o3-mini", "high"), "high"));
    CHECK(!strcmp(as_oai_effort("gpt-6-sol", "minimal"), "none"));      /* whatever was wanted */
    CHECK(!strcmp(as_oai_effort("gpt-6-astra", "high"), "none"));
    CHECK(as_oai_effort("gpt-4o-mini", "minimal") == NULL);
    CHECK(as_oai_effort("gpt-4.1", "low") == NULL);
    CHECK(!strcmp(as_oai_effort("", "low"), "low"));                    /* empty: the default model, gpt-5 */
    CHECK(!strcmp(as_oai_effort(NULL, "low"), "low"));

    /* and it reaches the wire */
    as_hist_t h;
    as_hist_init(&h);
    as_hist_user(&h, NULL, "hello");
    as_req_t q = { .model = "gpt-5-mini", .effort = "minimal", .system = "s" };
    ab_t body;
    ab_init(&body);
    as_oai_request(&h, &q, &body);
    aj_t *d = parse(body.p);
    CHECK(d && !strcmp(aj_gets(d, "reasoning_effort"), "minimal"));
    CHECK(d && !aj_get(d, "temperature"));
    aj_free(d);
    ab_free(&body);
    as_hist_free(&h);
}

/* ---- memory: the pure pool (as_mem.c) ---- */

static void memory(void)
{
    as_mem_t *m = malloc(sizeof *m), *m2 = malloc(sizeof *m2);
    as_mem_res_t r;
    if (!m || !m2) {
        CHECK(0);
        free(m);
        free(m2);
        return;
    }
    const int64_t T0 = 1790000000; /* 2026-09-21 */
    as_mem_init(m, AS_MEM_ASSIST);

    /* add, ids from 1, kinds and importance clamped */
    CHECK(as_mem_add(m, "fix", "FL drive wheel set screw backed out at 2026casj Q34; re-torqued and loctited", 3, T0, &r));
    CHECK(r.id == 1 && !r.merged && m->n == 1);
    CHECK(as_mem_add(m, "bogus-kind", "Team prefers the red flashlight in the queue", 9, T0, &r) && r.id == 2);
    CHECK(!strcmp(m->it[1].kind, "fact") && m->it[1].imp == 3);
    CHECK(as_mem_add(m, "procedure", "Always zero the elevator before the first match of the day", 2, T0, &r) && r.id == 3);

    /* the same thing said again updates the old one instead of sitting beside it */
    CHECK(as_mem_add(m, "fix", "At 2026casj Q34 the FL drive wheel set screw backed out; it was re-torqued and loctited",
                     2, T0 + 60, &r));
    CHECK(r.merged && r.id == 1 && m->n == 3 && m->it[0].imp == 3 && m->it[0].updated == T0 + 60);
    /* ... but a different fact about the same wheel is its own memory */
    CHECK(as_mem_add(m, "fault", "FL drive motor ran hot (78 C) in Q40", 2, T0, &r) && !r.merged && r.id == 4);

    /* secrets are refused, and nothing is stored */
    int n = m->n;
    CHECK(!as_mem_add(m, "fact", "the openai key is sk-proj-abcdefghijklmnopqrstuvwxyz0123", 2, T0, &r) && r.id == 0 && r.why[0]);
    CHECK(!as_mem_add(m, "fact", "school wifi password is pit5805!", 2, T0, &r));
    CHECK(!as_mem_add(m, "fact", "HA token: eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9", 2, T0, &r));
    CHECK(!as_mem_add(m, "fact", "use ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZ012345 for the repo", 2, T0, &r));
    CHECK(m->n == n);
    /* ... while ordinary technician talk isn't mistaken for one */
    CHECK(!as_mem_secret("The Link token was wrong after re-pairing the PC; pairing again fixed it"));
    CHECK(!as_mem_secret("CAN id 12 on can_s0 is the FL steer motor; src/main/java/frc/robot/subsystems/Drive.java"));
    CHECK(!as_mem_secret("Battery #7 reads 18 mohm and is the best one we have"));
    CHECK(as_mem_secret("password: hunter2"));
    CHECK(as_mem_secret("key 9f8e7d6c5b4a39281706f5e4d3c2b1a0ff00aa11"));

    /* update and forget by id */
    CHECK(as_mem_update(m, 3, NULL, "Always zero the elevator by the hard stop before the first match of the day", 3, T0 + 5, &r));
    CHECK(m->it[as_mem_find(m, 3)].imp == 3 && strstr(m->it[as_mem_find(m, 3)].text, "hard stop"));
    CHECK(!as_mem_update(m, 99, NULL, "x y z w", 0, T0, &r) && strstr(r.why, "#99"));
    CHECK(as_mem_forget(m, 2) && !as_mem_forget(m, 2) && as_mem_find(m, 2) < 0);

    /* search: by words (with their stems), by kind, and nothing for an unrelated word */
    int idx[10];
    int k = as_mem_search(m, "wheel screw", NULL, idx, 10, T0);
    CHECK(k >= 1 && m->it[idx[0]].id == 1);
    k = as_mem_search(m, "", "fault", idx, 10, T0);
    CHECK(k == 1 && m->it[idx[0]].id == 4);
    CHECK(as_mem_search(m, "pneumatics", NULL, idx, 10, T0) == 0);
    k = as_mem_search(m, NULL, NULL, idx, 10, T0);
    CHECK(k == m->n);

    /* self sections: only this pool's names; secrets refused */
    char why[160];
    CHECK(as_mem_set_self(m, "robot", "Numbers: 28x26 swerve, Kraken drives, turret shooter", T0, why, sizeof why));
    CHECK(!as_mem_set_self(m, "personality", "cheerful", T0, why, sizeof why)); /* the companion's, not this pool's */
    CHECK(!as_mem_set_self(m, "team", "the password is 5805rocks", T0, why, sizeof why));
    CHECK(as_mem_self_index(AS_MEM_COMPANION, "running_jokes") == 3 && as_mem_self_index(AS_MEM_ASSIST, "name") < 0);

    /* JSON round trip, and the pool number stays whose it is */
    size_t len = 0;
    char *js = as_mem_to_json(m, 0, &len);
    CHECK(js && len == strlen(js));
    as_mem_init(m2, AS_MEM_ASSIST);
    CHECK(js && as_mem_from_json(m2, js, len));
    CHECK(m2->n == m->n && m2->next_id == m->next_id && !strcmp(m2->self[0], m->self[0]));
    CHECK(as_mem_find(m2, 3) >= 0 && !strcmp(m2->it[as_mem_find(m2, 3)].text, m->it[as_mem_find(m, 3)].text));
    free(js);
    CHECK(!as_mem_from_json(m2, "[1,2]", 5));
    /* a damaged entry is dropped, the rest kept; a missing next_id is recovered from the ids */
    const char *odd = "{\"memories\":[{\"id\":5,\"text\":\"a real memory here\",\"imp\":7},{\"id\":0,\"text\":\"no id\"},"
                      "{\"id\":6},{\"id\":5,\"text\":\"duplicate id\"}]}";
    as_mem_init(m2, AS_MEM_COMPANION);
    CHECK(as_mem_from_json(m2, odd, strlen(odd)) && m2->n == 1 && m2->it[0].imp == 3 && m2->next_id == 6);

    /* the kv copy fits its budget and keeps the most valuable */
    for (int i = 0; i < 40; i++) {
        char t[120];
        snprintf(t, sizeof t, "Memory number %d about subject%d with a few words so it has some length to it", i, i);
        as_mem_add(m, "fact", t, 1, T0 - 86400 * 60, &r);
    }
    js = as_mem_to_json(m, AS_MEM_KV_MAX, &len);
    CHECK(js && len <= AS_MEM_KV_MAX && strstr(js, "hard stop") && strstr(js, "Numbers: 28x26"));
    free(js);

    /* full: the least valuable memory makes room for one that matters as much, never for a lesser one */
    as_mem_init(m2, AS_MEM_ASSIST);
    for (int i = 0; i < AS_MEM_MAX; i++) {
        char t[80];
        snprintf(t, sizeof t, "Fact %d, subject%d, value%d, detail%d", i, i, i, i);
        as_mem_add(m2, "fact", t, i == 0 ? 1 : 2, T0 + i, &r);
    }
    CHECK(m2->n == AS_MEM_MAX);
    CHECK(as_mem_add(m2, "fix", "A new important fix nobody wrote down before now", 2, T0 + 1000, &r) && r.evicted == 1);
    CHECK(m2->n == AS_MEM_MAX && as_mem_find(m2, 1) < 0);
    CHECK(!as_mem_add(m2, "fact", "A minor fact that shouldn't push out a useful one", 1, T0 + 1000, &r) && r.why[0]);

    /* the digest: self sections, most important first, within budget, and says what's left */
    char *d = malloc(16384);
    size_t dl = as_mem_digest(m, "hard stop elevator", T0, 600, d, 16384);
    CHECK(dl == strlen(d) && strstr(d, "notebook") && strstr(d, "- robot: Numbers"));
    CHECK(strstr(d, "#3 [procedure, important") && strstr(d, "more not shown"));
    const char *first = strstr(d, "- #");
    CHECK(first && !strncmp(first, "- #3", 4)); /* the question's subject comes first among equals */
    as_mem_init(m2, AS_MEM_COMPANION);
    as_mem_digest(m2, NULL, T0, 600, d, 16384);
    CHECK(strstr(d, "Who you are") && strstr(d, "- name: (empty so far)") && strstr(d, "none yet"));
    free(d);

    /* the typography a model writes and the faces don't carry is folded on the way in */
    as_mem_init(m2, AS_MEM_ASSIST);
    CHECK(as_mem_add(m2, "fix", "The wheel\xe2\x80\x99s \xe2\x80\x9cset screw\xe2\x80\x9d \xe2\x80\x93 re-torqued\xc2\xa0today", 2, T0, &r));
    CHECK(!strcmp(m2->it[0].text, "The wheel's \"set screw\" - re-torqued today"));
    char fold[64] = "a \xe2\x80\x94 b \xe2\x80\xa6 \xe2\x80\xa2 c";
    as_fold_text(fold);
    CHECK(!strcmp(fold, "a \xe2\x80\x94 b \xe2\x80\xa6 \xc2\xb7 c")); /* the em dash and ellipsis stay */

    /* clear keeps ids growing: an old id in a transcript never names a new memory */
    int next = m->next_id;
    as_mem_clear(m);
    CHECK(m->n == 0 && !m->self[0][0] && m->next_id == next);
    free(m);
    free(m2);
}

/* ---- diagnostics: triage and swerve tracking (as_diag.c) ---- */

static cat_robot_t *robot_ok(void)
{
    cat_robot_t *r = calloc(1, sizeof *r);
    r->connected = true;
    r->have_mode = r->ds = true;
    r->have_battery = true;
    r->battery_v = 12.7;
    r->brownout_v = r->predicted_v = r->total_current = NAN;
    r->loop_last_ms = r->loop_avg_ms = r->loop_max_ms = NAN;
    r->match_time = r->heading_deg = NAN;
    r->tag_seen = -1;
    snprintf(r->name, sizeof r->name, "Numbers");
    return r;
}

static void diagnostics(void)
{
    as_finding_t f[40];
    cat_robot_t *r = robot_ok();
    int n = as_diag_triage(r, f, 40);
    CHECK(n == 0);

    /* not connected: one fault, nothing else claimed */
    r->connected = false;
    n = as_diag_triage(r, f, 40);
    CHECK(n == 1 && f[0].sev == AS_DG_FAULT && !strcmp(f[0].area, "link"));
    r->connected = true;

    /* the triage order: a low battery ranks before a missing device, a device before a hot motor */
    r->battery_v = 11.6;
    r->ndevices = 2;
    snprintf(r->devices[0].name, sizeof r->devices[0].name, "FL drive");
    snprintf(r->devices[0].bus, sizeof r->devices[0].bus, "can_s0");
    r->devices[0].id = 12;
    r->devices[0].connected = 0;
    r->devices[1].connected = 1;
    r->nmechs = 1;
    snprintf(r->mechs[0].name, sizeof r->mechs[0].name, "Elevator");
    r->mechs[0].temp_c = 84;
    r->mechs[0].current_a = r->mechs[0].vel = NAN;
    r->ncan_util = 5;
    r->can_util[0] = 0.55;
    r->can_util[1] = 0.55; /* each fine alone; together they saturate their shared controller */
    r->can_util[2] = r->can_util[3] = r->can_util[4] = 0.1;
    n = as_diag_triage(r, f, 40);
    CHECK(n >= 4);
    CHECK(f[0].sev == AS_DG_FAULT && !strcmp(f[0].area, "power") && strstr(f[0].what, "11.60"));
    int can_i = -1, mech_i = -1, pair_i = -1;
    for (int i = 0; i < n; i++) {
        if (strstr(f[i].what, "FL drive can_s0 id 12")) can_i = i;
        if (strstr(f[i].what, "Elevator motor at 84")) mech_i = i;
        if (strstr(f[i].what, "can_s0 + can_s1")) pair_i = i;
    }
    CHECK(can_i > 0 && mech_i > can_i && pair_i > 0 && f[pair_i].sev == AS_DG_FAULT);
    for (int i = 1; i < n; i++) CHECK(f[i - 1].sev > f[i].sev || (f[i - 1].sev == f[i].sev && f[i - 1].order <= f[i].order));

    /* the swerve snapshot: judged only while enabled and moving */
    free(r);
    r = robot_ok();
    r->nmodules = 4;
    r->have_targets = true;
    for (int i = 0; i < 4; i++) {
        r->target_speed[i] = 2.0;
        r->target_angle[i] = 0.3;
        r->module_speed[i] = i == 2 ? 0.8 : 1.95;
        r->module_angle[i] = 0.3;
    }
    n = as_diag_triage(r, f, 40);
    CHECK(n == 1 && f[0].sev == AS_DG_INFO && strstr(f[0].what, "disabled"));
    r->enabled = true;
    n = as_diag_triage(r, f, 40);
    CHECK(n == 1 && strstr(f[0].what, "module bl") && strstr(f[0].what, "40%"));
    free(r);

    /* swerve tracking over time: the 180-degree twin counts as following; one lagging module is named */
    as_track_t t;
    as_track_verdict_t v;
    as_track_init(&t, 4);
    for (int s = 0; s < 50; s++) {
        double cs[4] = { 2, 2, 2, 2 }, ca[4] = { 0.5, 0.5, 0.5, 0.5 };
        double as[4] = { 1.96, -1.97, 1.1, 1.98 };           /* fr reports its twin: -speed, angle + pi */
        double aa[4] = { 0.5, 0.5 + 3.14159265, 0.5, 0.52 };
        as_track_add(&t, cs, ca, as, aa);
    }
    as_track_judge(&t, &v);
    CHECK(v.mod[1].follow_pct > 97 && v.mod[1].angle_err_deg < 1);
    CHECK(!strcmp(v.mod[2].verdict, "lags") && !strcmp(v.mod[0].verdict, "follows"));
    CHECK(strstr(v.summary, "module bl") && strstr(v.summary, "hardware"));

    /* all under-following alike: global, not one wheel */
    as_track_init(&t, 4);
    for (int s = 0; s < 50; s++) {
        double cs[4] = { 3, 3, 3, 3 }, ca[4] = { 0 }, as[4] = { 2.1, 2.2, 2.15, 2.1 }, aa[4] = { 0 };
        as_track_add(&t, cs, ca, as, aa);
    }
    as_track_judge(&t, &v);
    CHECK(strstr(v.summary, "every module") != NULL);

    /* a steering fault */
    as_track_init(&t, 4);
    for (int s = 0; s < 50; s++) {
        double cs[4] = { 2, 2, 2, 2 }, ca[4] = { 0 }, as[4] = { 2, 2, 2, 2 }, aa[4] = { 0, 0, 0, 0.35 };
        as_track_add(&t, cs, ca, as, aa);
    }
    as_track_judge(&t, &v);
    CHECK(!strcmp(v.mod[3].verdict, "steering") && strstr(v.summary, "module br steers"));

    /* standing still: nothing to judge, and it says so */
    as_track_init(&t, 4);
    double z[4] = { 0 };
    for (int s = 0; s < 50; s++) as_track_add(&t, z, z, z, z);
    as_track_judge(&t, &v);
    CHECK(strstr(v.summary, "not enough driving") && !strcmp(v.mod[0].verdict, "can't judge"));

    /* the recorder's CSV */
    int cols[16];
    const char *hdr = "t,mark,Swerve/ModuleStates.fl_speed,Swerve/ModuleStates.fl_angle,Swerve/ModuleStates.fr_speed,"
                      "Swerve/ModuleStates.fr_angle,Swerve/ModuleTargets.fl_speed,Swerve/ModuleTargets.fl_angle,"
                      "Swerve/ModuleTargets.fr_speed,Swerve/ModuleTargets.fr_angle,Systemcore/BatteryVolts\n";
    CHECK(as_track_csv_header(hdr, cols) && cols[0] == 2 && cols[8] == 6 && cols[4] < 0);
    CHECK(!as_track_csv_header("t,mark,Systemcore/BatteryVolts\n", cols));
    as_track_csv_header(hdr, cols);
    as_track_init(&t, 4);
    for (int s = 0; s < 20; s++) CHECK(as_track_csv_row(&t, "0.02,,1.5,0.1,0.6,0.1,1.5,0.1,1.5,0.1,12.4\n", cols));
    CHECK(!as_track_csv_row(&t, "0.04,,,,,,,,,,12.4\n", cols));
    as_track_judge(&t, &v);
    CHECK(t.used[0] == 20 && t.used[1] == 20 && t.used[2] == 0);
    CHECK(v.mod[1].follow_pct < 45 && strstr(v.summary, "module fr"));
}

/* ---- the prompts, the tools each conversation gets, and the companion's voice ---- */

static void prompts_and_tools(void)
{
    /* each prompt names the memory tools it has, and the technician's names the method it must follow */
    CHECK(strstr(AS_PROMPT_TECH, "update_notebook") && strstr(AS_PROMPT_TECH, "remember") && strstr(AS_PROMPT_TECH, "recall"));
    CHECK(strstr(AS_PROMPT_TECH, "Hardware before tuning") && strstr(AS_PROMPT_TECH, "swerve_check") && strstr(AS_PROMPT_TECH, "triage"));
    CHECK(!strstr(AS_PROMPT_TECH, "update_persona"));
    CHECK(strstr(AS_PROMPT_COMPANION, "update_persona") && !strstr(AS_PROMPT_COMPANION, "update_notebook"));
    CHECK(strstr(AS_PROMPT_COMPANION, "young woman") && strstr(AS_PROMPT_COMPANION, "\"say\""));
    /* every tool a prompt names exists */
    static const char *const NAMED[] = { "triage", "swerve_check", "remember", "forget", "recall", "update_notebook",
                                         "update_persona", "timer", "alarm", "reminder", "notes", "documents",
                                         "checklist", "calendar", "weather", "notifications", "tablet_status",
                                         "set_tablet", "open_app", "music", "smart_home", NULL };
    for (int i = 0; NAMED[i]; i++) CHECK(as_tool_find(NAMED[i]) != NULL);

    /* who gets what: robot changes and the notebook are the technician's; the persona is the companion's */
    CHECK(as_tool_for(as_tool_find("set_tunable"), AS_MEM_ASSIST) && !as_tool_for(as_tool_find("set_tunable"), AS_MEM_COMPANION));
    CHECK(!as_tool_for(as_tool_find("create_work_order"), AS_MEM_COMPANION));
    CHECK(as_tool_for(as_tool_find("update_notebook"), AS_MEM_ASSIST) && !as_tool_for(as_tool_find("update_notebook"), AS_MEM_COMPANION));
    CHECK(as_tool_for(as_tool_find("update_persona"), AS_MEM_COMPANION) && !as_tool_for(as_tool_find("update_persona"), AS_MEM_ASSIST));
    CHECK(as_tool_for(as_tool_find("pet_feed"), AS_MEM_COMPANION) && !as_tool_for(as_tool_find("pet_feed"), AS_MEM_ASSIST));
    CHECK(as_tool_for(as_tool_find("pet_status"), AS_MEM_COMPANION) && !as_tool_for(as_tool_find("pet_rename"), AS_MEM_ASSIST));
    CHECK(as_tool_for(as_tool_find("remember"), AS_MEM_ASSIST) && as_tool_for(as_tool_find("remember"), AS_MEM_COMPANION));
    CHECK(as_tool_for(as_tool_find("timer"), AS_MEM_COMPANION) && !as_tool_for(NULL, AS_MEM_ASSIST));

    /* the technician's tools array (both wire formats) leaves the persona out; every schema parses */
    ab_t b;
    ab_init(&b);
    as_tools_json(&b);
    aj_t *arr = parse(b.p);
    CHECK(arr && arr->type == AJ_ARR);
    bool persona = false, notebook = false;
    for (int i = 0; arr && i < arr->n; i++) {
        const char *nm = aj_gets(arr->kid[i], "name");
        persona |= nm && !strcmp(nm, "update_persona");
        notebook |= nm && !strcmp(nm, "update_notebook");
    }
    CHECK(!persona && notebook);
    aj_free(arr);
    for (int i = 0; i < AS_NTOOLS; i++) {
        aj_t *sc = parse(AS_TOOLS[i].schema);
        CHECK(sc != NULL);
        if (!sc) printf("  the schema of %s doesn't parse\n", AS_TOOLS[i].name);
        aj_free(sc);
    }
    as_hist_t h;
    as_hist_init(&h);
    as_hist_user(&h, NULL, "hi");
    as_req_t q = { .model = "gpt-5", .system = "s", .tools = "x" };
    as_oai_request(&h, &q, &b);
    aj_t *d = parse(b.p);
    const aj_t *tl = aj_get(d, "tools");
    persona = false;
    for (int i = 0; tl && i < tl->n; i++) {
        const char *nm = aj_gets(aj_get(tl->kid[i], "function"), "name");
        persona |= nm && !strcmp(nm, "update_persona");
    }
    CHECK(tl && tl->n > 30 && !persona);
    aj_free(d);
    as_hist_free(&h);

    /* the island's words: every tool has a label; writes are work, with a name once the input has one */
    for (int i = 0; i < AS_NTOOLS; i++) CHECK(strcmp(as_tool_label(AS_TOOLS[i].name), "working") != 0);
    aj_t *w = parse("{\"action\":\"write\",\"name\":\"pit-plan\"}"), *rd = parse("{\"action\":\"read\"}");
    char wt[80];
    CHECK(as_tool_work("documents", w) && !strcmp(as_tool_work("documents", w), "edit") && !as_tool_work("documents", rd));
    CHECK(!as_tool_work("documents", NULL) && as_tool_work("propose_patch", NULL) && !as_tool_work("triage", NULL));
    as_tool_work_text("documents", w, wt, sizeof wt);
    CHECK(!strcmp(wt, "editing pit-plan"));
    as_tool_work_text("documents", NULL, wt, sizeof wt);
    CHECK(!strcmp(wt, "editing a document"));
    aj_free(w);
    aj_free(rd);

    /* the music tool can reach the PC (its schema used to refuse "where") */
    aj_t *sc = parse(as_tool_find("music")->schema), *in = parse("{\"action\":\"pause\",\"where\":\"pc\"}");
    char err[160];
    CHECK(aj_validate(sc, in, err, sizeof err));
    aj_free(sc);
    aj_free(in);
    ab_free(&b);

    /* the companion's voice: female by default, and a model that lacks the voice gets a female one it has */
    CHECK(!strcmp(as_tts_voice("gpt-4o-mini-tts", ""), "marin"));
    CHECK(!strcmp(as_tts_voice("gpt-4o-mini-tts", NULL), "marin"));
    CHECK(!strcmp(as_tts_voice("gpt-4o-mini-tts", "shimmer"), "shimmer"));
    CHECK(!strcmp(as_tts_voice("tts-1", "marin"), "nova"));
    CHECK(!strcmp(as_tts_voice("tts-1-hd", ""), "nova"));
    CHECK(!strcmp(as_tts_voice("tts-1", "coral"), "coral"));
    CHECK(as_tts_voice_known("gpt-4o-mini-tts", "marin") && !as_tts_voice_known("tts-1", "marin"));
    CHECK(as_tts_voice_known("tts-1", AS_TTS_VOICE_SAFE) && as_tts_voice_known("gpt-4o-mini-tts", AS_TTS_VOICE_SAFE));
    char ins[320];
    as_tts_instructions(ins, sizeof ins, "cheerful and warm", true);
    CHECK(strstr(ins, "young woman") && strstr(ins, "cheerful and warm, clearly"));
}

void test_assist(int *checks, int *fails)
{
    g_checks = checks;
    g_fails = fails;
    memory();
    diagnostics();
    prompts_and_tools();
    oai_effort();
    json();
    schema();
    sse();
    assembly();
    stop_reasons();
    request_shape();
    fallback_echo();
    trimming();
    openai();
    desk();
}
