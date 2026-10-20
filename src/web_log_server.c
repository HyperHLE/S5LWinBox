#include "web_log_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include "platform_compat.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define S5L_INVALID_SOCKET INVALID_SOCKET
#define s5l_sock_close(s) closesocket(s)
#define s5l_sock_send(s, b, l) send(s, (const char *)(b), (int)(l), 0)
#define s5l_poll(p, n, t) WSAPoll(p, (ULONG)(n), t)
#define s5l_sock_err() WSAGetLastError()
#define S5L_EWOULDBLOCK WSAEWOULDBLOCK
#define S5L_ECONNRESET WSAECONNRESET
#else
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
typedef int socket_t;
#define S5L_INVALID_SOCKET (-1)
#define s5l_sock_close(s) close(s)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#define s5l_sock_send(s, b, l) send(s, (const void *)(b), (size_t)(l), MSG_DONTWAIT | MSG_NOSIGNAL)
#define s5l_poll(p, n, t) poll(p, (nfds_t)(n), t)
#define s5l_sock_err() errno
#define S5L_EWOULDBLOCK EWOULDBLOCK
#define S5L_ECONNRESET ECONNRESET
#endif

#define RING_BUFFER_CAP  8192u
#define MAX_CLIENTS      8u
#define MAX_LINE_LEN     512u

typedef struct {
    uint32_t id;
    char time_str[16];
    char category[16];
    char text[MAX_LINE_LEN];
} log_entry_t;

typedef struct {
    socket_t fd;
    bool is_sse;
} client_t;

