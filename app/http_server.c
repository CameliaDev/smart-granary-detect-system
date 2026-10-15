#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "http_server.h"
#include "state.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static const char *HTML_PAGE =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"/>"
    "<title>Smart Granary Monitor</title>"
    "<style>"
    "body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:16px}"
    "img{max-width:100%;border:2px solid #444}"
    "#overlay{white-space:pre-wrap;background:#222;padding:12px;margin-top:12px;"
    "border-radius:8px;font-size:14px}"
    ".alarm{color:#f66;font-weight:bold}"
    "</style></head><body>"
    "<h1>智能粮仓监控 / Smart Granary</h1>"
    "<img id=\"v\" src=\"/stream\" alt=\"live\"/>"
    "<div id=\"overlay\">loading...</div>"
    "<script>"
    "async function tick(){"
    "try{const r=await fetch('/status.json');const j=await r.json();"
    "const el=document.getElementById('overlay');"
    "el.className=j.any_alarm?'alarm':'';"
    "el.textContent="
    "'Mode: '+(j.running?'RUNNING':'PAUSED')+'\\n'+"
    "'U='+j.U_V+'V I='+j.I_A+'A P='+j.P_W+'W\\n'+"
    "'T='+j.T_C+'C RH='+j.RH+'% Lux='+j.lux+'\\n'+"
    "'PIR='+j.pir+' Flame='+j.flame+'\\n'+"
    "'E01='+j.E01+' E02='+j.E02+' E03='+j.E03+' E04='+j.E04+' E05='+j.E05+'\\n'+"
    "'Alarm: '+j.alarm+'\\nAI: '+j.ai;"
    "}catch(e){}"
    "setTimeout(tick,1000);}"
    "tick();"
    "</script></body></html>";


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

static void handle_status(int client, app_state_t *st)
{
    char body[4096];
    char header[256];
    int any;
    int n;

    char alarm_e[STATE_ALARM_TEXT_MAX * 2];
    char ai_e[STATE_AI_TEXT_MAX * 2];
    char overlay_e[STATE_OVERLAY_MAX * 2];

    pthread_mutex_lock(&st->lock);
    any = st->e01 || st->e02 || st->e03 || st->e04 || st->e05;
    json_escape(st->alarm_text, alarm_e, sizeof(alarm_e));
    json_escape(st->ai_text, ai_e, sizeof(ai_e));
    json_escape(st->overlay_text, overlay_e, sizeof(overlay_e));
    n = snprintf(
        body, sizeof(body),
        "{\"running\":%s,\"U_V\":%.3f,\"I_A\":%.3f,\"P_W\":%.3f,"
        "\"T_C\":%.2f,\"RH\":%.2f,\"lux\":%.1f,\"pir\":%s,\"flame\":%s,"
        "\"E01\":%d,\"E02\":%d,\"E03\":%d,\"E04\":%d,\"E05\":%d,"
        "\"any_alarm\":%s,\"alarm\":\"%s\",\"ai\":\"%s\",\"overlay\":\"%s\"}",
        st->inspection_running ? "true" : "false", st->supply_voltage_v,
        st->current_a, st->power_w, st->temperature_c, st->humidity_rh,
        st->illuminance, st->pir_detected ? "true" : "false",
        st->flame_detected ? "true" : "false", st->e01, st->e02, st->e03,
        st->e04, st->e05, any ? "true" : "false", alarm_e, ai_e, overlay_e);
    pthread_mutex_unlock(&st->lock);

    if (n < 0) {
        return;
    }
    /* naive escape: strip quotes already unlikely; truncate */
    snprintf(header, sizeof(header),
             "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
             "Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\n"
             "Connection: close\r\n\r\n",
             n);
    send_all(client, header, strlen(header));
    send_all(client, body, (size_t)n);
}

static void handle_index(int client)
{
    char header[256];
    int len = (int)strlen(HTML_PAGE);

    snprintf(header, sizeof(header),
             "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
             "Content-Length: %d\r\nConnection: close\r\n\r\n",
             len);
    send_all(client, header, strlen(header));
    send_all(client, HTML_PAGE, (size_t)len);
}

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
            /* Pause stream: send a short wait, keep connection */
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

static void *client_thread(void *arg)
{
    int client = (int)(intptr_t)arg;
    char req[1024];
    ssize_t n;
    app_state_t *st = state_get();

    n = read(client, req, sizeof(req) - 1);
    if (n <= 0) {
        close(client);
        return NULL;
    }
    req[n] = '\0';

    if (strncmp(req, "GET /status.json", 16) == 0) {
        handle_status(client, st);
    } else if (strncmp(req, "GET /stream", 11) == 0) {
        handle_stream(client, st);
    } else if (strncmp(req, "GET / ", 6) == 0 ||
               strncmp(req, "GET /index", 10) == 0) {
        handle_index(client);
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
    unsigned short port = HTTP_SERVER_PORT;

    (void)argument;

    {
        const char *env_port = getenv("GRANARY_HTTP_PORT");
        if (env_port != NULL && env_port[0] != '\0') {
            int p = atoi(env_port);
            if (p > 0 && p < 65536) {
                port = (unsigned short)p;
            }
        }
    }

    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        perror("http socket");
        return NULL;
    }
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("http bind");
        close(server);
        return NULL;
    }
    if (listen(server, 8) < 0) {
        perror("http listen");
        close(server);
        return NULL;
    }

    printf("HTTP server listening on 0.0.0.0:%u (open http://BOARD_IP:%u/)\n",
           port, port);

    while (!st->stop) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);
        int client;
        pthread_t tid;
        fd_set fds;
        struct timeval tv;

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
    puts("HTTP server stopped");
    return NULL;
}
