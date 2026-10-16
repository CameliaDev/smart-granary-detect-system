/*
 * multimodal.c —— 多模态大模型图像识别（火山方舟 Responses API）
 *
 * 本文件仅实现 Responses API 一套接口（旧版对话补全接口逻辑已移除）。
 *
 * 【接口】
 *   POST {MULTIMODAL_API_BASE}/responses
 *   默认 BASE : https://ark.cn-beijing.volces.com/api/v3
 *   默认模型 : doubao-seed-2-1-lite-260915
 *   密钥     : MULTIMODAL_API_KEY（环境变量，切勿写进代码）
 *   提示词   : MULTIMODAL_PROMPT（可覆盖内置巡检提示词）
 *
 * 【请求体格式】（stream:true 流式）
 *   {
 *     "model": "doubao-seed-2-1-lite-260915",
 *     "stream": true,
 *     "input": [{
 *       "role": "user",
 *       "content": [
 *         {"type": "input_text",  "text": "<巡检提示词>"},
 *         {"type": "input_image", "image_url": "data:image/jpeg;base64,<b64>"}
 *       ]
 *     }],
 *     "max_output_tokens": 300,
 *     "text": {"format": {"type": "json_object"}}
 *   }
 *
 * 【响应】SSE 流式事件，逐行解析 data: 负载，
 *   累积 response.output_text.delta 事件的 "delta" 字段得到完整回复文本；
 *   兼容流末尾的 [DONE] 标记；若流中无 delta（如非流式响应），
 *   回退提取整体 JSON 中的 "output_text" 字段。
 *
 * 【降级】未链接 libcurl / 无密钥 / 网络失败：填充 graceful stub
 *   并清除 AI 告警标志，不影响传感器告警（E01/E02/E05）与其余功能。
 */

#define _POSIX_C_SOURCE 200809L

#include "multimodal.h"

#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define MULTIMODAL_MAYBE_UNUSED __attribute__((unused))
#else
#define MULTIMODAL_MAYBE_UNUSED
#endif

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

static const char k_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* 内置默认巡检提示词（MULTIMODAL_PROMPT 可覆盖） */
static const char k_default_prompt[] =
    "You are a granary safety inspector. Describe the scene briefly. "
    "If you see illegal human intrusion, grain mold/clumping, surface "
    "collapse, pests, leftover debris, or open flame/smoke, say so "
    "clearly using keywords: intrusion, mold, collapse, pest, debris, fire.";

/* 流式累积文本上限（防止异常响应耗尽内存；超出部分丢弃） */
#define AI_TEXT_ACCUM_MAX (64 * 1024)

/*
 * 内置默认 API Key（全局生效，按项目要求直接写入代码）：
 *   - 环境变量 MULTIMODAL_API_KEY 优先，未设置时自动使用此内置密钥；
 *   - 编译期可用 -DMULTIMODAL_API_KEY_DEFAULT="..." 覆盖；
 *   - 安全提醒：含真实密钥的源码切勿提交到公开仓库或外发。
 */
#ifndef MULTIMODAL_API_KEY_DEFAULT
#define MULTIMODAL_API_KEY_DEFAULT \
    "ark-73964143-8d3a-4c90-9975-cdcb28e74da7-392cc"
#endif

/* ---------- Base64 编码 ---------- */

static char *base64_encode(const unsigned char *data, size_t len)
{
    size_t out_len = 4 * ((len + 2) / 3);
    char *out;
    size_t i;
    size_t j = 0;

    out = malloc(out_len + 1);
    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i < len; i += 3) {
        unsigned int n = ((unsigned int)data[i]) << 16;
        if (i + 1 < len) {
            n |= ((unsigned int)data[i + 1]) << 8;
        }
        if (i + 2 < len) {
            n |= (unsigned int)data[i + 2];
        }
        out[j++] = k_b64[(n >> 18) & 63];
        out[j++] = k_b64[(n >> 12) & 63];
        out[j++] = (i + 1 < len) ? k_b64[(n >> 6) & 63] : '=';
        out[j++] = (i + 2 < len) ? k_b64[n & 63] : '=';
    }
    out[j] = '\0';
    return out;
}

