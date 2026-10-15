/*
 * http_server.c —— 局域网网页服务
 *
 * 端点：
 *   GET /                 监控页（MJPEG 画面 + 数据/告警叠加 + 控制按钮 + 快照库）
 *   GET /stream           MJPEG 推流（multipart/x-mixed-replace）
 *   GET /status.json      状态 JSON（传感器 / AI / 告警 / 告警历史 / 统计）
 *   GET /snapshots.json   快照文件列表（供网页画廊浏览）
 *   GET /snapshot.jpg?name=<YYYY-MM-DD/snap_*.jpg>  读取单张快照
 *   GET /control?action=pause|resume|toggle|mute    巡检/静音控制
 *
 * 端口：GRANARY_HTTP_PORT（默认 8081），运行期生效。
 * 每客户端独立线程，套接字带收发超时，避免慢速连接拖死服务。
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "http_server.h"
#include "state.h"
#include "log.h"

#include "app.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* status.json 中告警历史最多返回条数 */
#define STATUS_HISTORY_MAX 8
/* 快照列表最多返回条数 */
#define SNAPSHOT_LIST_MAX 200

static const char *HTML_PAGE =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"/>"
    "<title>Smart Granary Monitor</title>"
    "<style>"
    "body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:16px}"
    "img{max-width:100%;border:2px solid #444}"
    "#overlay{white-space:pre-wrap;background:#222;padding:12px;margin-top:12px;"
    "border-radius:8px;font-size:14px}"
    ".alarm{color:#f66;font-weight:bold}"
    ".panel{background:#1a1a1a;border:1px solid #333;border-radius:8px;"
    "padding:12px;margin-top:12px}"
    "button{background:#2b6cb0;color:#fff;border:0;border-radius:6px;"
    "padding:8px 14px;margin-right:8px;cursor:pointer;font-size:14px}"
    "button:hover{background:#2c5282}"
    "button.off{background:#4a5568}"
    "#history{max-height:180px;overflow:auto;font-size:13px}"
    "#history div{padding:2px 0;border-bottom:1px solid #222}"
    "#shots{display:flex;flex-wrap:wrap;gap:8px;margin-top:8px}"
    "#shots img{width:220px;border:1px solid #444;border-radius:4px}"
    ".dim{color:#999;font-size:12px}"
    "</style></head><body>"
    "<h1>智能粮仓监控 / Smart Granary</h1>"
    "<img id=\"v\" src=\"/stream\" alt=\"live\"/>"
    "<div class=\"panel\">"
    "<button id=\"btnRun\" onclick=\"ctrl('toggle')\">暂停/恢复巡检</button>"
    "<button id=\"btnMute\" onclick=\"ctrl('mute')\">蜂鸣器静音</button>"
    "<span id=\"mode\" class=\"dim\"></span>"
    "</div>"
    "<div id=\"overlay\">loading...</div>"
    "<div class=\"panel\"><b>告警历史</b>"
    "<div id=\"history\"><div class=\"dim\">（暂无）</div></div></div>"
    "<div class=\"panel\"><b>快照库</b>"
    "<div id=\"shots\"><div class=\"dim\">loading...</div></div></div>"
    "<script>"
    "async function tick(){"
    "try{const r=await fetch('/status.json');const j=await r.json();"
    "const el=document.getElementById('overlay');"
    "el.className=j.any_alarm?'alarm':'';"
    "el.textContent="
    "'Mode: '+(j.running?'RUNNING':'PAUSED')+'\\n'+"
    "'U='+j.U_V+'V I='+j.I_A+'A P='+j.P_W+'W\\n'+"
    "'T='+(j.T_valid?j.T_C:'ERR')+'C RH='+(j.RH_valid?j.RH:'ERR')+'% Lux='+(j.lux_valid?j.lux:'ERR')+'\\n'+"
    "'PIR='+j.pir+' Flame='+j.flame+'\\n'+"
    "'E01='+j.E01+' E02='+j.E02+' E03='+j.E03+' E04='+j.E04+' E05='+j.E05+'\\n'+"
    "'Alarm: '+j.alarm+'\\nAI: '+j.ai;"
    "document.getElementById('mode').textContent="
    "'snapshots='+j.snapshots+' uptime='+j.uptime_s+'s';"
    "document.getElementById('btnRun').textContent="
    "j.running?'暂停巡检':'启动巡检';"
    "document.getElementById('btnRun').className=j.running?'':'off';"
    "document.getElementById('btnMute').textContent="
    "j.muted?'取消静音':'蜂鸣器静音';"
    "document.getElementById('btnMute').className=j.muted?'off':'';"
    "const h=document.getElementById('history');"
    "if(j.history&&j.history.length){"
    "h.innerHTML='';"
    "for(const e of j.history){"
    "const d=document.createElement('div');"
    "d.textContent='['+e.t+'] '+e.text;"
    "h.appendChild(d);}"
    "}else{h.innerHTML='<div class=\"dim\">（暂无）</div>';}"
    "}catch(e){}"
    "setTimeout(tick,1000);}"
    "async function loadShots(){"
    "try{const r=await fetch('/snapshots.json');const j=await r.json();"
    "const box=document.getElementById('shots');"
    "if(!j.files||!j.files.length){"
    "box.innerHTML='<div class=\"dim\">（暂无快照）</div>';return;}"
    "box.innerHTML='';"
    "for(const f of j.files){"
    "const im=document.createElement('img');"
    "im.src='/snapshot.jpg?name='+encodeURIComponent(f.name);"
    "im.title=f.name+' ('+f.size+'B)';"
    "box.appendChild(im);}"
    "}catch(e){}"
    "setTimeout(loadShots,5000);}"
    "async function ctrl(action){"
    "try{await fetch('/control?action='+action);}catch(e){}"
    "tick();}"
    "tick();loadShots();"
    "</script></body></html>";

