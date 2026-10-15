/*
 * multimodal.c —— 多模态大模型图像识别
 *
 * 流程：JPEG → Base64 → OpenAI 兼容 chat/completions → 提取 content 文本
 *       → 关键词解析出告警标志（intrusion/mold/collapse/pest/debris/fire）。
 *
 * 配置（环境变量）：
 *   MULTIMODAL_API_BASE  如 https://api.openai.com/v1
 *   MULTIMODAL_API_KEY   API 密钥（切勿写进代码）
 *   MULTIMODAL_MODEL     默认 gpt-4o-mini
 *   MULTIMODAL_PROMPT    覆盖内置巡检提示词
 *
 * 无 libcurl 或无密钥时：填充 graceful stub 并清除 AI 告警标志，
 * 不影响传感器告警（E01/E02/E05 火焰）与其余功能。
 */

#define _POSIX_C_SOURCE 200809L

#include "multimodal.h"

#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

static const char k_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* 内置默认巡检提示词（可用 MULTIMODAL_PROMPT 覆盖） */
static const char k_default_prompt[] =
    "You are a granary safety inspector. Describe the scene briefly. "
    "If you see illegal human intrusion, grain mold/clumping, surface "
    "collapse, pests, leftover debris, or open flame/smoke, say so "
    "clearly using keywords: intrusion, mold, collapse, pest, debris, fire.";

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

static void apply_stub(app_state_t *st, const char *reason)
{
    snprintf(st->ai_text, sizeof(st->ai_text),
             "stub analysis (%s): no obvious anomaly", reason);
    st->ai_intrusion = 0;
    st->ai_mold = 0;
    st->ai_collapse = 0;
    st->ai_pest = 0;
    st->ai_debris = 0;
    st->ai_fire = 0;
    st->ai_seq++;
}

/*
 * 关键词解析：中英文双语。
 * 与 alarm.c 的 E03/E04/E05 对应：
 *   intrusion → E03；mold/collapse/pest/debris → E04；fire → E05
 */
static void parse_ai_flags(app_state_t *st, const char *text)
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

    st->ai_intrusion =
        strstr(lower, "intrusion") != NULL || strstr(lower, "person") != NULL ||
        strstr(lower, "human") != NULL || strstr(lower, "闯入") != NULL ||
        strstr(lower, "人员") != NULL;
    st->ai_mold = strstr(lower, "mold") != NULL ||
                  strstr(lower, "mildew") != NULL ||
                  strstr(lower, "霉变") != NULL ||
                  strstr(lower, "结块") != NULL;
    st->ai_collapse = strstr(lower, "collapse") != NULL ||
                      strstr(lower, "塌陷") != NULL ||
                      strstr(lower, "sink") != NULL;
    st->ai_pest = strstr(lower, "pest") != NULL ||
                  strstr(lower, "insect") != NULL ||
                  strstr(lower, "worm") != NULL ||
                  strstr(lower, "虫害") != NULL ||
                  strstr(lower, "虫子") != NULL;
    st->ai_debris = strstr(lower, "debris") != NULL ||
                    strstr(lower, "leftover") != NULL ||
                    strstr(lower, "foreign") != NULL ||
                    strstr(lower, "杂物") != NULL ||
                    strstr(lower, "遗留") != NULL;
    st->ai_fire = strstr(lower, "flame") != NULL ||
                  strstr(lower, "fire") != NULL ||
                  strstr(lower, "明火") != NULL ||
                  strstr(lower, "烟雾") != NULL;
}

#ifdef HAVE_LIBCURL
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

/* ---------- JSON 字符串值提取（健壮版） ---------- */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 解析 4 位十六进制（失败返回 -1） */
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

/* 将 Unicode 码点编码为 UTF-8，返回写入字节数（最多 4） */
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