/* ---------- JSON 字符串转义（提示词嵌入请求体前使用） ---------- */

static char *json_escape(const char *in)
{
    size_t len = strlen(in);
    char *out = malloc(len * 6 + 1);
    size_t j = 0;
    size_t i;

    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else if (c == '\r') {
            out[j++] = '\\';
            out[j++] = 'r';
        } else if (c == '\t') {
            out[j++] = '\\';
            out[j++] = 't';
        } else if (c < 0x20) {
            j += (size_t)snprintf(out + j, len * 6 + 1 - j, "\\u%04x", c);
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
    return out;
}

/* ---------- 动态文本缓冲（累积流式 delta） ---------- */

struct text_buf {
    char *data;
    size_t len;
    size_t cap;
};

MULTIMODAL_MAYBE_UNUSED
static int text_buf_append(struct text_buf *tb, const char *s, size_t n)
{
    if (tb->len + n > AI_TEXT_ACCUM_MAX) {
        return -1; /* 超限丢弃，防异常响应耗尽内存 */
    }
    if (tb->len + n + 1 > tb->cap) {
        size_t cap = (tb->cap != 0) ? tb->cap : 1024;
        char *grown;
        while (cap < tb->len + n + 1) {
            cap *= 2;
        }
        grown = realloc(tb->data, cap);
        if (grown == NULL) {
            return -1;
        }
        tb->data = grown;
        tb->cap = cap;
    }
    memcpy(tb->data + tb->len, s, n);
    tb->len += n;
    tb->data[tb->len] = '\0';
    return 0;
}

#ifdef HAVE_LIBCURL
/* ---------- JSON 字符串值提取（健壮转义 + \uXXXX → UTF-8） ---------- */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex4(const char *p)
{
    int i;
    int v = 0;

    for (i = 0; i < 4; i++) {
        int h = hex_value(p[i]);
        if (h < 0) {
            return -1;
        }
        v = (v << 4) | h;
    }
    return v;
}

static size_t utf8_encode(unsigned int cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int ensure_cap(char **buf, size_t *cap, size_t used, size_t extra)
{
    size_t need = used + extra + 1;
    char *tmp;

    if (need <= *cap) {
        return 0;
    }
    tmp = realloc(*buf, *cap * 2 > need ? *cap * 2 : need);
    if (tmp == NULL) {
        return -1;
    }
    *buf = tmp;
    *cap = (*cap * 2 > need) ? *cap * 2 : need;
    return 0;
}

/*
 * 从 JSON 文本中提取指定键的字符串值。
 * 正确处理 \" \\ \/ \n \t \r \b \f 与 \uXXXX（含代理对，转 UTF-8）。
 * 返回 malloc 的字符串（调用方 free）；失败返回 NULL。
 */
static char *extract_json_string_field(const char *body, const char *key)
{
    char pattern[64];
    const char *p;
    char *out;
    size_t n = 0;
    size_t cap = 512;

    if (body == NULL || key == NULL) {
        return NULL;
    }
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >=
        (int)sizeof(pattern)) {
        return NULL;
    }
    p = strstr(body, pattern);
    if (p == NULL) {
        return NULL;
    }
    p = strchr(p + strlen(pattern), '"');
    if (p == NULL) {
        return NULL;
    }

    out = malloc(cap);
    if (out == NULL) {
        return NULL;
    }

    for (p = p + 1; *p != '\0'; p++) {
        if (*p == '"') {
            break; /* 字符串结束 */
        }
        if (*p == '\\' && p[1] != '\0') {
            p++;
            switch (*p) {
            case 'n': out[n++] = '\n'; break;
            case 't': out[n++] = '\t'; break;
            case 'r': out[n++] = '\r'; break;
            case 'b': out[n++] = '\b'; break;
            case 'f': out[n++] = '\f'; break;
            case '/': out[n++] = '/';  break;
            case '"': out[n++] = '"';  break;
            case '\\': out[n++] = '\\'; break;
            case 'u': {
                int cp = parse_hex4(p + 1);
                if (cp < 0) {
                    out[n++] = '?';
                } else {
                    p += 4;
                    /* 代理对：高代理 + 低代理 → 合并码点 */
                    if (cp >= 0xD800 && cp <= 0xDBFF && p[1] == '\\' &&
                        p[2] == 'u') {
                        int lo = parse_hex4(p + 3);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            unsigned int full =
                                0x10000u +
                                (((unsigned int)(cp - 0xD800)) << 10) +
                                (unsigned int)(lo - 0xDC00);
                            if (ensure_cap(&out, &cap, n, 4) == 0) {
                                n += utf8_encode(full, out + n);
                            }
                            p += 6;
                            continue;
                        }
                    }
                    if (ensure_cap(&out, &cap, n, 4) == 0) {
                        n += utf8_encode((unsigned int)cp, out + n);
                    }
                }
                continue;
            }
            default:
                out[n++] = *p;
                break;
            }
        } else {
            out[n++] = *p;
        }
        if (ensure_cap(&out, &cap, n, 4) < 0) {
            free(out);
            return NULL;
        }
    }
    out[n] = '\0';
    return out;
}