/* ---------- 工具函数 ---------- */

static void json_escape(const char *in, char *out, size_t out_sz)
{
    size_t j = 0;
    size_t i;

    if (out_sz == 0) {
        return;
    }
    for (i = 0; in != NULL && in[i] != '\0' && j + 2 < out_sz; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = c;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            out[j++] = ' ';
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t left = len;

    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

static void send_simple(int client, const char *status, const char *ctype,
                        const char *body, size_t body_len)
{
    char header[256];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.0 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     status, ctype, body_len);
    if (n < 0 || (size_t)n >= sizeof(header)) {
        return;
    }
    if (send_all(client, header, strlen(header)) == 0 && body_len > 0) {
        send_all(client, body, body_len);
    }
}

/* ---------- /status.json ---------- */

static void handle_status(int client, app_state_t *st)
{
    char body[8192];
    char history[4096];
    char header[256];
    char alarm_e[STATE_ALARM_TEXT_MAX * 2];
    char ai_e[STATE_AI_TEXT_MAX * 2];
    char overlay_e[STATE_OVERLAY_MAX * 2];
    int any;
    int n;
    size_t hn = 0;
    int count;
    int head;
    int k;
    time_t now = time(NULL);

    pthread_mutex_lock(&st->lock);
    any = st->e01 || st->e02 || st->e03 || st->e04 || st->e05;
    json_escape(st->alarm_text, alarm_e, sizeof(alarm_e));
    json_escape(st->ai_text, ai_e, sizeof(ai_e));
    json_escape(st->overlay_text, overlay_e, sizeof(overlay_e));

    /* 告警历史（最新在前，最多 STATUS_HISTORY_MAX 条） */
    history[0] = '\0';
    count = st->alarm_history_count;
    head = st->alarm_history_head;
    if (count > STATUS_HISTORY_MAX) {
        count = STATUS_HISTORY_MAX;
    }
    for (k = 0; k < count; k++) {
        int idx = (head - 1 - k + STATE_ALARM_HISTORY_MAX) %
                  STATE_ALARM_HISTORY_MAX;
        char tbuf[16];
        char hesc[STATE_ALARM_TEXT_MAX * 2];
        struct tm tm_h;
        int w;

        json_escape(st->alarm_history_text[idx], hesc, sizeof(hesc));
        localtime_r(&st->alarm_history_time[idx], &tm_h);
        strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_h);
        w = snprintf(history + hn, sizeof(history) - hn,
                     "%s{\"t\":\"%s\",\"text\":\"%s\"}",
                     (k == 0) ? "" : ",", tbuf, hesc);
        if (w < 0 || (size_t)w >= sizeof(history) - hn) {
            history[hn] = '\0';
            break;
        }
        hn += (size_t)w;
    }

    n = snprintf(
        body, sizeof(body),
        "{\"running\":%s,\"U_V\":%.3f,\"I_A\":%.3f,\"P_W\":%.3f,"
        "\"T_C\":%.2f,\"RH\":%.2f,\"lux\":%.1f,"
        "\"T_valid\":%s,\"RH_valid\":%s,\"lux_valid\":%s,"
        "\"pir\":%s,\"flame\":%s,"
        "\"E01\":%d,\"E02\":%d,\"E03\":%d,\"E04\":%d,\"E05\":%d,"
        "\"any_alarm\":%s,\"muted\":%s,\"alarm\":\"%s\",\"ai\":\"%s\","
        "\"overlay\":\"%s\",\"ai_seq\":%llu,\"jpeg_seq\":%llu,"
        "\"snapshots\":%llu,\"uptime_s\":%ld,\"history\":[%s]}",
        st->inspection_running ? "true" : "false", st->supply_voltage_v,
        st->current_a, st->power_w, st->temperature_c, st->humidity_rh,
        st->illuminance,
        st->temp_valid ? "true" : "false",
        st->hum_valid ? "true" : "false",
        st->lux_valid ? "true" : "false",
        st->pir_detected ? "true" : "false",
        st->flame_detected ? "true" : "false", st->e01, st->e02, st->e03,
        st->e04, st->e05, any ? "true" : "false",
        st->buzzer_muted ? "true" : "false", alarm_e, ai_e, overlay_e,
        (unsigned long long)st->ai_seq,
        (unsigned long long)st->jpeg_seq,
        (unsigned long long)st->snapshot_count,
        (long)(now - st->start_time), history);
    pthread_mutex_unlock(&st->lock);

    if (n < 0) {
        return;
    }
    /* 超长（历史过多）时退化为不含历史的响应 */
    if ((size_t)n >= sizeof(body) - 1) {
        n = snprintf(body, sizeof(body),
                     "{\"running\":%s,\"alarm\":\"%s\",\"ai\":\"%s\","
                     "\"history\":[]}",
                     st->inspection_running ? "true" : "false", alarm_e, ai_e);
        if (n < 0) {
            return;
        }
    }
    snprintf(header, sizeof(header),
             "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
             "Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\n"
             "Connection: close\r\n\r\n",
             n);
    send_all(client, header, strlen(header));
    send_all(client, body, (size_t)n);
}