static const char HTML_PAGE[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"<title>S5LWinBox &bull; Live Inspector</title>\n"
"<style>\n"
"* { box-sizing: border-box; margin: 0; padding: 0; }\n"
"body {\n"
"  background-color: #0d1117;\n"
"  color: #c9d1d9;\n"
"  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;\n"
"  height: 100vh;\n"
"  display: flex;\n"
"  flex-direction: column;\n"
"}\n"
"header {\n"
"  background: #161b22;\n"
"  border-bottom: 1px solid #30363d;\n"
"  padding: 10px 16px;\n"
"  display: flex;\n"
"  align-items: center;\n"
"  justify-content: space-between;\n"
"  gap: 12px;\n"
"  flex-wrap: wrap;\n"
"}\n"
".brand {\n"
"  display: flex;\n"
"  align-items: center;\n"
"  gap: 10px;\n"
"  font-weight: 700;\n"
"  font-size: 1.05rem;\n"
"  color: #f0f6fc;\n"
"}\n"
".badge {\n"
"  font-size: 0.72rem;\n"
"  padding: 3px 8px;\n"
"  border-radius: 12px;\n"
"  font-weight: 600;\n"
"  text-transform: uppercase;\n"
"  letter-spacing: 0.5px;\n"
"}\n"
".badge.live { background: #238636; color: #fff; }\n"
".badge.reconn { background: #d29922; color: #000; }\n"
".stats-bar {\n"
"  display: flex;\n"
"  align-items: center;\n"
"  gap: 16px;\n"
"  font-size: 0.82rem;\n"
"  color: #8b949e;\n"
"}\n"
".stat-item strong { color: #58a6ff; font-weight: 600; }\n"
".toolbar {\n"
"  background: #161b22;\n"
"  border-bottom: 1px solid #30363d;\n"
"  padding: 8px 16px;\n"
"  display: flex;\n"
"  align-items: center;\n"
"  gap: 10px;\n"
"  flex-wrap: wrap;\n"
"}\n"
".filter-group {\n"
"  display: flex;\n"
"  gap: 4px;\n"
"  background: #0d1117;\n"
"  padding: 2px;\n"
"  border-radius: 6px;\n"
"  border: 1px solid #30363d;\n"
"}\n"
".filter-btn {\n"
"  background: transparent;\n"
"  color: #8b949e;\n"
"  border: none;\n"
"  padding: 4px 10px;\n"
"  border-radius: 4px;\n"
"  font-size: 0.8rem;\n"
"  font-weight: 500;\n"
"  cursor: pointer;\n"
"  transition: all 0.15s;\n"
"}\n"
".filter-btn:hover { color: #c9d1d9; }\n"
".filter-btn.active { background: #1f6feb; color: #fff; }\n"
".search-input {\n"
"  background: #0d1117;\n"
"  border: 1px solid #30363d;\n"
"  border-radius: 6px;\n"
"  padding: 4px 10px;\n"
"  font-size: 0.82rem;\n"
"  color: #c9d1d9;\n"
"  flex: 1;\n"
"  min-width: 140px;\n"
"  outline: none;\n"
"}\n"
".search-input:focus { border-color: #58a6ff; }\n"
".action-btn {\n"
"  background: #21262d;\n"
"  color: #c9d1d9;\n"
"  border: 1px solid #30363d;\n"
"  padding: 4px 12px;\n"
"  border-radius: 6px;\n"
"  font-size: 0.8rem;\n"
"  font-weight: 500;\n"
"  cursor: pointer;\n"
"}\n"
".action-btn:hover { background: #30363d; border-color: #8b949e; }\n"
"#log-container {\n"
"  flex: 1;\n"
"  overflow-y: auto;\n"
"  padding: 8px 12px;\n"
"  font-family: 'SFMono-Regular', Consolas, 'Liberation Mono', Menlo, monospace;\n"
"  font-size: 0.8rem;\n"
"  line-height: 1.45;\n"
"}\n"
".log-row {\n"
"  display: flex;\n"
"  padding: 1px 4px;\n"
"  border-radius: 3px;\n"
"  word-break: break-all;\n"
"}\n"
".log-row:hover { background: #161b22; }\n"
".log-time { color: #8b949e; min-width: 90px; flex-shrink: 0; user-select: none; }\n"
".log-cat {\n"
"  display: inline-block;\n"
"  padding: 0 6px;\n"
"  border-radius: 3px;\n"
"  font-weight: 600;\n"
"  font-size: 0.72rem;\n"
"  text-align: center;\n"
"  margin-right: 8px;\n"
"  flex-shrink: 0;\n"
"  min-width: 60px;\n"
"}\n"
".cat-kernel  { background: #1f6feb22; color: #58a6ff; border: 1px solid #1f6feb55; }\n"
".cat-panic   { background: #da363322; color: #f85149; border: 1px solid #da363355; font-weight: bold; }\n"
".cat-hle     { background: #23863622; color: #56d364; border: 1px solid #23863655; }\n"
".cat-mbx     { background: #a371f722; color: #bc8cff; border: 1px solid #a371f755; }\n"
".cat-dyld    { background: #d2992222; color: #e3b341; border: 1px solid #d2992255; }\n"
".cat-info    { background: #8b949e22; color: #8b949e; border: 1px solid #8b949e55; }\n"
".cat-warn    { background: #d2992222; color: #d29922; border: 1px solid #d2992255; }\n"
".cat-error   { background: #da363322; color: #f85149; border: 1px solid #da363355; }\n"
".log-msg { color: #c9d1d9; flex: 1; }\n"
".log-row.panic .log-msg { color: #f85149; font-weight: 600; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<header>\n"
"  <div class=\"brand\">\n"
"    <span>&#x1F4F1; S5LWinBox</span>\n"
"    <span id=\"conn-badge\" class=\"badge reconn\">CONNECTING</span>\n"
"  </div>\n"
"  <div class=\"stats-bar\">\n"
"    <div class=\"stat-item\">FPS: <strong id=\"stat-fps\">0.0</strong></div>\n"
"    <div class=\"stat-item\">MIPS: <strong id=\"stat-mips\">0.0</strong></div>\n"
"    <div class=\"stat-item\">RETIRED: <strong id=\"stat-retired\">0</strong></div>\n"
"    <div class=\"stat-item\">SPACE: <strong id=\"stat-space\">0x0</strong></div>\n"
"    <div class=\"stat-item\">STATE: <strong id=\"stat-state\">idle</strong></div>\n"
"  </div>\n"
"</header>\n"
"<div class=\"toolbar\">\n"
"  <div class=\"filter-group\">\n"
"    <button class=\"filter-btn active\" data-cat=\"all\">All</button>\n"
"    <button class=\"filter-btn\" data-cat=\"kernel\">Kernel</button>\n"
"    <button class=\"filter-btn\" data-cat=\"panic\">Panic</button>\n"
"    <button class=\"filter-btn\" data-cat=\"hle\">HLE</button>\n"
"    <button class=\"filter-btn\" data-cat=\"mbx\">MBX</button>\n"
"    <button class=\"filter-btn\" data-cat=\"dyld\">dyld</button>\n"
"  </div>\n"
"  <input type=\"text\" id=\"search-box\" class=\"search-input\" placeholder=\"Filter logs...\">\n"
"  <button id=\"btn-autoscroll\" class=\"action-btn active\">Auto-scroll: ON</button>\n"
"  <button id=\"btn-clear\" class=\"action-btn\">Clear</button>\n"
"  <a href=\"/log.txt\" download=\"s5lwinbox_log.txt\" style=\"text-decoration:none;\">\n"
"    <button class=\"action-btn\">Export Log</button>\n"
"  </a>\n"
"</div>\n"
"<div id=\"log-container\"></div>\n"
"<script>\n"
"let autoScroll = true;\n"
"let activeFilter = 'all';\n"
"let filterText = '';\n"
"const container = document.getElementById('log-container');\n"
"const badge = document.getElementById('conn-badge');\n"
"const btnAuto = document.getElementById('btn-autoscroll');\n"
"const searchBox = document.getElementById('search-box');\n"
"const statFps = document.getElementById('stat-fps');\n"
"const statMips = document.getElementById('stat-mips');\n"
"const statRetired = document.getElementById('stat-retired');\n"
"const statSpace = document.getElementById('stat-space');\n"
"const statState = document.getElementById('stat-state');\n"
"\n"
"btnAuto.onclick = () => {\n"
"  autoScroll = !autoScroll;\n"
"  btnAuto.textContent = 'Auto-scroll: ' + (autoScroll ? 'ON' : 'OFF');\n"
"  btnAuto.classList.toggle('active', autoScroll);\n"
"};\n"
"document.getElementById('btn-clear').onclick = () => { container.innerHTML = ''; };\n"
"\n"
"document.querySelectorAll('.filter-btn').forEach(btn => {\n"
"  btn.onclick = () => {\n"
"    document.querySelectorAll('.filter-btn').forEach(b => b.classList.remove('active'));\n"
"    btn.classList.add('active');\n"
"    activeFilter = btn.dataset.cat;\n"
"    applyFilters();\n"
"  };\n"
"});\n"
"\n"
"searchBox.oninput = (e) => {\n"
"  filterText = e.target.value.toLowerCase();\n"
"  applyFilters();\n"
"};\n"
"\n"
"function applyFilters() {\n"
"  const rows = container.querySelectorAll('.log-row');\n"
"  for (const r of rows) {\n"
"    const cat = r.dataset.cat;\n"
"    const text = r.textContent.toLowerCase();\n"
"    const catMatch = (activeFilter === 'all' || cat === activeFilter);\n"
"    const textMatch = (!filterText || text.includes(filterText));\n"
"    r.style.display = (catMatch && textMatch) ? 'flex' : 'none';\n"
"  }\n"
"}\n"
"\n"
"function appendLog(entry) {\n"
"  const row = document.createElement('div');\n"
"  row.className = 'log-row' + (entry.cat === 'panic' ? ' panic' : '');\n"
"  row.dataset.cat = entry.cat;\n"
"  \n"
"  const catClass = 'cat-' + entry.cat.toLowerCase();\n"
"  row.innerHTML = `<span class=\"log-time\">${entry.time}</span>` +\n"
"                  `<span class=\"log-cat ${catClass}\">${entry.cat.toUpperCase()}</span>` +\n"
"                  `<span class=\"log-msg\"></span>`;\n"
"  row.querySelector('.log-msg').textContent = entry.msg;\n"
"  \n"
"  const catMatch = (activeFilter === 'all' || entry.cat === activeFilter);\n"
"  const textMatch = (!filterText || row.textContent.toLowerCase().includes(filterText));\n"
"  row.style.display = (catMatch && textMatch) ? 'flex' : 'none';\n"
"  \n"
"  container.appendChild(row);\n"
"  if (container.children.length > 5000) {\n"
"    container.removeChild(container.firstChild);\n"
"  }\n"
"  if (autoScroll) {\n"
"    container.scrollTop = container.scrollHeight;\n"
"  }\n"
"}\n"
"\n"
"function connectSSE() {\n"
"  const es = new EventSource('/events');\n"
"  es.onopen = () => {\n"
"    badge.className = 'badge live';\n"
"    badge.textContent = 'LIVE';\n"
"  };\n"
"  es.onmessage = (e) => {\n"
"    try {\n"
"      const d = JSON.parse(e.data);\n"
"      appendLog(d);\n"
"    } catch (err) {}\n"
"  };\n"
"  es.onerror = () => {\n"
"    badge.className = 'badge reconn';\n"
"    badge.textContent = 'RECONNECTING';\n"
"  };\n"
"}\n"
"\n"
"async function pollStatus() {\n"
"  try {\n"
"    const res = await fetch('/api/status');\n"
"    if (res.ok) {\n"
"      const s = await res.json();\n"
"      if (s.fps !== undefined) statFps.textContent = s.fps.toFixed(1);\n"
"      if (s.mips !== undefined) statMips.textContent = s.mips.toFixed(1);\n"
"      if (s.state !== undefined) statState.textContent = s.state;\n"
"      if (s.retired !== undefined) statRetired.textContent = s.retired;\n"
"      if (s.ttbr0 !== undefined) statSpace.textContent = s.ttbr0;\n"
"    }\n"
"  } catch (e) {}\n"
"}\n"
"\n"
"connectSSE();\n"
"setInterval(pollStatus, 1000);\n"
"</script>\n"
"</body>\n"
"</html>\n";

static pthread_t s_thread;
static volatile bool s_running = false;
static uint16_t s_port = 0;
static socket_t s_listen_fd = S5L_INVALID_SOCKET;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static log_entry_t s_ring[RING_BUFFER_CAP];
static uint32_t s_head = 0u;
static uint32_t s_count = 0u;
static uint32_t s_seq = 0u;

static client_t s_clients[MAX_CLIENTS];

/* Live status */
static uint64_t s_status_retired = 0u;
static double s_status_fps = 0.0;
static double s_status_mips = 0.0;
static uint32_t s_status_ttbr0 = 0u;
static char s_status_state[32] = "idle";

static void format_now(char *out, size_t cap)
{
#if defined(_WIN32)
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(out, cap, "%02d:%02d:%02d.%03d",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    snprintf(out, cap, "%02d:%02d:%02d.%03ld",
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L);
#endif
}

static void escape_json_string(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 6 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"') {
            out[o++] = '\\'; out[o++] = '"';
        } else if (c == '\\') {
            out[o++] = '\\'; out[o++] = '\\';
        } else if (c == '\n') {
            out[o++] = '\\'; out[o++] = 'n';
        } else if (c == '\r') {
            out[o++] = '\\'; out[o++] = 'r';
        } else if (c == '\t') {
            out[o++] = '\\'; out[o++] = 't';
        } else if (c < 0x20) {
            snprintf(out + o, cap - o, "\\u%04x", c);
            o += 6;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

void web_log_post(const char *category, const char *msg)
{
    if (!msg || !msg[0]) return;
    char time_buf[16];
    format_now(time_buf, sizeof time_buf);

    pthread_mutex_lock(&s_lock);
    uint32_t idx = (s_head + s_count) % RING_BUFFER_CAP;
    if (s_count < RING_BUFFER_CAP) {
        s_count++;
    } else {
        s_head = (s_head + 1u) % RING_BUFFER_CAP;
    }
    log_entry_t *entry = &s_ring[idx];
    entry->id = ++s_seq;
    snprintf(entry->time_str, sizeof entry->time_str, "%s", time_buf);
    snprintf(entry->category, sizeof entry->category, "%s", category ? category : "info");
    snprintf(entry->text, sizeof entry->text, "%s", msg);

    /* Broadcast to active SSE clients */
    char esc_msg[MAX_LINE_LEN * 2];
    escape_json_string(msg, esc_msg, sizeof esc_msg);
    char packet[MAX_LINE_LEN * 3];
    int len = snprintf(packet, sizeof packet,
                       "data: {\"id\":%u,\"time\":\"%s\",\"cat\":\"%s\",\"msg\":\"%s\"}\n\n",
                       entry->id, entry->time_str, entry->category, esc_msg);

    if (len > 0) {
        for (unsigned i = 0; i < MAX_CLIENTS; i++) {
            if (s_clients[i].fd != S5L_INVALID_SOCKET && s_clients[i].is_sse) {
                int ret = s5l_sock_send(s_clients[i].fd, packet, len);
                if (ret < 0 && s5l_sock_err() == S5L_ECONNRESET) {
                    s5l_sock_close(s_clients[i].fd);
                    s_clients[i].fd = S5L_INVALID_SOCKET;
                    s_clients[i].is_sse = false;
                }
            }
        }
    }
    pthread_mutex_unlock(&s_lock);
}

static char s_raw_pending[MAX_LINE_LEN];
static size_t s_raw_pending_len = 0;
static char s_raw_pending_cat[16] = "kernel";

void web_log_flush_raw(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_raw_pending_len > 0) {
        s_raw_pending[s_raw_pending_len] = '\0';
        s_raw_pending_len = 0;
        char cat[16];
        snprintf(cat, sizeof cat, "%s", s_raw_pending_cat);
        pthread_mutex_unlock(&s_lock);
        web_log_post(cat, s_raw_pending);
        return;
    }
    pthread_mutex_unlock(&s_lock);
}

void web_log_post_raw(const char *category, const char *data, size_t len)
{
    if (!data || len == 0) return;
    const char *cat = category ? category : "kernel";

    pthread_mutex_lock(&s_lock);
    if (s_raw_pending_len > 0 && strcmp(s_raw_pending_cat, cat) != 0) {
        s_raw_pending[s_raw_pending_len] = '\0';
        char prev_cat[16];
        snprintf(prev_cat, sizeof prev_cat, "%s", s_raw_pending_cat);
        s_raw_pending_len = 0;
        pthread_mutex_unlock(&s_lock);
        web_log_post(prev_cat, s_raw_pending);
        pthread_mutex_lock(&s_lock);
    }
    snprintf(s_raw_pending_cat, sizeof s_raw_pending_cat, "%s", cat);

    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            s_raw_pending[s_raw_pending_len] = '\0';
            if (s_raw_pending_len > 0) {
                pthread_mutex_unlock(&s_lock);
                web_log_post(cat, s_raw_pending);
                pthread_mutex_lock(&s_lock);
            }
            s_raw_pending_len = 0;
        } else {
            if (s_raw_pending_len + 1 < sizeof s_raw_pending) {
                s_raw_pending[s_raw_pending_len++] = c;
            } else {
                s_raw_pending[s_raw_pending_len] = '\0';
                pthread_mutex_unlock(&s_lock);
                web_log_post(cat, s_raw_pending);
                pthread_mutex_lock(&s_lock);
                s_raw_pending_len = 0;
                s_raw_pending[s_raw_pending_len++] = c;
            }
        }
    }
    pthread_mutex_unlock(&s_lock);
}

