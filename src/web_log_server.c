#include "web_log_server.h"
#include "engine.h"
#include "VMButtonQueue.h"

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
"<title>S5LWinBox / S5LPCBox &bull; Web Inspector & Manager</title>\n"
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
"  padding: 8px 16px;\n"
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
".nav-tabs {\n"
"  display: flex;\n"
"  gap: 4px;\n"
"  background: #0d1117;\n"
"  padding: 3px;\n"
"  border-radius: 6px;\n"
"  border: 1px solid #30363d;\n"
"}\n"
".tab-btn {\n"
"  background: transparent;\n"
"  color: #8b949e;\n"
"  border: none;\n"
"  padding: 5px 12px;\n"
"  border-radius: 4px;\n"
"  font-size: 0.82rem;\n"
"  font-weight: 600;\n"
"  cursor: pointer;\n"
"  transition: all 0.15s;\n"
"}\n"
".tab-btn:hover { color: #c9d1d9; }\n"
".tab-btn.active { background: #1f6feb; color: #fff; }\n"
".stats-bar {\n"
"  display: flex;\n"
"  align-items: center;\n"
"  gap: 14px;\n"
"  font-size: 0.82rem;\n"
"  color: #8b949e;\n"
"}\n"
".stat-item strong { color: #58a6ff; font-weight: 600; }\n"
".tab-pane { display: none; flex: 1; flex-direction: column; overflow: hidden; }\n"
".tab-pane.active { display: flex; }\n"
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
".action-btn.primary { background: #238636; color: #fff; border-color: #2ea043; }\n"
".action-btn.primary:hover { background: #2ea043; }\n"
".action-btn.danger { background: #da3633; color: #fff; border-color: #f85149; }\n"
".action-btn.danger:hover { background: #f85149; }\n"
"#log-container {\n"
"  flex: 1;\n"
"  overflow-y: auto;\n"
"  padding: 8px 12px;\n"
"  font-family: 'SFMono-Regular', Consolas, 'Liberation Mono', Menlo, monospace;\n"
"  font-size: 0.8rem;\n"
"  line-height: 1.45;\n"
"}\n"
".log-row { display: flex; padding: 1px 4px; border-radius: 3px; word-break: break-all; }\n"
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
"/* Machine Manager */\n"
".mm-wrapper { padding: 20px; overflow-y: auto; flex: 1; max-width: 900px; margin: 0 auto; width: 100%; }\n"
".mm-top { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }\n"
".mm-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: 16px; }\n"
".m-card { background: #161b22; border: 1px solid #30363d; border-radius: 8px; padding: 16px; display: flex; flex-direction: column; gap: 10px; }\n"
".m-card.is-running { border-color: #238636; box-shadow: 0 0 10px rgba(35,134,54,0.2); }\n"
".m-header { display: flex; justify-content: space-between; align-items: center; font-weight: 700; font-size: 1.05rem; }\n"
".m-meta { font-size: 0.82rem; color: #8b949e; line-height: 1.5; font-family: monospace; }\n"
".m-actions { display: flex; gap: 8px; margin-top: auto; padding-top: 8px; }\n"
"/* Remote Phone */\n"
".remote-wrapper { display: flex; justify-content: center; align-items: center; padding: 24px; flex: 1; overflow-y: auto; }\n"
".phone-body { background: #161b22; border: 2px solid #30363d; border-radius: 36px; padding: 24px; width: 320px; display: flex; flex-direction: column; align-items: center; gap: 16px; box-shadow: 0 12px 36px rgba(0,0,0,0.6); }\n"
".phone-screen { width: 100%; height: 260px; background: #000; border-radius: 12px; border: 1px solid #21262d; display: flex; flex-direction: column; align-items: center; justify-content: center; padding: 16px; text-align: center; gap: 10px; color: #8b949e; }\n"
".phone-btn-row { display: flex; gap: 8px; width: 100%; justify-content: center; }\n"
".phone-key { background: #21262d; border: 1px solid #30363d; color: #f0f6fc; border-radius: 8px; padding: 10px 14px; font-weight: 600; font-size: 0.85rem; cursor: pointer; transition: all 0.1s; flex: 1; text-align: center; }\n"
".phone-key:hover { background: #30363d; border-color: #58a6ff; }\n"
".phone-key:active { background: #1f6feb; transform: translateY(1px); }\n"
".home-btn { width: 62px; height: 62px; border-radius: 31px; background: #21262d; border: 2px solid #30363d; display: flex; align-items: center; justify-content: center; cursor: pointer; transition: all 0.15s; }\n"
".home-btn:hover { border-color: #58a6ff; background: #30363d; }\n"
".home-btn:active { background: #1f6feb; transform: scale(0.96); }\n"
".home-sq { width: 18px; height: 18px; border: 2px solid #c9d1d9; border-radius: 4px; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<header>\n"
"  <div class=\"brand\">\n"
"    <span>&#x1F4F1; S5LWinBox</span>\n"
"    <span id=\"conn-badge\" class=\"badge reconn\">CONNECTING</span>\n"
"  </div>\n"
"  <div class=\"nav-tabs\">\n"
"    <button class=\"tab-btn active\" onclick=\"switchTab('logs')\">&#x1F4CB; Logs</button>\n"
"    <button class=\"tab-btn\" onclick=\"switchTab('machines')\">&#x1F4F1; Machines</button>\n"
"    <button class=\"tab-btn\" onclick=\"switchTab('remote')\">&#x1F3AE; Remote</button>\n"
"  </div>\n"
"  <div class=\"stats-bar\">\n"
"    <div class=\"stat-item\">FPS: <strong id=\"stat-fps\">0.0</strong></div>\n"
"    <div class=\"stat-item\">MIPS: <strong id=\"stat-mips\">0.0</strong></div>\n"
"    <div class=\"stat-item\">RETIRED: <strong id=\"stat-retired\">0</strong></div>\n"
"    <div class=\"stat-item\">STATE: <strong id=\"stat-state\">idle</strong></div>\n"
"  </div>\n"
"</header>\n"
"\n"
"<div id=\"tab-logs\" class=\"tab-pane active\">\n"
"  <div class=\"toolbar\">\n"
"    <div class=\"filter-group\">\n"
"      <button class=\"filter-btn active\" data-cat=\"all\">All</button>\n"
"      <button class=\"filter-btn\" data-cat=\"kernel\">Kernel</button>\n"
"      <button class=\"filter-btn\" data-cat=\"panic\">Panic</button>\n"
"      <button class=\"filter-btn\" data-cat=\"hle\">HLE</button>\n"
"      <button class=\"filter-btn\" data-cat=\"mbx\">MBX</button>\n"
"      <button class=\"filter-btn\" data-cat=\"dyld\">dyld</button>\n"
"    </div>\n"
"    <input type=\"text\" id=\"search-box\" class=\"search-input\" placeholder=\"Filter logs...\">\n"
"    <button id=\"btn-autoscroll\" class=\"action-btn active\">Auto-scroll: ON</button>\n"
"    <button id=\"btn-clear\" class=\"action-btn\">Clear</button>\n"
"    <a href=\"/log.txt\" download=\"s5lwinbox_log.txt\" style=\"text-decoration:none;\">\n"
"      <button class=\"action-btn\">Export Log</button>\n"
"    </a>\n"
"  </div>\n"
"  <div id=\"log-container\"></div>\n"
"</div>\n"
"\n"
"<div id=\"tab-machines\" class=\"tab-pane\">\n"
"  <div class=\"mm-wrapper\">\n"
"    <div class=\"mm-top\">\n"
"      <h2>Virtual Machines</h2>\n"
"      <button class=\"action-btn primary\" onclick=\"promptCreateMachine()\">+ Create Machine</button>\n"
"    </div>\n"
"    <div id=\"mm-grid\" class=\"mm-grid\">Loading machines...</div>\n"
"  </div>\n"
"</div>\n"
"\n"
"<div id=\"tab-remote\" class=\"tab-pane\">\n"
"  <div class=\"remote-wrapper\">\n"
"    <div class=\"phone-body\">\n"
"      <div class=\"phone-btn-row\">\n"
"        <button class=\"phone-key\" onclick=\"btnAction('power')\">&#x23FB; Power</button>\n"
"        <button class=\"phone-key\" onclick=\"btnAction('mute')\">&#x1F515; Mute</button>\n"
"      </div>\n"
"      <div class=\"phone-screen\">\n"
"        <h3 id=\"remote-m-name\" style=\"color:#f0f6fc;\">iPhone 3G</h3>\n"
"        <p id=\"remote-status-text\">State: Running</p>\n"
"        <p id=\"remote-stats-detail\" style=\"font-size:0.8rem;\">MIPS: 22.0 | FPS: 60.0</p>\n"
"      </div>\n"
"      <div class=\"phone-btn-row\">\n"
"        <button class=\"phone-key\" onclick=\"btnAction('voldown')\">&#x1F509; Vol -</button>\n"
"        <button class=\"phone-key\" onclick=\"btnAction('volup')\">&#x1F50A; Vol +</button>\n"
"      </div>\n"
"      <div class=\"phone-btn-row\">\n"
"        <button class=\"phone-key\" onclick=\"btnAction('pause')\">&#x23F8; Pause</button>\n"
"        <button class=\"phone-key\" onclick=\"btnAction('reset')\">&#x1F504; Reboot</button>\n"
"      </div>\n"
"      <div class=\"home-btn\" onclick=\"btnAction('home')\">\n"
"        <div class=\"home-sq\"></div>\n"
"      </div>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
"\n"
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
"const statState = document.getElementById('stat-state');\n"
"\n"
"function switchTab(name) {\n"
"  document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));\n"
"  document.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));\n"
"  event.target.classList.add('active');\n"
"  document.getElementById('tab-' + name).classList.add('active');\n"
"  if (name === 'machines') loadMachines();\n"
"}\n"
"\n"
"async function btnAction(btn) {\n"
"  try {\n"
"    await fetch('/api/button?btn=' + btn);\n"
"  } catch(e) {}\n"
"}\n"
"\n"
"async function loadMachines() {\n"
"  const grid = document.getElementById('mm-grid');\n"
"  try {\n"
"    const res = await fetch('/api/machines');\n"
"    const data = await res.json();\n"
"    if (!data.machines || data.machines.length === 0) {\n"
"      grid.innerHTML = '<p>No machines found.</p>';\n"
"      return;\n"
"    }\n"
"    grid.innerHTML = data.machines.map(m => {\n"
"      const isRun = (m.id === data.runningId);\n"
"      return `<div class=\"m-card ${isRun ? 'is-running' : ''}\">` +\n"
"        `<div class=\"m-header\"><span>${m.name}</span>` +\n"
"        `<span class=\"badge ${isRun ? 'live' : 'reconn'}\">${isRun ? 'RUNNING' : 'IDLE'}</span></div>` +\n"
"        `<div class=\"m-meta\">ID: ${m.id}<br>Disk: ${m.diskGiB}GB<br>Retired: ${(m.retiredTotal/1e6).toFixed(1)}M</div>` +\n"
"        `<div class=\"m-actions\">` +\n"
"        `<button class=\"action-btn primary\" onclick=\"startMachine('${m.id}')\">${isRun ? 'Restart' : 'Boot'}</button>` +\n"
"        `${isRun ? `<button class=\"action-btn danger\" onclick=\"stopMachine()\">Stop</button>` : `<button class=\"action-btn danger\" onclick=\"deleteMachine('${m.id}')\">Delete</button>`}` +\n"
"        `</div></div>`;\n"
"    }).join('');\n"
"  } catch(e) {\n"
"    grid.innerHTML = '<p>Failed to load machines.</p>';\n"
"  }\n"
"}\n"
"\n"
"async function startMachine(id) {\n"
"  await fetch('/api/machines/start?id=' + id);\n"
"  setTimeout(loadMachines, 600);\n"
"}\n"
"async function stopMachine() {\n"
"  await fetch('/api/machines/stop');\n"
"  setTimeout(loadMachines, 600);\n"
"}\n"
"async function deleteMachine(id) {\n"
"  if (confirm('Delete machine ' + id + '?')) {\n"
"    await fetch('/api/machines/delete?id=' + id);\n"
"    setTimeout(loadMachines, 600);\n"
"  }\n"
"}\n"
"async function promptCreateMachine() {\n"
"  const name = prompt('Enter machine name:', 'iPhone 3G');\n"
"  if (name) {\n"
"    await fetch('/api/machines/create?name=' + encodeURIComponent(name) + '&disk=8');\n"
"    setTimeout(loadMachines, 600);\n"
"  }\n"
"}\n"
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
"  const catClass = 'cat-' + entry.cat.toLowerCase();\n"
"  row.innerHTML = `<span class=\"log-time\">${entry.time}</span>` +\n"
"                  `<span class=\"log-cat ${catClass}\">${entry.cat.toUpperCase()}</span>` +\n"
"                  `<span class=\"log-msg\"></span>`;\n"
"  row.querySelector('.log-msg').textContent = entry.msg;\n"
"  const catMatch = (activeFilter === 'all' || entry.cat === activeFilter);\n"
"  const textMatch = (!filterText || row.textContent.toLowerCase().includes(filterText));\n"
"  row.style.display = (catMatch && textMatch) ? 'flex' : 'none';\n"
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
"      if (s.state !== undefined) {\n"
"        statState.textContent = s.state;\n"
"        const rText = document.getElementById('remote-status-text');\n"
"        if (rText) rText.textContent = 'State: ' + s.state;\n"
"      }\n"
"      if (s.retired !== undefined) {\n"
"        statRetired.textContent = (s.retired >= 1e9) ? (s.retired/1e9).toFixed(2)+'G' : (s.retired/1e6).toFixed(1)+'M';\n"
"      }\n"
"      const rDetail = document.getElementById('remote-stats-detail');\n"
"      if (rDetail && s.mips !== undefined && s.fps !== undefined) {\n"
"        rDetail.textContent = 'MIPS: ' + s.mips.toFixed(1) + ' | FPS: ' + s.fps.toFixed(1);\n"
"      }\n"
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