/* 确保 out 缓冲还能写入 extra 字节（不够则扩容） */
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
 * 从 chat/completions 响应体中提取 "content" 字符串值。
 * 正确处理 JSON 转义：\" \\ \/ \n \t \r \b \f 及 \uXXXX（含代理对，
 * 转为 UTF-8，使中文描述可直接显示）。
 */
static char *extract_json_content(const char *body)
{
    const char *key = "\"content\"";
    const char *p;
    char *out;
    size_t n = 0;
    size_t cap = 512;

    p = strstr(body, key);
    if (p == NULL) {
        return NULL;
    }
    p = strchr(p + strlen(key), '"');
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
                                0x10000u + (((unsigned int)(cp - 0xD800)) << 10) +
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
#endif /* HAVE_LIBCURL */

int multimodal_analyze_jpeg(app_state_t *st, const unsigned char *jpeg,
                            size_t jpeg_len)
{
    const char *api_base;
    const char *api_key;
    const char *model;
    const char *prompt;
    char *b64;

    if (st == NULL || jpeg == NULL || jpeg_len == 0) {
        return -1;
    }

    api_base = getenv("MULTIMODAL_API_BASE");
    api_key = getenv("MULTIMODAL_API_KEY");
    model = getenv("MULTIMODAL_MODEL");
    if (model == NULL || model[0] == '\0') {
        model = "gpt-4o-mini";
    }
    /* 提示词：st->cfg（MULTIMODAL_PROMPT）优先，其次内置默认 */
    prompt = (st->cfg.multimodal_prompt[0] != '\0') ? st->cfg.multimodal_prompt
                                                    : k_default_prompt;

    b64 = base64_encode(jpeg, jpeg_len);
    if (b64 == NULL) {
        apply_stub(st, "base64 failed");
        return -1;
    }

#ifndef HAVE_LIBCURL
    (void)api_base;
    (void)api_key;
    (void)model;
    (void)prompt;
    free(b64);
    apply_stub(st, "built without libcurl");
    return 0;
#else
    if (api_base == NULL || api_base[0] == '\0' || api_key == NULL ||
        api_key[0] == '\0') {
        free(b64);
        apply_stub(st, "no MULTIMODAL_API_BASE/KEY");
        return 0;
    }

    {
        CURL *curl;
        CURLcode rc;
        struct curl_slist *headers = NULL;
        struct mem_buf response = {0};
        char url[512];
        char auth[768];
        char *payload;
        size_t payload_cap;

        payload_cap = strlen(b64) + strlen(prompt) + 4096;
        payload = malloc(payload_cap);
        if (payload == NULL) {
            free(b64);
            apply_stub(st, "oom");
            return -1;
        }

        snprintf(url, sizeof(url), "%s/chat/completions", api_base);

        snprintf(payload, payload_cap,
                 "{"
                 "\"model\":\"%s\","
                 "\"messages\":[{"
                 "\"role\":\"user\","
                 "\"content\":["
                 "{\"type\":\"text\",\"text\":\"%s\"},"
                 "{\"type\":\"image_url\",\"image_url\":{"
                 "\"url\":\"data:image/jpeg;base64,%s\"}}"
                 "]}],"
                 "\"max_tokens\":300"
                 "}",
                 model, prompt, b64);
        free(b64);

        curl = curl_easy_init();
        if (curl == NULL) {
            free(payload);
            apply_stub(st, "curl init failed");
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
            apply_stub(st, "API request failed");
            return -1;
        }

        {
            char *content = extract_json_content(response.data);
            if (content == NULL) {
                snprintf(st->ai_text, sizeof(st->ai_text),
                         "API response parse failed");
                parse_ai_flags(st, response.data);
            } else {
                snprintf(st->ai_text, sizeof(st->ai_text), "%s", content);
                parse_ai_flags(st, content);
                free(content);
            }
            st->ai_seq++;
            log_info("multimodal: analysis done (seq=%llu, text=%zu bytes)",
                     (unsigned long long)st->ai_seq, strlen(st->ai_text));
        }
        free(response.data);
    }
    return 0;
#endif
}