/* ---------- SSE 流式解析 ---------- */

/*
 * 解析 SSE 事件流：逐行扫描 "data:" 负载，
 * 提取其中的 "delta" 字符串字段（response.output_text.delta 事件）累积文本。
 * 兼容流末尾的 "[DONE]" 结束标记。
 */
static void parse_sse_deltas(const char *body, struct text_buf *tb)
{
    const char *p = body;

    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
        const char *line = p;
        const char *payload;
        size_t plen;
        char *line_buf;
        char *delta;

        /* 去掉行尾 \r */
        while (line_len > 0 && line[line_len - 1] == '\r') {
            line_len--;
        }
        if (line_len > 5 && strncmp(line, "data:", 5) == 0) {
            payload = line + 5;
            plen = line_len - 5;
            while (plen > 0 && (*payload == ' ' || *payload == '\t')) {
                payload++;
                plen--;
            }
            /* 流结束标记 */
            if (plen >= 6 && strncmp(payload, "[DONE]", 6) == 0) {
                break;
            }
            line_buf = malloc(plen + 1);
            if (line_buf != NULL) {
                memcpy(line_buf, payload, plen);
                line_buf[plen] = '\0';
                delta = extract_json_string_field(line_buf, "delta");
                if (delta != NULL) {
                    text_buf_append(tb, delta, strlen(delta));
                    free(delta);
                }
                free(line_buf);
            }
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }
}

/* ---------- libcurl 响应累积 ---------- */

struct mem_buf {
    char *data;
    size_t len;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct mem_buf *m = userdata;
    size_t total = size * nmemb;
    char *n = realloc(m->data, m->len + total + 1);

    if (n == NULL) {
        return 0;
    }
    m->data = n;
    memcpy(m->data + m->len, ptr, total);
    m->len += total;
    m->data[m->len] = '\0';
    return total;
}
#endif /* HAVE_LIBCURL */

/* ---------- AI 结果解析 ---------- */

/*
 * 分析结果先在局部累积，最后一次性提交到 st（短暂加锁）。
 * 网络请求全程不持有 st->lock，避免阻塞推流/状态查询等其余线程。
 */
typedef struct {
    char text[STATE_AI_TEXT_MAX];
    int intrusion;
    int mold;
    int collapse;
    int pest;
    int debris;
    int fire;
} ai_result_t;

