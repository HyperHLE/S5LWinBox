#ifndef WEB_LOG_SERVER_H
#define WEB_LOG_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start web log server on preferred_port (tries up to preferred_port + 9 if busy).
 * Returns the actual port bound to, or 0 on failure. */
uint16_t web_log_server_start(uint16_t preferred_port);

/* Stop the server and close all client connections. */
void web_log_server_stop(void);

/* Returns the active listening port, or 0 if inactive. */
uint16_t web_log_server_get_port(void);

/* Post a message to the live log stream and ring buffer.
 * Categories: "kernel", "gles", "dyld", "crash", "info". */
void web_log_post(const char *category, const char *msg);

/* Post raw text (e.g. UART chunk), buffering until newline. */
void web_log_post_raw(const char *category, const char *data, size_t len);

/* Flush any pending incomplete line in the raw buffer. */
void web_log_flush_raw(void);

/* Formatted log post. */
void web_log_printf(const char *category, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Update emulator status for the /api/status endpoint. */
void web_log_set_status(uint64_t retired, double fps, double mips,
                        uint32_t ttbr0, const char *state);

/* Returns JSON array of log entries newer than last_id:
 * [{"id":12,"time":"12:34:56.789","cat":"kernel","msg":"..."}, ...]
 * Writes up to capacity bytes into out, null-terminated. Returns length. */
size_t web_log_get_entries_json(uint32_t last_id, char *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* WEB_LOG_SERVER_H */