/* ---------- 监控页 ---------- */

static void handle_index(int client)
{
    int len = (int)strlen(HTML_PAGE);
    char header[256];

    snprintf(header, sizeof(header),
             "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
             "Content-Length: %d\r\nConnection: close\r\n\r\n",
             len);
    send_all(client, header, strlen(header));
    send_all(client, HTML_PAGE, (size_t)len);
}

/* ---------- MJPEG 推流 ---------- */

static void handle_stream(int client, app_state_t *st)
{
    const char *hdr =
        "HTTP/1.0 200 OK\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Connection: close\r\n\r\n";
    uint64_t last_seq = 0;

    if (send_all(client, hdr, strlen(hdr)) < 0) {
        return;
    }

    while (!st->stop) {
        unsigned char *jpeg = NULL;
        size_t jpeg_len = 0;
        uint64_t seq = 0;
        char part[128];
        int running;

        pthread_mutex_lock(&st->lock);
        running = st->inspection_running;
        pthread_mutex_unlock(&st->lock);

        if (!running) {
            /* 推流同步暂停：保持连接，等待恢复 */
            sleep(1);
            continue;
        }

        if (state_copy_jpeg(&jpeg, &jpeg_len, &seq) < 0) {
            usleep(100000);
            continue;
        }
        if (seq == last_seq) {
            free(jpeg);
            usleep(50000);
            continue;
        }
        last_seq = seq;

        snprintf(part, sizeof(part),
                 "--frame\r\nContent-Type: image/jpeg\r\n"
                 "Content-Length: %zu\r\n\r\n",
                 jpeg_len);
        if (send_all(client, part, strlen(part)) < 0 ||
            send_all(client, jpeg, jpeg_len) < 0 ||
            send_all(client, "\r\n", 2) < 0) {
            free(jpeg);
            break;
        }
        free(jpeg);
        usleep(30000);
    }
}

/* ---------- 快照列表 / 读取 / 控制 ---------- */