/* 提交结果到全局状态：本文件唯一触碰 st 共享字段的地方，短暂加锁 */
static void commit_result(app_state_t *st, const ai_result_t *res)
{
    pthread_mutex_lock(&st->lock);
    snprintf(st->ai_text, sizeof(st->ai_text), "%s", res->text);
    st->ai_intrusion = res->intrusion;
    st->ai_mold = res->mold;
    st->ai_collapse = res->collapse;
    st->ai_pest = res->pest;
    st->ai_debris = res->debris;
    st->ai_fire = res->fire;
    st->ai_seq++;
    pthread_mutex_unlock(&st->lock);
}

static void fill_stub(ai_result_t *res, const char *reason)
{
    snprintf(res->text, sizeof(res->text),
             "stub analysis (%s): no obvious anomaly", reason);
    res->intrusion = 0;
    res->mold = 0;
    res->collapse = 0;
    res->pest = 0;
    res->debris = 0;
    res->fire = 0;
}

/*
 * 关键词解析：中英文双语。
 * 与 alarm.c 的 E03/E04/E05 对应：
 *   intrusion → E03；mold/collapse/pest/debris → E04；fire → E05
 */
MULTIMODAL_MAYBE_UNUSED
static void parse_ai_flags(ai_result_t *res, const char *text)
{
    char lower[STATE_AI_TEXT_MAX];
    size_t i;

    if (text == NULL) {
        return;
    }
    for (i = 0; text[i] != '\0' && i + 1 < sizeof(lower); i++) {
        lower[i] = (char)tolower((unsigned char)text[i]);
    }
    lower[i] = '\0';

    res->intrusion =
        strstr(lower, "intrusion") != NULL || strstr(lower, "person") != NULL ||
        strstr(lower, "human") != NULL || strstr(lower, "闯入") != NULL ||
        strstr(lower, "人员") != NULL;
    res->mold = strstr(lower, "mold") != NULL ||
                strstr(lower, "mildew") != NULL ||
                strstr(lower, "霉变") != NULL ||
                strstr(lower, "结块") != NULL;
    res->collapse = strstr(lower, "collapse") != NULL ||
                    strstr(lower, "塌陷") != NULL ||
                    strstr(lower, "sink") != NULL;
    res->pest = strstr(lower, "pest") != NULL ||
                strstr(lower, "insect") != NULL ||
                strstr(lower, "worm") != NULL ||
                strstr(lower, "虫害") != NULL ||
                strstr(lower, "虫子") != NULL;
    res->debris = strstr(lower, "debris") != NULL ||
                  strstr(lower, "leftover") != NULL ||
                  strstr(lower, "foreign") != NULL ||
                  strstr(lower, "杂物") != NULL ||
                  strstr(lower, "遗留") != NULL;
    res->fire = strstr(lower, "flame") != NULL ||
                strstr(lower, "fire") != NULL ||
                strstr(lower, "明火") != NULL ||
                strstr(lower, "烟雾") != NULL;
}

/* ---------- 主入口 ---------- */