void web_log_printf(const char *category, const char *fmt, ...)
{
    char buf[MAX_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    web_log_post(category, buf);
}

void web_log_set_status(uint64_t retired, double fps, double mips, uint32_t ttbr0, const char *state)
{
    pthread_mutex_lock(&s_lock);
    s_status_retired = retired;
    s_status_fps = fps;
    s_status_mips = mips;
    s_status_ttbr0 = ttbr0;
    if (state && state[0]) {
        snprintf(s_status_state, sizeof s_status_state, "%s", state);
    }
    pthread_mutex_unlock(&s_lock);
}

size_t web_log_get_entries_json(uint32_t last_id, char *out, size_t capacity)
{
    if (!out || capacity < 3) return 0;
    pthread_mutex_lock(&s_lock);

    size_t written = 0;
    written += snprintf(out + written, capacity - written, "[");

    bool first = true;
    for (uint32_t i = 0; i < s_count; i++) {
        uint32_t idx = (s_head + i) % RING_BUFFER_CAP;
        log_entry_t *entry = &s_ring[idx];
        if (entry->id <= last_id) continue;

        char esc_msg[MAX_LINE_LEN * 2];
        escape_json_string(entry->text, esc_msg, sizeof esc_msg);

        char item[MAX_LINE_LEN * 3];
        int n = snprintf(item, sizeof item, "%s{\"id\":%u,\"time\":\"%s\",\"cat\":\"%s\",\"msg\":\"%s\"}",
                         first ? "" : ",", entry->id, entry->time_str, entry->category, esc_msg);
        if (n > 0 && written + (size_t)n + 2 < capacity) {
            memcpy(out + written, item, (size_t)n);
            written += (size_t)n;
            first = false;
        } else {
            break;
        }
    }

    if (written + 2 < capacity) {
        out[written++] = ']';
        out[written] = '\0';
    } else {
        out[capacity - 2] = ']';
        out[capacity - 1] = '\0';
        written = capacity - 1;
    }

    pthread_mutex_unlock(&s_lock);
    return written;
}

static void send_sse_backlog(socket_t fd)
{
    pthread_mutex_lock(&s_lock);
    uint32_t start = s_count > 200u ? s_count - 200u : 0u;
    for (uint32_t i = start; i < s_count; i++) {
        uint32_t idx = (s_head + i) % RING_BUFFER_CAP;
        log_entry_t *entry = &s_ring[idx];

        char esc_msg[MAX_LINE_LEN * 2];
        escape_json_string(entry->text, esc_msg, sizeof esc_msg);
        char packet[MAX_LINE_LEN * 3];
        int len = snprintf(packet, sizeof packet,
                           "data: {\"id\":%u,\"time\":\"%s\",\"cat\":\"%s\",\"msg\":\"%s\"}\n\n",
                           entry->id, entry->time_str, entry->category, esc_msg);
        if (len > 0) {
            (void)s5l_sock_send(fd, packet, len);
        }
    }
    pthread_mutex_unlock(&s_lock);
}

static void handle_client_request(socket_t fd, int slot)
{
    char req[2048];
    int n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) {
        s5l_sock_close(fd);
        s_clients[slot].fd = S5L_INVALID_SOCKET;
        s_clients[slot].is_sse = false;
        return;
    }
    req[n] = '\0';

    if (strncmp(req, "GET /events", 11) == 0) {
        const char header[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n\r\n";
        (void)s5l_sock_send(fd, header, sizeof(header) - 1);
        s_clients[slot].is_sse = true;
        send_sse_backlog(fd);
    } else if (strncmp(req, "GET /log.txt", 12) == 0) {
        const char header[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Disposition: attachment; filename=\"s5lwinbox_log.txt\"\r\n"
            "Connection: close\r\n\r\n";
        (void)s5l_sock_send(fd, header, sizeof(header) - 1);
        pthread_mutex_lock(&s_lock);
        for (uint32_t i = 0; i < s_count; i++) {
            uint32_t idx = (s_head + i) % RING_BUFFER_CAP;
            log_entry_t *entry = &s_ring[idx];
            char line[MAX_LINE_LEN + 64];
            int len = snprintf(line, sizeof line, "[%s] [%-6s] %s\n",
                               entry->time_str, entry->category, entry->text);
            if (len > 0)
                (void)s5l_sock_send(fd, line, len);
        }
        pthread_mutex_unlock(&s_lock);
        s5l_sock_close(fd);
        s_clients[slot].fd = S5L_INVALID_SOCKET;
    } else if (strncmp(req, "GET /api/status", 15) == 0) {
        char body[256];
        pthread_mutex_lock(&s_lock);
        int body_len = snprintf(body, sizeof body,
            "{\"retired\":%llu,\"fps\":%.2f,\"mips\":%.2f,\"ttbr0\":\"0x%08x\",\"state\":\"%s\"}",
            (unsigned long long)s_status_retired, s_status_fps, s_status_mips,
            s_status_ttbr0, s_status_state);
        pthread_mutex_unlock(&s_lock);

        char header[256];
        int hlen = snprintf(header, sizeof header,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n\r\n", body_len);
        (void)s5l_sock_send(fd, header, hlen);
        (void)s5l_sock_send(fd, body, body_len);
        s5l_sock_close(fd);
        s_clients[slot].fd = S5L_INVALID_SOCKET;
    } else {
        /* Default: serve HTML UI */
        char header[256];
        int hlen = snprintf(header, sizeof header,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n", sizeof(HTML_PAGE) - 1);
        (void)s5l_sock_send(fd, header, hlen);
        (void)s5l_sock_send(fd, HTML_PAGE, sizeof(HTML_PAGE) - 1);
        s5l_sock_close(fd);
        s_clients[slot].fd = S5L_INVALID_SOCKET;
    }
}

static void *server_thread_func(void *arg)
{
    (void)arg;
#if defined(_WIN32)
    WSAPOLLFD pfds[MAX_CLIENTS + 1];
#else
    struct pollfd pfds[MAX_CLIENTS + 1];
#endif
    time_t last_ping = time(NULL);

    while (s_running) {
        int nfds = 0;

        /* pfds[0] = s_listen_fd */
        pfds[nfds].fd = s_listen_fd;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        int listen_idx = nfds++;

        int client_map[MAX_CLIENTS];
        for (unsigned i = 0; i < MAX_CLIENTS; i++) {
            if (s_clients[i].fd != S5L_INVALID_SOCKET) {
                client_map[i] = nfds;
                pfds[nfds].fd = s_clients[i].fd;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                nfds++;
            } else {
                client_map[i] = -1;
            }
        }

        int ret = s5l_poll(pfds, nfds, 250);
        if (!s_running) break;
        if (ret < 0) {
            continue;
        }

        /* New connection */
        if (pfds[listen_idx].revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof client_addr;
            socket_t client_fd = accept(s_listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd != S5L_INVALID_SOCKET) {
#if defined(_WIN32)
                u_long nonblock = 1;
                ioctlsocket(client_fd, FIONBIO, &nonblock);
#else
                int flags = fcntl(client_fd, F_GETFL, 0);
                if (flags >= 0) (void)fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
#endif
                int slot = -1;
                for (unsigned i = 0; i < MAX_CLIENTS; i++) {
                    if (s_clients[i].fd == S5L_INVALID_SOCKET) { slot = (int)i; break; }
                }
                if (slot >= 0) {
                    s_clients[slot].fd = client_fd;
                    s_clients[slot].is_sse = false;
                } else {
                    s5l_sock_close(client_fd);
                }
            }
        }

        /* Check client events */
        for (unsigned i = 0; i < MAX_CLIENTS; i++) {
            int p_idx = client_map[i];
            if (p_idx >= 0 && (pfds[p_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
                if (pfds[p_idx].revents & POLLIN) {
                    handle_client_request(s_clients[i].fd, (int)i);
                } else {
                    s5l_sock_close(s_clients[i].fd);
                    s_clients[i].fd = S5L_INVALID_SOCKET;
                    s_clients[i].is_sse = false;
                }
            }
        }

        /* Periodic SSE heartbeat ping (every 15s) */
        time_t now = time(NULL);
        if (now - last_ping >= 15) {
            last_ping = now;
            pthread_mutex_lock(&s_lock);
            const char ping_msg[] = ": ping\n\n";
            for (unsigned i = 0; i < MAX_CLIENTS; i++) {
                if (s_clients[i].fd != S5L_INVALID_SOCKET && s_clients[i].is_sse) {
                    int s = s5l_sock_send(s_clients[i].fd, ping_msg, sizeof(ping_msg) - 1);
                    if (s < 0 && s5l_sock_err() == S5L_ECONNRESET) {
                        s5l_sock_close(s_clients[i].fd);
                        s_clients[i].fd = S5L_INVALID_SOCKET;
                        s_clients[i].is_sse = false;
                    }
                }
            }
            pthread_mutex_unlock(&s_lock);
        }
    }

    return NULL;
}

uint16_t web_log_server_start(uint16_t preferred_port)
{
    if (s_running) return s_port;

#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
#endif

    for (unsigned i = 0; i < MAX_CLIENTS; i++) {
        s_clients[i].fd = S5L_INVALID_SOCKET;
        s_clients[i].is_sse = false;
    }

    uint16_t try_port = preferred_port ? preferred_port : 8080u;
    socket_t fd = S5L_INVALID_SOCKET;

    for (uint16_t p = try_port; p <= try_port + 10u; p++) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == S5L_INVALID_SOCKET) continue;

        int opt = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof opt);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(p);

        if (bind(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
            if (listen(fd, 8) == 0) {
                s_port = p;
                s_listen_fd = fd;
                break;
            }
        }
        s5l_sock_close(fd);
        fd = S5L_INVALID_SOCKET;
    }

    if (fd == S5L_INVALID_SOCKET) {
#if defined(_WIN32)
        WSACleanup();
#endif
        return 0;
    }

    s_running = true;
    if (pthread_create(&s_thread, NULL, server_thread_func, NULL) != 0) {
        s_running = false;
        s5l_sock_close(s_listen_fd);
        s_listen_fd = S5L_INVALID_SOCKET;
        s_port = 0;
#if defined(_WIN32)
        WSACleanup();
#endif
        return 0;
    }

    web_log_printf("info", "Web log inspector started on port %u (http://<ip>:%u)", s_port, s_port);
    return s_port;
}

void web_log_server_stop(void)
{
    if (!s_running) return;
    web_log_flush_raw();
    s_running = false;

    pthread_join(s_thread, NULL);

    if (s_listen_fd != S5L_INVALID_SOCKET) {
        s5l_sock_close(s_listen_fd);
        s_listen_fd = S5L_INVALID_SOCKET;
    }

    for (unsigned i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].fd != S5L_INVALID_SOCKET) {
            s5l_sock_close(s_clients[i].fd);
            s_clients[i].fd = S5L_INVALID_SOCKET;
            s_clients[i].is_sse = false;
        }
    }
    s_port = 0;

#if defined(_WIN32)
    WSACleanup();
#endif
}

uint16_t web_log_server_get_port(void)
{
    return s_port;
}