/* 收集快照文件（日期子目录一层），按名称排序由前端处理 */
static void handle_snapshots_list(int client, app_state_t *st)
{
    char body[16384];
    char dir[256];
    size_t n = 0;
    int count = 0;

    pthread_mutex_lock(&st->lock);
    snprintf(dir, sizeof(dir), "%s", st->cfg.snapshot_dir);
    pthread_mutex_unlock(&st->lock);

    n += (size_t)snprintf(body + n, sizeof(body) - n,
                          "{\"dir\":\"%s\",\"files\":[", dir);

    /* 扫描日期子目录（快照均按 YYYY-MM-DD/ 存放） */
    {
        DIR *d = opendir(dir);
        struct dirent *e;

        if (d != NULL) {
            while ((e = readdir(d)) != NULL && count < SNAPSHOT_LIST_MAX) {
                char sub[320];
                DIR *sd;
                struct dirent *f;

                if (e->d_name[0] == '.') {
                    continue;
                }
                if (snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name) >=
                    (int)sizeof(sub)) {
                    continue;
                }
                sd = opendir(sub);
                if (sd == NULL) {
                    continue;
                }
                while ((f = readdir(sd)) != NULL &&
                       count < SNAPSHOT_LIST_MAX) {
                    char fpath[640];
                    struct stat fst;

                    if (f->d_name[0] == '.') {
                        continue;
                    }
                    if (snprintf(fpath, sizeof(fpath), "%s/%s", sub,
                                 f->d_name) >= (int)sizeof(fpath)) {
                        continue;
                    }
                    if (stat(fpath, &fst) != 0 || !S_ISREG(fst.st_mode)) {
                        continue;
                    }
                    {
                        char tbuf[16];
                        struct tm tm_f;
                        int w;

                        localtime_r(&fst.st_mtime, &tm_f);
                        strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_f);
                        w = snprintf(body + n, sizeof(body) - n,
                                     "%s{\"name\":\"%s/%s\",\"size\":%ld,"
                                     "\"mtime\":\"%s\"}",
                                     (count == 0) ? "" : ",", e->d_name,
                                     f->d_name, (long)fst.st_size, tbuf);
                        if (w < 0 || (size_t)w >= sizeof(body) - n) {
                            body[n] = '\0';
                            goto done;
                        }
                        n += (size_t)w;
                        count++;
                    }
                }
                closedir(sd);
            }
            closedir(d);
        }
    }
done:
    n += (size_t)snprintf(body + n, sizeof(body) - n, "]}");
    send_simple(client, "200 OK", "application/json", body, n);
}

