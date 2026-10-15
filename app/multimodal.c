#define _POSIX_C_SOURCE 200809L

#include "multimodal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

static const char k_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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
    st->ai_fire = 0;
    st->ai_seq++;
}

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

static char *extract_json_content(const char *body)
{
    const char *key = "\"content\"";
    const char *p;
    const char *start;
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
    start = p + 1;
    out = malloc(cap);
    if (out == NULL) {
        return NULL;
    }
    for (p = start; *p != '\0'; p++) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
            if (*p == 'n') {
                out[n++] = '\n';
            } else if (*p == 't') {
                out[n++] = '\t';
            } else if (*p == '"') {
                out[n++] = '"';
            } else if (*p == '\\') {
                out[n++] = '\\';
            } else {
                out[n++] = *p;
            }
        } else if (*p == '"') {
            break;
        } else {
            out[n++] = *p;
        }
        if (n + 1 >= cap) {
            char *tmp = realloc(out, cap * 2);
            if (tmp == NULL) {
                free(out);
                return NULL;
            }
            out = tmp;
            cap *= 2;
        }
    }
    out[n] = '\0';
    return out;
}
#endif

int multimodal_analyze_jpeg(app_state_t *st, const unsigned char *jpeg,
                            size_t jpeg_len)
{
    const char *api_base;
    const char *api_key;
    const char *model;
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

    b64 = base64_encode(jpeg, jpeg_len);
    if (b64 == NULL) {
        apply_stub(st, "base64 failed");
        return -1;
    }

#ifndef HAVE_LIBCURL
    (void)api_base;
    (void)api_key;
    (void)model;
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
        const char *prompt =
            "You are a granary safety inspector. Describe the scene briefly. "
            "If you see illegal human intrusion, grain mold/clumping, surface "
            "collapse, pests, leftover debris, or open flame/smoke, say so "
            "clearly using keywords: intrusion, mold, collapse, fire.";

        payload_cap = strlen(b64) + 4096;
        payload = malloc(payload_cap);
        if (payload == NULL) {
            free(b64);
            apply_stub(st, "oom");
            return -1;
        }

        snprintf(url, sizeof(url), "%s/chat/completions", api_base);
        /* trim trailing slash on base already handled by format */

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

        rc = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(payload);

        if (rc != CURLE_OK || response.data == NULL) {
            fprintf(stderr, "multimodal: curl error %s\n",
                    curl_easy_strerror(rc));
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
        }
        free(response.data);
    }
    return 0;
#endif
}