static engine_t *s_engine = NULL;

void web_log_server_set_engine(struct engine *engine)
{
    s_engine = (engine_t *)engine;
}

static void s5l_sleep_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    usleep(ms * 1000u);
#endif
}

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
            "{\"retired\":%llu,\"fps\":%.2f,\"mips\":%.2f,\"ttbr0\":\"0x%08x\",\"state\":\"%s\",\"jit\":%s}",
            (unsigned long long)s_status_retired, s_status_fps, s_status_mips,
            s_status_ttbr0, s_status_state,
            (s_engine && engine_is_jit_enabled(s_engine)) ? "true" : "false");
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
    } else if (strncmp(req, "GET /api/machines/start", 23) == 0 || strncmp(req, "POST /api/machines/start", 24) == 0) {
        char id[64] = {0};
        const char *p = strstr(req, "id=");
        if (p) {
            p += 3;
            size_t i = 0;
            while (p[i] && p[i] != ' ' && p[i] != '&' && p[i] != '\r' && p[i] != '\n' && i < sizeof(id) - 1) {
                id[i] = p[i];
                i++;
            }
            id[i] = '\0';
        }
        if (s_engine && id[0]) {
            char detail[256] = {0};
            engine_stop(s_engine, ENGINE_STOP_PLAIN);
            engine_start(s_engine, id, detail, sizeof detail);
        }
        const char body[] = "{\"ok\":true}";
        int body_len = (int)sizeof(body) - 1;
        char header[256];
        int hlen = snprintf(header, sizeof header,
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", body_len);
        (void)s5l_sock_send(fd, header, hlen);
        (void)s5l_sock_send(fd, body, body_len);
        s5l_sock_close(fd);
        s_clients[slot].fd = S5L_INVALID_SOCKET;
    } else if (strncmp(req, "GET /api/machines/stop", 22) == 0 || strncmp(req, "POST /api/machines/stop", 23) == 0) {
        if (s_engine) {
            engine_stop(s_engine, ENGINE_STOP_PLAIN);
        }
        const char body[] = "{\"ok\":true}";
        int body_len = (int)sizeof(body) - 1;
        char header[256];
        int hlen = snprintf(header, sizeof header,
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", body_len);
        (void)s5l_sock_send(fd, header, hlen);
        (void)s5l_sock_send(fd, body, body_len);
        s5l_sock_close(fd);
        s_clients[slot].fd = S5L_INVALID_SOCKET;
    } else if (strncmp(req, "GET /api/machines/delete", 24) == 0 || strncmp(req, "POST /api/machines/delete", 25) == 0) {
        char id[64] = {0};
        const char *p = strstr(req, "id=");
        if (p) {
            p += 3;
            size_t i = 0;
            while (p[i] && p[i] != ' ' && p[i] != '&' && p[i] != '\r' && p[i] != '\n' && i < sizeof(id) - 1) {
                id[i] = p[i];
                i++;
            }
            id[i] = '\0';
        }
        char detail[256] = {0};
        int rc = -1;
        if (s_engine && id[0]) {
            rc = engine_machine_delete(s_engine, id, detail, sizeof detail);
        }
        char body[256];
        int body_len = snprintf(body, sizeof body, "{\"ok\":%s,\"detail\":\"%s\"}", rc == 0 ? "true" : "false", detail);
        char header[256];
        int hlen = snprintf(header, sizeof header,
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", body_len);
        (void)s5l_sock_send(fd, header, hlen);
        (void)s5l_sock_send(fd, body, body_len);
        s5l_sock_close(fd);
        s_clients[slot].fd = S5L_INVALID_SOCKET;
    } else if (strncmp(req, "GET /api/machines/create", 24) == 0 || strncmp(req, "POST /api/machines/create", 25) == 0) {
        char name[64] = "iPhone 3G";
        unsigned disk = 8;
        const char *np = strstr(req, "name=");
        if (np) {
            np += 5;
            size_t i = 0;
            while (np[i] && np[i] != ' ' && np[i] != '&' && np[i] != '\r' && np[i] != '\n' && i < sizeof(name) - 1) {
                if (np[i] == '+') name[i] = ' ';
                else name[i] = np[i];
                i++;
            }
            name[i] = '\0';
        }
        const char *dp = strstr(req, "disk=");
        if (dp) {
            disk = (unsigned)strtoul(dp + 5, NULL, 10);
            if (disk != 2 && disk != 4 && disk != 8) disk = 8;
        }
        char new_id[64] = {0};
        char detail[256] = {0};
        int rc = -1;
        if (s_engine) {
            rc = engine_machine_create(s_engine, name, NULL, 0, disk, new_id, sizeof new_id, detail, sizeof detail);
        }
        char body[256];
        int body_len = snprintf(body, sizeof body, "{\"ok\":%s,\"id\":\"%s\",\"detail\":\"%s\"}", rc == 0 ? "true" : "false", new_id, detail);
        char header[256];
        int hlen = snprintf(header, sizeof header,
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", body_len);
        (void)s5l_sock_send(fd, header, hlen);
        (void)s5l_sock_send(fd, body, body_len);
        s5l_sock_close(fd);
        s_clients[slot].fd = S5L_INVALID_SOCKET;
    } else if (strncmp(req, "GET /api/machines", 17) == 0) {
        char body[8192];
        int body_len = 0;
        if (s_engine) {
            body_len = engine_machines_json(s_engine, body, sizeof body);
        }
        if (body_len <= 0) {
            body_len = snprintf(body, sizeof body, "{\"ok\":false,\"machines\":[]}");
        }
        char header[256];
        int hlen = snprintf(header, sizeof header,
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", body_len);
        (void)s5l_sock_send(fd, header, hlen);
        (void)s5l_sock_send(fd, body, body_len);
        s5l_sock_close(fd);
        s_clients[slot].fd = S5L_INVALID_SOCKET;
    } else if (strncmp(req, "GET /api/button", 15) == 0 || strncmp(req, "POST /api/button", 16) == 0) {
        if (s_engine) {
            if (strstr(req, "btn=home")) {
                engine_button(s_engine, VM_BUTTON_HOME, true);
                s5l_sleep_ms(60);
                engine_button(s_engine, VM_BUTTON_HOME, false);
            } else if (strstr(req, "btn=power")) {
                engine_button(s_engine, VM_BUTTON_POWER, true);
                s5l_sleep_ms(60);
                engine_button(s_engine, VM_BUTTON_POWER, false);
            } else if (strstr(req, "btn=volup")) {
                engine_button(s_engine, VM_BUTTON_VOLUME_UP, true);
                s5l_sleep_ms(60);
                engine_button(s_engine, VM_BUTTON_VOLUME_UP, false);
            } else if (strstr(req, "btn=voldown")) {
                engine_button(s_engine, VM_BUTTON_VOLUME_DOWN, true);
                s5l_sleep_ms(60);
                engine_button(s_engine, VM_BUTTON_VOLUME_DOWN, false);
            } else if (strstr(req, "btn=mute")) {
                static bool s_web_mute = false;
                s_web_mute = !s_web_mute;
                engine_button(s_engine, VM_BUTTON_RINGER_SILENT, s_web_mute);
            } else if (strstr(req, "btn=pause")) {
                static bool s_web_paused = false;
                s_web_paused = !s_web_paused;
                engine_set_paused(s_engine, s_web_paused);
            } else if (strstr(req, "btn=reset")) {
                char cur_id[64] = {0};
                engine_get_or_create_default_machine(s_engine, cur_id, sizeof cur_id);
                engine_stop(s_engine, ENGINE_STOP_PLAIN);
                char detail[256] = {0};
                engine_start(s_engine, cur_id, detail, sizeof detail);
            }
        }
        const char body[] = "{\"ok\":true}";
        int body_len = (int)sizeof(body) - 1;
        char header[256];
        int hlen = snprintf(header, sizeof header,
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", body_len);
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