/* 名称安全校验：仅允许 日期/文件名 形式，防路径穿越 */
static int snapshot_name_safe(const char *name)
{
    size_t i;

    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    if (name[0] == '/' || strstr(name, "..") != NULL ||
        strchr(name, '\\') != NULL) {
        return 0;
    }
    for (i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-' ||
                 c == '.';
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

static void handle_snapshot_file(int client, app_state_t *st, const char *name)
{
    char dir[256];
    char path[640];
    struct stat fst;
    FILE *fp;
    unsigned char *buf;
    size_t size;
    char header[256];

    if (!snapshot_name_safe(name)) {
        send_simple(client, "400 Bad Request", "text/plain", "bad name", 8);
        return;
    }
    pthread_mutex_lock(&st->lock);
    snprintf(dir, sizeof(dir), "%s", st->cfg.snapshot_dir);
    pthread_mutex_unlock(&st->lock);

    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >=
        (int)sizeof(path)) {
        send_simple(client, "400 Bad Request", "text/plain", "too long", 8);
        return;
    }
    if (stat(path, &fst) != 0 || !S_ISREG(fst.st_mode) ||
        fst.st_size <= 0 || fst.st_size > 8 * 1024 * 1024) {
        send_simple(client, "404 Not Found", "text/plain", "not found", 9);
        return;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        send_simple(client, "404 Not Found", "text/plain", "not found", 9);
        return;
    }
    size = (size_t)fst.st_size;
    buf = malloc(size);
    if (buf == NULL) {
        fclose(fp);
        send_simple(client, "500 Internal Server Error", "text/plain", "oom", 3);
        return;
    }
    if (fread(buf, 1, size, fp) != size) {
        free(buf);
        fclose(fp);
        send_simple(client, "500 Internal Server Error", "text/plain", "io", 2);
        return;
    }
    fclose(fp);

    snprintf(header, sizeof(header),
             "HTTP/1.0 200 OK\r\nContent-Type: image/jpeg\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n",
             size);
    send_all(client, header, strlen(header));
    send_all(client, buf, size);
    free(buf);
}

static void handle_control(int client, app_state_t *st, const char *query)
{
    const char *action = query;
    char body[128];
    int running;
    int muted;
    int n;

    /* query 形如 "action=toggle" */
    if (strncmp(query, "action=", 7) == 0) {
        action = query + 7;
    }

    pthread_mutex_lock(&st->lock);
    if (strcmp(action, "pause") == 0) {
        st->inspection_running = 0;
    } else if (strcmp(action, "resume") == 0) {
        st->inspection_running = 1;
    } else if (strcmp(action, "toggle") == 0) {
        st->inspection_running = !st->inspection_running;
    } else if (strcmp(action, "mute") == 0) {
        st->buzzer_muted = !st->buzzer_muted;
    } else {
        pthread_mutex_unlock(&st->lock);
        send_simple(client, "400 Bad Request", "text/plain", "bad action", 10);
        return;
    }
    running = st->inspection_running;
    muted = st->buzzer_muted;
    n = snprintf(body, sizeof(body),
                 "{\"ok\":true,\"running\":%s,\"muted\":%s}",
                 running ? "true" : "false", muted ? "true" : "false");
    pthread_mutex_unlock(&st->lock);

    log_info("http control: action=%s running=%d", action, running);
    if (n < 0) {
        return;
    }
    send_simple(client, "200 OK", "application/json", body, (size_t)n);
}

/* ---------- 客户端线程 ---------- */

static void *client_thread(void *arg)
{
    int client = (int)(intptr_t)arg;
    char req[2048];
    char method[8];
    char path[768];
    char query[512];
    ssize_t n;
    app_state_t *st = state_get();
    struct timeval tv;

    /* 收发超时，防止慢速连接占用线程 */
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    n = read(client, req, sizeof(req) - 1);
    if (n <= 0) {
        close(client);
        return NULL;
    }
    req[n] = '\0';

    method[0] = path[0] = query[0] = '\0';
    if (sscanf(req, "%7s %767s", method, path) < 2) {
        close(client);
        return NULL;
    }
    /* 分离查询字符串 */
    {
        char *q = strchr(path, '?');
        if (q != NULL) {
            snprintf(query, sizeof(query), "%s", q + 1);
            *q = '\0';
        }
    }

    if (strcmp(path, "/status.json") == 0) {
        handle_status(client, st);
    } else if (strcmp(path, "/stream") == 0) {
        handle_stream(client, st);
    } else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        handle_index(client);
    } else if (strcmp(path, "/snapshots.json") == 0) {
        handle_snapshots_list(client, st);
    } else if (strcmp(path, "/snapshot.jpg") == 0) {
        handle_snapshot_file(client, st, query);
    } else if (strcmp(path, "/control") == 0) {
        handle_control(client, st, query);
    } else if (strcmp(path, "/favicon.ico") == 0) {
        send_simple(client, "204 No Content", "image/x-icon", "", 0);
    } else {
        const char *msg = "HTTP/1.0 404 Not Found\r\nConnection: close\r\n\r\n";
        send_all(client, msg, strlen(msg));
    }

    close(client);
    return NULL;
}

void *http_server_thread(void *argument)
{
    int server;
    struct sockaddr_in addr;
    int yes = 1;
    app_state_t *st = state_get();
    unsigned short port;

    (void)argument;

    /* 端口取自运行期配置（GRANARY_HTTP_PORT，state_init 已解析） */
    pthread_mutex_lock(&st->lock);
    port = st->cfg.http_port;
    pthread_mutex_unlock(&st->lock);
    if (port == 0) {
        port = HTTP_SERVER_PORT;
    }

    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        log_error("http socket: %s", strerror(errno));
        return NULL;
    }
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("http bind %u: %s", (unsigned)port, strerror(errno));
        close(server);
        return NULL;
    }
    if (listen(server, 8) < 0) {
        log_error("http listen: %s", strerror(errno));
        close(server);
        return NULL;
    }

    log_info("HTTP server listening on 0.0.0.0:%u (open http://BOARD_IP:%u/)",
             (unsigned)port, (unsigned)port);

    while (!st->stop) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);
        int client;
        pthread_t tid;
        fd_set fds;
        struct timeval tv;

        state_heartbeat(APP_THREAD_HTTP);

        FD_ZERO(&fds);
        FD_SET(server, &fds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        if (select(server + 1, &fds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        client = accept(server, (struct sockaddr *)&cli, &clilen);
        if (client < 0) {
            continue;
        }
        if (pthread_create(&tid, NULL, client_thread,
                           (void *)(intptr_t)client) == 0) {
            pthread_detach(tid);
        } else {
            close(client);
        }
    }

    close(server);
    log_info("HTTP server stopped");
    return NULL;
}