int multimodal_analyze_jpeg(app_state_t *st, const unsigned char *jpeg,
                            size_t jpeg_len)
{
    const char *api_key;
    const char *prompt;
    char *prompt_esc;
    char *b64;
    ai_result_t res;

    if (st == NULL || jpeg == NULL || jpeg_len == 0) {
        return -1;
    }

    /*
     * st->cfg 为启动期一次性加载的只读配置（state_init 后不再变化），
     * 无锁读取安全。本函数从头到尾不持有 st->lock，
     * 全部结果写入局部变量 res，最后 commit_result 短暂加锁提交。
     */
    api_key = getenv("MULTIMODAL_API_KEY");
    /* 未设置环境变量时使用内置默认密钥（全局生效） */
    if (api_key == NULL || api_key[0] == '\0') {
        api_key = MULTIMODAL_API_KEY_DEFAULT;
    }
    /* 提示词：st->cfg（MULTIMODAL_PROMPT）优先，其次内置默认 */
    prompt = (st->cfg.multimodal_prompt[0] != '\0') ? st->cfg.multimodal_prompt
                                                    : k_default_prompt;
    prompt_esc = json_escape(prompt);
    if (prompt_esc == NULL) {
        fill_stub(&res, "prompt escape failed");
        commit_result(st, &res);
        return -1;
    }

    b64 = base64_encode(jpeg, jpeg_len);
    if (b64 == NULL) {
        free(prompt_esc);
        fill_stub(&res, "base64 failed");
        commit_result(st, &res);
        return -1;
    }

#ifndef HAVE_LIBCURL
    (void)api_key;
    free(b64);
    free(prompt_esc);
    fill_stub(&res, "built without libcurl");
    commit_result(st, &res);
    return 0;
#else
    if (api_key == NULL || api_key[0] == '\0') {
        free(b64);
        free(prompt_esc);
        fill_stub(&res, "no API key configured");
        commit_result(st, &res);
        return 0;
    }

    {
        CURL *curl;
        CURLcode rc;
        struct curl_slist *headers = NULL;
        struct mem_buf response = {0};
        struct text_buf text = {0};
        char url[512];
        char auth[768];
        char *payload;
        size_t payload_cap;

        /* URL: {MULTIMODAL_API_BASE}/responses */
        snprintf(url, sizeof(url), "%s/responses",
                 st->cfg.multimodal_api_base);

        payload_cap = strlen(b64) + strlen(prompt_esc) + 4096;
        payload = malloc(payload_cap);
        if (payload == NULL) {
            free(b64);
            free(prompt_esc);
            fill_stub(&res, "oom");
            commit_result(st, &res);
            return -1;
        }

        /*
         * Responses API 请求体：
         *   input[] 承载多模态内容（input_text + input_image Data URL），
         *   text.format 强制 JSON 对象输出，stream:true 流式返回。
         */
        snprintf(payload, payload_cap,
                 "{"
                 "\"model\":\"%s\","
                 "\"stream\":true,"
                 "\"input\":[{"
                 "\"role\":\"user\","
                 "\"content\":["
                 "{\"type\":\"input_text\",\"text\":\"%s\"},"
                 "{\"type\":\"input_image\","
                 "\"image_url\":\"data:image/jpeg;base64,%s\"}"
                 "]}],"
                 "\"max_output_tokens\":300,"
                 "\"text\":{\"format\":{\"type\":\"json_object\"}}"
                 "}",
                 st->cfg.multimodal_model, prompt_esc, b64);
        free(b64);
        free(prompt_esc);

        curl = curl_easy_init();
        if (curl == NULL) {
            free(payload);
            fill_stub(&res, "curl init failed");
            commit_result(st, &res);
            return -1;
        }

        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, auth);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

        rc = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(payload);

        if (rc != CURLE_OK || response.data == NULL) {
            log_error("multimodal: curl error %s", curl_easy_strerror(rc));
            free(response.data);
            fill_stub(&res, "API request failed");
            commit_result(st, &res);
            return -1;
        }

        /*
         * 解析响应：
         *  1. 流式（stream:true）：累积各事件的 delta 文本；
         *  2. 回退：整体响应中的 "output_text" 字段（非流式/完成事件）。
         */
        parse_sse_deltas(response.data, &text);
        if (text.len == 0) {
            char *fallback = extract_json_string_field(response.data,
                                                       "output_text");
            if (fallback != NULL) {
                text_buf_append(&text, fallback, strlen(fallback));
                free(fallback);
            }
        }

        if (text.len == 0) {
            snprintf(res.text, sizeof(res.text), "API response parse failed");
            parse_ai_flags(&res, response.data);
            log_warn("multimodal: no text delta in response (%zu bytes)",
                     response.len);
        } else {
            snprintf(res.text, sizeof(res.text), "%s", text.data);
            parse_ai_flags(&res, text.data);
            log_info("multimodal: analysis done (text=%zu bytes)", text.len);
        }

        free(text.data);
        free(response.data);
    }
    commit_result(st, &res);
    return 0;
#endif
}
