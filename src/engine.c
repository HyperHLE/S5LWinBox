/*
 * S5LWinBox — the emulator thread and engine orchestration.
 */
#include "engine.h"
#include "platform_compat.h"
#include <SDL.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>
#include <dlfcn.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifndef O_BINARY
#  ifdef _O_BINARY
#    define O_BINARY _O_BINARY
#  else
#    define O_BINARY 0
#  endif
#endif

#include "soc.h"
#include "usb_host.h"

#include "VMButtonQueue.h"
#include "VMFirmwareBoot.h"
#include "VMFirmwareImport.h"
#include "VMFirmwareKeys.h"
#include "VMGuest.h"
#include "VMGuestInstallBuild.h"
#include "VMDiskSize.h"
#include "engine_ipa.h"
#include "VMGuestPackageManifest.h"
#include "VMInstancePaths.h"
#include "VMInstances.h"
#include "VMOptions.h"
#include "VMResumeCheckpoint.h"
#include "VMSnapshotCow.h"
#include "VMSnapshotStore.h"
#include "VMTouchMap.h"
#include "VMTouchQueue.h"
#include "VMAudioBuffer.h"
#include "file_block.h"
#include "web_log_server.h"
#include "ios3_hle.h"
#if defined(S5LBOX_DYNARMIC_JIT)
#include "dynarmic_jit.h"
#endif

#define ENGINE_LOG_TAG "S5LWinBox"
#define ENGINE_LOG(...) do { printf("[INFO] " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define ENGINE_WARN(...) do { fprintf(stderr, "[WARN] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while (0)

#define ENGINE_CHUNK_INSTRUCTIONS 100000u
#define ENGINE_PUBLISH_SECONDS (1.0 / 60.0)
#define ENGINE_CONSOLE_CAP 16000u
#define ENGINE_PATH_CAP VM_FW_BOOT_PATH_CAPACITY
#define ENGINE_IMPORT_REPORT_CAP 4096u
#define ENGINE_AUDIO_SCRATCH 1024u
#define ENGINE_COW_HEADER_BYTES 28u

static bool engine_new_machine_option_value(unsigned index)
{
    return vm_option_new_desktop_machine_value(index, false, false);
}

enum {
    PHASE_IDLE = 0,
    PHASE_STARTING,
    PHASE_RUNNING,
    PHASE_PAUSED,
    PHASE_STOPPING,
    PHASE_ERROR
};

typedef struct import_sink import_sink_t;
struct import_sink {
    FILE *file;
    char path[ENGINE_PATH_CAP];
};

struct engine {
    char root[ENGINE_PATH_CAP];
    char firmware[ENGINE_PATH_CAP];
    char machines[ENGINE_PATH_CAP];

    pthread_mutex_t lock;
    pthread_t thread;
    bool thread_live;
    bool join_pending;
    int phase;

    bool stop_requested;
    engine_stop_mode_t stop_mode;
    bool paused;
    bool machine_ready;
    bool preparing_root;
    _Atomic bool abort_flag;
    _Atomic uint64_t prepare_done;
    _Atomic uint64_t prepare_total;
    _Atomic uint64_t instruction_cap;

    char instance_id[VM_INSTANCE_ID_LEN + 1u];
    bool options[VM_INSTANCE_OPTION_MAX];
    unsigned option_count;

    char status[192];
    char mode[128];
    char note[320];
    uint64_t retired;
    double rate;
    double fps;
    double fps_window_start;
    uint64_t fps_window_frames;
    arm_status_t last_arm_status;

    uint8_t *snapshot;
    bool have_frame;
    bool snapshot_fresh;
    bool snapshot_blank;
    bool snapshot_argb;
    uint32_t snap_w, snap_h, snap_stride;

    char console[ENGINE_CONSOLE_CAP];
    size_t console_len;
    char machine_log_path[ENGINE_PATH_CAP];

    vm_touch_queue_t touch;
    vm_touch_delivery_state_t touch_delivery;
    vm_touch_tracker_t tracker;
    vm_button_queue_t button_queue;
    bool buttons[VM_BUTTON_COUNT];
    bool ringer_silent;
    vm_button_power_hold_t power_hold;
    vm_button_momentary_holds_t momentary;
    uint64_t touch_delivered, touch_dropped;
    uint64_t button_delivered, button_dropped;

    vm_audio_buffer_t audio;
    bool audio_ok;
    float audio_left[ENGINE_AUDIO_SCRATCH];
    float audio_right[ENGINE_AUDIO_SCRATCH];
    _Atomic bool microphone_on;

    vm_instance_list_t instances;

    _Atomic bool import_active;
    _Atomic bool import_cancel;
    _Atomic bool import_ok;
    _Atomic int import_artefact;
    _Atomic int import_stage;
    _Atomic uint64_t import_done;
    _Atomic uint64_t import_total;
    char import_report[ENGINE_IMPORT_REPORT_CAP];
    char import_source[VM_FW_BOOT_DETAIL_CAPACITY];

    _Atomic bool install_active;
    _Atomic int install_phase;
    _Atomic uint64_t install_done;
    _Atomic uint64_t install_total;

    vm_firmware_boot_t *boot;

    bool usb_active;
    bool usb_ready;
    usb_host_t usb_host;
};

/* ------------------------------------------------------------------ misc --- */

static double engine_now(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    static bool init = false;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        init = true;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static uint64_t engine_now_ns(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    static bool init = false;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        init = true;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000000000ULL) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
#endif
}

static uint64_t engine_unix_now(void)
{
    return (uint64_t)time(NULL);
}

static bool is_directory(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool mkdir_p(const char *path)
{
    char tmp[ENGINE_PATH_CAP];
    size_t len;
    if (!path || !path[0]) return false;
    len = strlen(path);
    if (len >= sizeof tmp) return false;
    memcpy(tmp, path, len + 1u);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        if (p == tmp + 2 && tmp[1] == ':') continue; /* Skip C:\ drive root */
        char sep = *p;
        *p = '\0';
        if (mkdir(tmp, 0770) != 0 && errno != EEXIST) {
            *p = sep;
            if (!is_directory(tmp)) return false;
        } else {
            *p = sep;
        }
    }
    if (mkdir(tmp, 0770) != 0 && errno != EEXIST) {
        if (!is_directory(tmp)) return false;
    }
    return is_directory(tmp);
}

static bool join_path(char *out, size_t cap, const char *dir, const char *leaf)
{
    int n;
    if (!out || !dir || !leaf || cap == 0) return false;
    n = snprintf(out, cap, "%s/%s", dir, leaf);
    return n > 0 && (size_t)n < cap;
}

static bool file_present(const char *path, uint64_t *size)
{
    struct stat st;
    if (!path || stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
        return false;
    if (size) *size = (uint64_t)st.st_size;
    return true;
}

static int rm_rf(const char *path)
{
    return portable_rm_rf(path);
}

static uint64_t directory_size(const char *path)
{
#if defined(_WIN32)
    if (!path || !path[0]) return 0;
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return 0;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        char search_path[MAX_PATH];
        snprintf(search_path, sizeof(search_path), "%s\\*.*", path);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(search_path, &fd);
        uint64_t total = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                char child[MAX_PATH];
                snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
                total += directory_size(child);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        return total;
    } else {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
            return ((uint64_t)fad.nFileSizeHigh << 32) | (uint64_t)fad.nFileSizeLow;
        }
        return 0;
    }
#else
    struct stat st;
    uint64_t total = 0;
    if (!path || lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
    {
        DIR *d = opendir(path);
        struct dirent *ent;
        if (!d) return 0;
        while ((ent = readdir(d)) != NULL) {
            char child[ENGINE_PATH_CAP];
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            if (!join_path(child, sizeof child, path, ent->d_name)) continue;
            total += directory_size(child);
        }
        closedir(d);
    }
    return total;
#endif
}

static bool copy_file(const char *source, const char *destination)
{
    int in = -1, out = -1;
    uint8_t *buffer = NULL;
    bool ok = false;
    struct stat st;
    if (!source || !destination) return false;
    in = open(source, O_RDONLY | O_CLOEXEC);
    if (in < 0) goto done;
    if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) goto done;
    out = open(destination, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
    if (out < 0) goto done;
    buffer = malloc(1024u * 1024u);
    if (!buffer) goto done;
    for (;;) {
        ssize_t got = read(in, buffer, 1024u * 1024u);
        if (got < 0) {
            if (errno == EINTR) continue;
            goto done;
        }
        if (got == 0) break;
        {
            size_t written = 0;
            while (written < (size_t)got) {
                ssize_t put = write(out, buffer + written, (size_t)got - written);
                if (put < 0) {
                    if (errno == EINTR) continue;
                    goto done;
                }
                written += (size_t)put;
            }
        }
    }
    if (fsync(out) != 0) goto done;
    ok = true;
done:
    free(buffer);
    if (out >= 0) close(out);
    if (in >= 0) close(in);
    if (!ok && destination) unlink(destination);
    return ok;
}

static bool write_marker(const char *path)
{
    static const char text[] = "restore\n";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
    bool ok;
    if (fd < 0) return false;
    ok = write(fd, text, sizeof text - 1u) == (ssize_t)(sizeof text - 1u);
    if (ok && fsync(fd) != 0) ok = false;
    close(fd);
    if (!ok) unlink(path);
    return ok;
}

static void make_instance_id(engine_t *e, char *out)
{
    static uint64_t counter;
    uint64_t x = engine_now_ns() ^ ((uint64_t)getpid() << 32);
    x ^= ++counter * UINT64_C(0x9E3779B97F4A7C15);
    x ^= (uint64_t)(uintptr_t)e;
    /* xorshift64* so the low sixteen nibbles are all visible entropy. */
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    x *= UINT64_C(2685821657736338717);
    snprintf(out, VM_INSTANCE_ID_LEN + 1u, "%016llx",
             (unsigned long long)x);
}

/* ------------------------------------------------------------- JSON build --- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} json_t;

static void json_reset(json_t *j, char *buf, size_t cap)
{
    j->buf = buf;
    j->cap = cap;
    j->len = 0;
    if (buf && cap) buf[0] = '\0';
}

static void json_raw(json_t *j, const char *text)
{
    size_t n;
    if (!j->buf || j->cap == 0 || !text) return;
    n = strlen(text);
    if (j->len + n >= j->cap) n = j->cap - 1u - j->len;
    memcpy(j->buf + j->len, text, n);
    j->len += n;
    j->buf[j->len] = '\0';
}

static void json_printf(json_t *j, const char *format, ...)
{
    char temp[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(temp, sizeof temp, format, args);
    va_end(args);
    json_raw(j, temp);
}

static void json_string(json_t *j, const char *text)
{
    char escaped[8];
    json_raw(j, "\"");
    if (text) {
        for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
            switch (*p) {
            case '"':  json_raw(j, "\\\""); break;
            case '\\': json_raw(j, "\\\\"); break;
            case '\n': json_raw(j, "\\n"); break;
            case '\r': json_raw(j, "\\r"); break;
            case '\t': json_raw(j, "\\t"); break;
            default:
                if (*p < 0x20) {
                    snprintf(escaped, sizeof escaped, "\\u%04x", *p);
                    json_raw(j, escaped);
                } else {
                    char one[2] = { (char)*p, '\0' };
                    json_raw(j, one);
                }
                break;
            }
        }
    }
    json_raw(j, "\"");
}

/* ------------------------------------------------------------- consoles --- */

static void console_append_len_locked(engine_t *e, const char *text, size_t n)
{
    if (!text || n == 0) return;
    if (e->machine_log_path[0]) {
        FILE *lf = fopen(e->machine_log_path, "ab");
        if (lf) {
            fwrite(text, 1, n, lf);
            fclose(lf);
        }
    }
    if (n >= ENGINE_CONSOLE_CAP) {
        text += n - (ENGINE_CONSOLE_CAP - 1u);
        n = ENGINE_CONSOLE_CAP - 1u;
    }
    if (e->console_len + n > ENGINE_CONSOLE_CAP - 1u) {
        size_t drop = e->console_len + n - (ENGINE_CONSOLE_CAP - 1u);
        memmove(e->console, e->console + drop, e->console_len - drop);
        e->console_len -= drop;
    }
    memcpy(e->console + e->console_len, text, n);
    e->console_len += n;
    e->console[e->console_len] = '\0';
}

static void console_append_locked(engine_t *e, const char *text)
{
    if (!text) return;
    console_append_len_locked(e, text, strlen(text));
}

static void console_append(engine_t *e, const char *text)
{
    pthread_mutex_lock(&e->lock);
    console_append_locked(e, text);
    pthread_mutex_unlock(&e->lock);
}

static void console_printf(engine_t *e, const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    console_append(e, line);
    web_log_post("info", line);
}

/* -------------------------------------------------------- hyperhle gles --- */

typedef struct GlesHandle GlesHandle;

static GlesHandle *(*p_s5l_gles_create)(int32_t api, void *native_window, uint32_t width, uint32_t height) = NULL;
static void (*p_s5l_gles_destroy)(GlesHandle *handle) = NULL;
static int32_t (*p_s5l_gles_make_current)(GlesHandle *handle) = NULL;
static int32_t (*p_s5l_gles_swap_buffers)(GlesHandle *handle) = NULL;
static void (*p_s5l_gles_clear_color)(GlesHandle *handle, float r, float g, float b, float a) = NULL;
static void (*p_s5l_gles_clear)(GlesHandle *handle, uint32_t mask) = NULL;
static void (*p_s5l_gles_viewport)(GlesHandle *handle, int32_t x, int32_t y, int32_t width, int32_t height) = NULL;
static void (*p_s5l_gles_draw_arrays)(GlesHandle *handle, uint32_t mode, int32_t first, int32_t count) = NULL;
static const char *(*p_s5l_gles_get_string)(GlesHandle *handle, uint32_t name) = NULL;

static s5l_dynlib_t s_hyperhle_lib = NULL;
static GlesHandle *s_hyperhle_ctx = NULL;

static void hyperhle_dispatch_clear(uint32_t mask) {
    if (s_hyperhle_ctx && p_s5l_gles_clear) {
        p_s5l_gles_clear(s_hyperhle_ctx, mask);
        web_log_printf("gles", "glClear(0x%04x)", mask);
    }
}

static void hyperhle_dispatch_clear_color(float r, float g, float b, float a) {
    if (s_hyperhle_ctx && p_s5l_gles_clear_color) {
        p_s5l_gles_clear_color(s_hyperhle_ctx, r, g, b, a);
        web_log_printf("gles", "glClearColor(%.2f, %.2f, %.2f, %.2f)", r, g, b, a);
    }
}

static void hyperhle_dispatch_viewport(int32_t x, int32_t y, int32_t width, int32_t height) {
    if (s_hyperhle_ctx && p_s5l_gles_viewport) {
        p_s5l_gles_viewport(s_hyperhle_ctx, x, y, width, height);
        web_log_printf("gles", "glViewport(%d, %d, %d, %d)", x, y, width, height);
    }
}

static void hyperhle_dispatch_draw_arrays(uint32_t mode, int32_t first, int32_t count) {
    if (s_hyperhle_ctx && p_s5l_gles_draw_arrays) {
        p_s5l_gles_draw_arrays(s_hyperhle_ctx, mode, first, count);
        web_log_printf("gles", "glDrawArrays(0x%x, first=%d, count=%d)", mode, first, count);
    }
}

static const char *hyperhle_dispatch_get_string(uint32_t name) {
    if (s_hyperhle_ctx && p_s5l_gles_get_string) {
        const char *s = p_s5l_gles_get_string(s_hyperhle_ctx, name);
        web_log_printf("gles", "glGetString(0x%x) -> %s", name, s ? s : "(null)");
        return s;
    }
    return NULL;
}

static const ios3_hle_gles_dispatch_t s_hyperhle_dispatch = {
    hyperhle_dispatch_clear,
    hyperhle_dispatch_clear_color,
    hyperhle_dispatch_viewport,
    hyperhle_dispatch_get_string,
    hyperhle_dispatch_draw_arrays
};

static void init_hyperhle_gles(engine_t *e) {
    (void)e;
    if (s_hyperhle_lib) return;
#if defined(_WIN32)
    s_hyperhle_lib = S5L_DYNLIB_OPEN("hyperhle_gles.dll");
    if (!s_hyperhle_lib) {
        s_hyperhle_lib = S5L_DYNLIB_OPEN("libhyperhle_gles.dll");
    }
#else
    char *base_path = SDL_GetBasePath();
    if (base_path) {
        static const char library_name[] = "libhyperhle_gles.so";
        size_t path_size = strlen(base_path) + sizeof library_name;
        char *library_path = (char *)malloc(path_size);
        if (library_path) {
            snprintf(library_path, path_size, "%s%s", base_path, library_name);
            s_hyperhle_lib = S5L_DYNLIB_OPEN(library_path);
            free(library_path);
        }
        SDL_free(base_path);
    }
    if (!s_hyperhle_lib) {
        s_hyperhle_lib = S5L_DYNLIB_OPEN("libhyperhle_gles.so");
    }
#endif
    if (!s_hyperhle_lib) {
        ENGINE_LOG("hyperhle-gles: acceleration library not found, running software MBX3D");
        web_log_printf("info", "hyperhle-gles: acceleration library not found, running software MBX3D");
        return;
    }
    web_log_printf("info", "hyperhle-gles: acceleration library loaded successfully");
    p_s5l_gles_create = (GlesHandle *(*)(int32_t, void *, uint32_t, uint32_t))S5L_DYNLIB_SYM(s_hyperhle_lib, "s5l_gles_create");
    p_s5l_gles_destroy = (void (*)(GlesHandle *))S5L_DYNLIB_SYM(s_hyperhle_lib, "s5l_gles_destroy");
    p_s5l_gles_make_current = (int32_t (*)(GlesHandle *))S5L_DYNLIB_SYM(s_hyperhle_lib, "s5l_gles_make_current");
    p_s5l_gles_swap_buffers = (int32_t (*)(GlesHandle *))S5L_DYNLIB_SYM(s_hyperhle_lib, "s5l_gles_swap_buffers");
    p_s5l_gles_clear_color = (void (*)(GlesHandle *, float, float, float, float))S5L_DYNLIB_SYM(s_hyperhle_lib, "s5l_gles_clear_color");
    p_s5l_gles_clear = (void (*)(GlesHandle *, uint32_t))S5L_DYNLIB_SYM(s_hyperhle_lib, "s5l_gles_clear");
    p_s5l_gles_viewport = (void (*)(GlesHandle *, int32_t, int32_t, int32_t, int32_t))S5L_DYNLIB_SYM(s_hyperhle_lib, "s5l_gles_viewport");
    p_s5l_gles_draw_arrays = (void (*)(GlesHandle *, uint32_t, int32_t, int32_t))S5L_DYNLIB_SYM(s_hyperhle_lib, "s5l_gles_draw_arrays");
    p_s5l_gles_get_string = (const char *(*)(GlesHandle *, uint32_t))S5L_DYNLIB_SYM(s_hyperhle_lib, "s5l_gles_get_string");

    if (p_s5l_gles_create) {
        s_hyperhle_ctx = p_s5l_gles_create(2, NULL, 320, 480);
        if (s_hyperhle_ctx) {
            ios3_hle_set_gles_dispatch(&s_hyperhle_dispatch);
            ENGINE_LOG("hyperhle-gles: acceleration initialized and armed");
            web_log_printf("info", "HyperHLE GLES hardware acceleration initialized and armed");
        } else {
            web_log_printf("warn", "hyperhle-gles: s5l_gles_create returned NULL");
        }
    } else {
        web_log_printf("warn", "hyperhle-gles: symbol s5l_gles_create not found");
    }
}

static void term_hyperhle_gles(void) {
    ios3_hle_set_gles_dispatch(NULL);
    if (s_hyperhle_ctx && p_s5l_gles_destroy) {
        p_s5l_gles_destroy(s_hyperhle_ctx);
        s_hyperhle_ctx = NULL;
    }
    if (s_hyperhle_lib) {
        S5L_DYNLIB_CLOSE(s_hyperhle_lib);
        s_hyperhle_lib = NULL;
    }
}

/* ------------------------------------------- user diagnostic observers --- */

static bool engine_read_user_byte(arm_cpu_t *cpu, uint32_t va, uint8_t *out)
{
    if (!cpu || !cpu->bus || !out) return false;
    uint32_t pa = 0;
    uint32_t fsr = arm_mmu_translate(cpu, va, ARM_ACCESS_READ, false, &pa);
    if (fsr != 0) return false;
    if (cpu->bus->host_ram) {
        uint8_t *ptr = cpu->bus->host_ram(cpu->bus->ctx, pa, 1);
        if (ptr) {
            *out = *ptr;
            return true;
        }
    }
    if (cpu->bus->read8) {
        *out = cpu->bus->read8(cpu->bus->ctx, pa);
        return true;
    }
    return false;
}

static size_t engine_read_user_string(arm_cpu_t *cpu, uint32_t va, char *out, size_t max_len)
{
    if (!cpu || !out || max_len == 0 || va == 0) return 0;
    size_t len = 0;
    while (len + 1 < max_len) {
        uint8_t b = 0;
        if (!engine_read_user_byte(cpu, va + (uint32_t)len, &b)) break;
        if (b == 0) break;
        out[len++] = (char)b;
    }
    out[len] = '\0';
    return len;
}

static size_t engine_read_user_bytes(arm_cpu_t *cpu, uint32_t va, uint8_t *out, size_t count)
{
    if (!cpu || !out || count == 0 || va == 0) return 0;
    size_t i = 0;
    for (; i < count; i++) {
        uint8_t b = 0;
        if (!engine_read_user_byte(cpu, va + (uint32_t)i, &b)) break;
        out[i] = b;
    }
    return i;
}

static const char *engine_signal_name(int sig)
{
    switch (sig) {
        case 1: return "SIGHUP";
        case 2: return "SIGINT";
        case 3: return "SIGQUIT";
        case 4: return "SIGILL";
        case 5: return "SIGTRAP";
        case 6: return "SIGABRT";
        case 7: return "SIGEMT";
        case 8: return "SIGFPE";
        case 9: return "SIGKILL";
        case 10: return "SIGBUS";
        case 11: return "SIGSEGV";
        case 12: return "SIGSYS";
        case 13: return "SIGPIPE";
        case 14: return "SIGALRM";
        case 15: return "SIGTERM";
        default: return "UNKNOWN";
    }
}

static bool is_printable_text(const char *buf, size_t len)
{
    if (!buf || len == 0) return false;
    size_t printable = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c >= 0x20 && c <= 0x7e) printable++;
        else if (c == '\n' || c == '\r' || c == '\t') printable++;
    }
    return (printable * 100u / len) >= 70u;
}

static void engine_uart0_tx_handler(void *ctx, uint8_t byte)
{
    (void)ctx;
    char c = (char)byte;
    web_log_post_raw("kernel", &c, 1);
}

static void engine_user_svc_observer(void *ctx, struct arm_cpu *cpu, uint32_t pc, uint32_t swi_num)
{
    (void)ctx;
    (void)pc;
    (void)swi_num;
    if (!cpu) return;

    int32_t raw_r12 = (int32_t)cpu->r[12];
    uint32_t syscall_num = 0;
    if (raw_r12 < 0) {
        /* Mach trap: negative syscall numbers (e.g. -31 mach_msg_trap) */
        return;
    } else if (raw_r12 == 0) {
        /* Indirect syscall: SYS_syscall */
        syscall_num = cpu->r[0];
    } else {
        /* Direct BSD syscall */
        syscall_num = (uint32_t)raw_r12;
    }

    switch (syscall_num) {
    case 4:   /* SYS_write(fd, buf, count) */
    case 397: /* SYS_write_nocancel */
    case 133: /* SYS_sendto (used by asl_log / syslog) */ {
        uint32_t fd = cpu->r[0];
        uint32_t buf_va = cpu->r[1];
        uint32_t count = cpu->r[2];
        if (count > 0 && count < 65536u && buf_va != 0) {
            size_t to_read = count > 512u ? 512u : (size_t)count;
            char text[513];
            size_t read_bytes = engine_read_user_bytes(cpu, buf_va, (uint8_t *)text, to_read);
            if (read_bytes > 0) {
                text[read_bytes] = '\0';
                if (strstr(text, "dyld:") != NULL || strstr(text, "Library not loaded") != NULL ||
                    strstr(text, "Symbol not found") != NULL) {
                    web_log_post_raw("dyld", text, read_bytes);
                } else if (strstr(text, "CrashReporter") != NULL ||
                           strstr(text, "Incident Identifier") != NULL ||
                           strstr(text, "Exception Type") != NULL) {
                    web_log_post_raw("crash", text, read_bytes);
                } else if (fd == 1 || fd == 2) {
                    web_log_post_raw("app", text, read_bytes);
                } else if (is_printable_text(text, read_bytes)) {
                    web_log_post_raw("app", text, read_bytes);
                }
            }
        }
        break;
    }
    case 121: { /* SYS_writev(fd, iov, iovcnt) */
        uint32_t fd = cpu->r[0];
        uint32_t iov_va = cpu->r[1];
        uint32_t iovcnt = cpu->r[2];
        if (iovcnt > 0 && iov_va != 0) {
            uint8_t iov_raw[8];
            if (engine_read_user_bytes(cpu, iov_va, iov_raw, sizeof iov_raw) == 8) {
                uint32_t base = (uint32_t)iov_raw[0] | ((uint32_t)iov_raw[1] << 8) | ((uint32_t)iov_raw[2] << 16) | ((uint32_t)iov_raw[3] << 24);
                uint32_t len  = (uint32_t)iov_raw[4] | ((uint32_t)iov_raw[5] << 8) | ((uint32_t)iov_raw[6] << 16) | ((uint32_t)iov_raw[7] << 24);
                if (len > 0 && len < 65536u && base != 0) {
                    size_t to_read = len > 512u ? 512u : (size_t)len;
                    char text[513];
                    size_t read_bytes = engine_read_user_bytes(cpu, base, (uint8_t *)text, to_read);
                    if (read_bytes > 0) {
                        text[read_bytes] = '\0';
                        if (strstr(text, "dyld:") || strstr(text, "Library not loaded")) {
                            web_log_post_raw("dyld", text, read_bytes);
                        } else if (strstr(text, "CrashReporter")) {
                            web_log_post_raw("crash", text, read_bytes);
                        } else if (fd == 1 || fd == 2 || is_printable_text(text, read_bytes)) {
                            web_log_post_raw("app", text, read_bytes);
                        }
                    }
                }
            }
        }
        break;
    }
    case 5:   /* SYS_open */
    case 277: /* SYS_open_extended */
    case 398: /* SYS_open_nocancel */ {
        char path[256];
        if (engine_read_user_string(cpu, cpu->r[0], path, sizeof path) > 0) {
            if (strstr(path, ".dylib") || strstr(path, "Frameworks") ||
                strstr(path, ".framework") || strstr(path, ".app") ||
                strstr(path, "CrashReporter") || strstr(path, "dyld") ||
                strstr(path, "Applications/") || strstr(path, "Library/") ||
                strstr(path, ".plist") || strstr(path, ".pvr") ||
                strstr(path, ".caf") || strstr(path, ".png")) {
                web_log_printf("dyld", "open(\"%s\", 0x%x) [ttbr0=0x%08x]", path, cpu->r[1], cpu->cp15.ttbr0);
            }
        }
        break;
    }
    case 59:  /* SYS_execve(path, argv, envp) */
    case 380: /* SYS___mac_execve */ {
        char path[256];
        if (engine_read_user_string(cpu, cpu->r[0], path, sizeof path) > 0) {
            web_log_printf("dyld", "[EXECVE] %s (ttbr0=0x%08x)", path, cpu->cp15.ttbr0);
        }
        break;
    }
    case 244:  /* SYS_posix_spawn */
    case 361: {
        char path[256];
        if (engine_read_user_string(cpu, cpu->r[1], path, sizeof path) > 0) {
            web_log_printf("dyld", "[SPAWN] %s (ttbr0=0x%08x)", path, cpu->cp15.ttbr0);
        }
        break;
    }
    case 37:   /* SYS_kill(pid, sig) */
    case 328:  /* SYS___pthread_kill(thread, sig) */ {
        int target = (int)cpu->r[0];
        int sig = (int)cpu->r[1];
        if (sig == 4 || sig == 6 || sig == 8 || sig == 9 || sig == 10 || sig == 11) {
            web_log_printf("crash", "==================== CRASH SIGNAL ====================");
            web_log_printf("crash", "[SIGNAL] %s(target=%d, sig=%d [%s]) caller PC=0x%08x LR=0x%08x SP=0x%08x",
                           syscall_num == 328 ? "pthread_kill" : "kill",
                           target, sig, engine_signal_name(sig), cpu->r[15], cpu->r[14], cpu->r[13]);
            web_log_printf("crash", "Registers at signal:");
            web_log_printf("crash", "  PC=0x%08x  LR=0x%08x  SP=0x%08x  CPSR=0x%08x",
                           cpu->r[15], cpu->r[14], cpu->r[13], cpu->cpsr);
            web_log_printf("crash", "  r0=0x%08x  r1=0x%08x  r2=0x%08x  r3=0x%08x",
                           cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3]);
            web_log_printf("crash", "  r4=0x%08x  r5=0x%08x  r6=0x%08x  r7=0x%08x",
                           cpu->r[4], cpu->r[5], cpu->r[6], cpu->r[7]);
            web_log_printf("crash", "=======================================================");
        }
        break;
    }
    case 1: { /* SYS_exit(status) */
        int status = (int)cpu->r[0];
        uint32_t stack_sample[8] = {0};
        size_t n = engine_read_user_bytes(cpu, cpu->r[13], (uint8_t *)stack_sample, sizeof stack_sample);
        int words = (int)(n / 4u);
        if (status != 0) {
            web_log_printf("crash", "[EXIT] Process exited with failure status %d (ttbr0=0x%08x PC=0x%08x LR=0x%08x SP=0x%08x)",
                           status, cpu->cp15.ttbr0, cpu->r[15], cpu->r[14], cpu->r[13]);
        } else {
            web_log_printf("info", "[EXIT] Process exited with status 0 (ttbr0=0x%08x PC=0x%08x LR=0x%08x SP=0x%08x)",
                           cpu->cp15.ttbr0, cpu->r[15], cpu->r[14], cpu->r[13]);
        }
        if (words >= 4) {
            web_log_printf("info", "  [EXIT SP..SP+31] %08x %08x %08x %08x %08x %08x %08x %08x",
                           stack_sample[0], stack_sample[1], stack_sample[2], stack_sample[3],
                           stack_sample[4], stack_sample[5], stack_sample[6], stack_sample[7]);
        }
        break;
    }
    default:
        break;
    }
}

/* ---------------------------------------------------------- persistence --- */

static void join_worker(engine_t *e);

static bool persist_instances_locked(engine_t *e)
{
    size_t need = vm_instance_serialize(&e->instances, NULL, 0);
    char *text;
    char path[ENGINE_PATH_CAP], tmp[ENGINE_PATH_CAP];
    bool ok = false;
    int fd;
    if (!join_path(path, sizeof path, e->machines, "instances.txt")) return false;
    if (!join_path(tmp, sizeof tmp, e->machines, "instances.txt.partial")) return false;
    text = malloc(need + 1u);
    if (!text) return false;
    vm_instance_serialize(&e->instances, text, need + 1u);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY | O_CLOEXEC, 0660);
    if (fd >= 0) {
        ok = write(fd, text, need) == (ssize_t)need;
        if (ok && fsync(fd) != 0) ok = false;
        close(fd);
    }
    free(text);
    if (ok && rename(tmp, path) != 0) {
        unlink(path);
        if (rename(tmp, path) != 0) ok = false;
    }
    if (!ok) unlink(tmp);
    return ok;
}

static void load_instances(engine_t *e)
{
    char path[ENGINE_PATH_CAP];
    struct stat st;
    char *text = NULL;
    int fd;
    vm_instance_list_reset(&e->instances);
    if (!join_path(path, sizeof path, e->machines, "instances.txt")) return;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
        st.st_size <= 1024 * 1024) {
        text = malloc((size_t)st.st_size + 1u);
        if (text) {
            fd = open(path, O_RDONLY | O_BINARY | O_CLOEXEC);
            if (fd >= 0) {
                ssize_t got = read(fd, text, (size_t)st.st_size);
                close(fd);
                if (got > 0) {
                    text[got] = '\0';
                    if (vm_instance_deserialize(&e->instances, text) != VM_INSTANCE_OK) {
                        ENGINE_WARN("instances.txt could not be parsed; starting empty");
                        vm_instance_list_reset(&e->instances);
                    }
                }
            }
            free(text);
        }
    }

    /* Auto-discover existing machines on disk that may not be in instances.txt */
    bool changed = false;
#ifdef _WIN32
    char search[MAX_PATH];
    snprintf(search, sizeof(search), "%s\\*", e->machines);
    WIN32_FIND_DATAA fd_find;
    HANDLE h = FindFirstFileA(search, &fd_find);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd_find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!strcmp(fd_find.cFileName, ".") || !strcmp(fd_find.cFileName, "..")) continue;
                if (strlen(fd_find.cFileName) == VM_INSTANCE_ID_LEN &&
                    vm_instance_id_check(fd_find.cFileName) == VM_INSTANCE_OK &&
                    vm_instance_index_of_id(&e->instances, fd_find.cFileName) < 0) {
                    char mname[VM_INSTANCE_NAME_MAX + 1u];
                    snprintf(mname, sizeof mname, "iPhone 3G (%.4s)", fd_find.cFileName);
                    bool opts[VM_INSTANCE_OPTION_MAX];
                    for (unsigned o = 0; o < VM_INSTANCE_OPTION_MAX; o++) {
                        opts[o] = vm_option_new_machine_value(o, false, false);
                    }
                    unsigned idx = 0;
                    if (vm_instance_add(&e->instances, fd_find.cFileName, mname, opts,
                                        VM_INSTANCE_OPTION_MAX, engine_unix_now(), &idx) == VM_INSTANCE_OK) {
                        changed = true;
                    }
                }
            }
        } while (FindNextFileA(h, &fd_find));
        FindClose(h);
    }
#else
    DIR *d = opendir(e->machines);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (strlen(ent->d_name) == VM_INSTANCE_ID_LEN &&
                vm_instance_id_check(ent->d_name) == VM_INSTANCE_OK &&
                vm_instance_index_of_id(&e->instances, ent->d_name) < 0) {
                char mname[VM_INSTANCE_NAME_MAX + 1u];
                snprintf(mname, sizeof mname, "iPhone 3G (%.4s)", ent->d_name);
                bool opts[VM_INSTANCE_OPTION_MAX];
                for (unsigned o = 0; o < VM_INSTANCE_OPTION_MAX; o++) {
                    opts[o] = vm_option_new_machine_value(o, false, false);
                }
                unsigned idx = 0;
                if (vm_instance_add(&e->instances, ent->d_name, mname, opts,
                                    VM_INSTANCE_OPTION_MAX, engine_unix_now(), &idx) == VM_INSTANCE_OK) {
                    changed = true;
                }
            }
        }
        closedir(d);
    }
#endif
    if (changed) {
        persist_instances_locked(e);
    }
}

static int instance_index_locked(engine_t *e, const char *id)
{
    return vm_instance_index_of_id(&e->instances, id);
}

/* --------------------------------------------------------------- import --- */

typedef struct {
    int fd;
} import_pread_ctx_t;

static size_t import_pread(void *context, uint64_t offset,
                           uint8_t *buffer, size_t length)
{
    import_pread_ctx_t *ctx = context;
    ptrdiff_t got;
    do {
        got = portable_pread(ctx->fd, buffer, length, (int64_t)offset);
    } while (got < 0 && errno == EINTR);
    if (got < 0) return VMFW_SOURCE_ERROR;
    return (size_t)got;
}

static void *import_sink_open(void *context, const char *name)
{
    engine_t *e = context;
    import_sink_t *sink;
    if (!name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, ".."))
        return NULL;
    sink = calloc(1, sizeof *sink);
    if (!sink) return NULL;
    if (!join_path(sink->path, sizeof sink->path, e->firmware, name)) {
        free(sink);
        return NULL;
    }
    /* "w+b", not "wb": the importer reads the scratch file back through
     * pread() to unpack the disk image, and pread() on a descriptor opened
     * write-only fails with EBADF. */
    sink->file = fopen(sink->path, "w+b");
    if (!sink->file) {
        free(sink);
        return NULL;
    }
    setvbuf(sink->file, NULL, _IONBF, 0);
    return sink;
}

static bool import_sink_write(void *context, void *handle,
                              const uint8_t *data, size_t length)
{
    import_sink_t *sink = handle;
    (void)context;
    if (!sink || !sink->file) return false;
    return fwrite(data, 1, length, sink->file) == length;
}

static size_t import_sink_pread(void *context, void *handle,
                                uint64_t offset, uint8_t *buffer, size_t length)
{
    import_sink_t *sink = handle;
    ptrdiff_t got;
    (void)context;
    if (!sink || !sink->file) return VMFW_SOURCE_ERROR;
    if (fflush(sink->file) != 0) return VMFW_SOURCE_ERROR;
    do {
        got = portable_pread(fileno(sink->file), buffer, length, (int64_t)offset);
    } while (got < 0 && errno == EINTR);
    if (got < 0) return VMFW_SOURCE_ERROR;
    return (size_t)got;
}

static void import_sink_close(void *context, void *handle, bool keep)
{
    import_sink_t *sink = handle;
    (void)context;
    if (!sink) return;
    if (sink->file) fclose(sink->file);
    if (!keep) unlink(sink->path);
    free(sink);
}

static void import_progress_cb(void *context, vm_fw_artefact_t which,
                               vm_fw_stage_t stage,
                               uint64_t done, uint64_t total)
{
    engine_t *e = context;
    atomic_store(&e->import_artefact, (int)which);
    atomic_store(&e->import_stage, (int)stage);
    atomic_store(&e->import_done, done);
    atomic_store(&e->import_total, total);
}

static bool import_cancel_cb(void *context)
{
    engine_t *e = context;
    return atomic_load(&e->import_cancel);
}

static int engine_import_ipsw_fd(engine_t *engine, int fd, uint64_t size,
                               bool skip_stock_checksums,
                               char *detail, size_t detail_capacity)
{
    engine_t *e = engine;
    import_pread_ctx_t source;
    vm_fw_files_t files;
    vm_fw_import_t request;
    vm_fw_report_t report;
    vm_fw_status_t status;
    bool prepared;
    char rendered[ENGINE_IMPORT_REPORT_CAP];

    if (!e || fd < 0 || size == 0) {
        if (detail && detail_capacity) snprintf(detail, detail_capacity,
                                                "No IPSW was provided.");
        return -1;
    }
    join_worker(e);
    pthread_mutex_lock(&e->lock);
    if (e->phase != PHASE_IDLE) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "Stop the running machine before importing firmware.");
        return -1;
    }
    if (atomic_load(&e->import_active)) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "An import is already running.");
        return -1;
    }
    atomic_store(&e->import_active, true);
    atomic_store(&e->import_cancel, false);
    atomic_store(&e->import_ok, false);
    atomic_store(&e->import_done, 0);
    atomic_store(&e->import_total, 0);
    e->import_report[0] = '\0';
    pthread_mutex_unlock(&e->lock);

    mkdir_p(e->firmware);

    source.fd = fd;
    files.open = import_sink_open;
    files.write = import_sink_write;
    files.pread = import_sink_pread;
    files.close = import_sink_close;
    files.ctx = e;

    memset(&request, 0, sizeof request);
    request.pread = import_pread;
    request.pread_ctx = &source;
    request.size = size;
    request.files = &files;
    request.keys = NULL;
    request.progress = import_progress_cb;
    request.progress_ctx = e;
    request.cancel = import_cancel_cb;
    request.cancel_ctx = e;
    request.resolve_keys = vm_fw_resolve_public_keys;
    request.require_supported = true;
    request.skip_stock_checksums = skip_stock_checksums;

    memset(&report, 0, sizeof report);
    status = vm_fw_import_run(&request, &report);
    prepared = vm_fw_report_is_prepared(&report);

    rendered[0] = '\0';
    vm_fw_report_render(&report, rendered, sizeof rendered);
    if (detail && detail_capacity) {
        const char *text = report.detail[0] ? report.detail
                                            : vm_fw_strerror(status);
        snprintf(detail, detail_capacity, "%s", text);
    }

    pthread_mutex_lock(&e->lock);
    snprintf(e->import_report, sizeof e->import_report, "%s", rendered);
    snprintf(e->import_source, sizeof e->import_source, "%s",
             vm_fw_public_keys_source());
    atomic_store(&e->import_ok, prepared);
    atomic_store(&e->import_active, false);
    pthread_mutex_unlock(&e->lock);

    console_printf(e, "[fw] import %s: %s\n",
                   prepared ? "complete" : "failed",
                   report.detail[0] ? report.detail : vm_fw_strerror(status));
    return prepared ? 0 : -1;
}

int engine_import_ipsw(engine_t *engine, const char *ipsw_path,
                       bool skip_stock_checksums,
                       char *detail, size_t detail_capacity)
{
    if (detail && detail_capacity) detail[0] = '\0';
    if (!engine || !ipsw_path || !ipsw_path[0]) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "No IPSW path specified.");
        return -1;
    }
    struct stat st;
    if (stat(ipsw_path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "IPSW file '%s' not found or empty.", ipsw_path);
        return -1;
    }
#if defined(_O_BINARY)
    int flags = O_RDONLY | _O_BINARY | O_CLOEXEC;
#else
    int flags = O_RDONLY | O_CLOEXEC;
#endif
    int fd = open(ipsw_path, flags);
    if (fd < 0) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "Cannot open IPSW file '%s' (errno %d).", ipsw_path, errno);
        return -1;
    }
    int rc = engine_import_ipsw_fd(engine, fd, (uint64_t)st.st_size, skip_stock_checksums, detail, detail_capacity);
    close(fd);
    return rc;
}

void engine_import_cancel(engine_t *engine)
{
    if (engine) atomic_store(&engine->import_cancel, true);
}

void engine_import_progress(engine_t *engine, int *active, int *artefact,
                            int *stage, uint64_t *done, uint64_t *total)
{
    engine_t *e = engine;
    if (!e) return;
    if (active) *active = atomic_load(&e->import_active) ? 1 : 0;
    if (artefact) *artefact = atomic_load(&e->import_artefact);
    if (stage) *stage = atomic_load(&e->import_stage);
    if (done) *done = atomic_load(&e->import_done);
    if (total) *total = atomic_load(&e->import_total);
}

int engine_firmware_json(engine_t *engine, char *out, size_t capacity)
{
    engine_t *e = engine;
    json_t j;
    char path[ENGINE_PATH_CAP];
    uint64_t kernel = 0, tree = 0, rootfs = 0;
    bool have_kernel, have_tree, have_rootfs;
    char report[ENGINE_IMPORT_REPORT_CAP];
    char source[VM_FW_BOOT_DETAIL_CAPACITY];
    bool import_ok;

    if (!e || !out || capacity == 0) return -1;
    json_reset(&j, out, capacity);

    have_kernel = join_path(path, sizeof path, e->firmware,
                            VM_FW_BOOT_KERNEL_FILE) &&
                  file_present(path, &kernel);
    have_tree = join_path(path, sizeof path, e->firmware,
                          VM_FW_BOOT_DEVICETREE_FILE) &&
                file_present(path, &tree);
    have_rootfs = join_path(path, sizeof path, e->firmware,
                            VM_FW_BOOT_ROOTFS_FILE) &&
                  file_present(path, &rootfs);

    pthread_mutex_lock(&e->lock);
    snprintf(report, sizeof report, "%s", e->import_report);
    snprintf(source, sizeof source, "%s", e->import_source);
    import_ok = atomic_load(&e->import_ok);
    pthread_mutex_unlock(&e->lock);

    json_printf(&j,
        "{\"ok\":%s,\"kernel\":{\"present\":%s,\"size\":%llu},"
        "\"devicetree\":{\"present\":%s,\"size\":%llu},"
        "\"rootfs\":{\"present\":%s,\"size\":%llu},"
        "\"importOk\":%s,\"importActive\":%s,\"source\":",
        have_kernel && have_tree && have_rootfs ? "true" : "false",
        have_kernel ? "true" : "false", (unsigned long long)kernel,
        have_tree ? "true" : "false", (unsigned long long)tree,
        have_rootfs ? "true" : "false", (unsigned long long)rootfs,
        import_ok ? "true" : "false",
        atomic_load(&e->import_active) ? "true" : "false");
    json_string(&j, source);
    json_raw(&j, ",\"lastReport\":");
    json_string(&j, report);
    json_raw(&j, "}");
    return (int)j.len;
}

/* -------------------------------------------------------------- packages --- */

static void package_json(json_t *j, const vm_guest_package_t *package)
{
    json_raw(j, "{\"package\":");
    json_string(j, package->package);
    json_raw(j, ",\"version\":");
    json_string(j, package->version);
    json_raw(j, ",\"filename\":");
    json_string(j, package->filename);
    json_raw(j, ",\"url\":");
    json_string(j, package->source_url);
    json_printf(j, ",\"size\":%llu,\"sha256\":",
                (unsigned long long)package->size);
    json_string(j, package->sha256_hex);
    json_printf(j, ",\"roles\":%u}", (unsigned)package->roles);
}

static void hex_encode(const uint8_t *bytes, size_t count, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < count; i++) {
        out[i * 2u] = digits[bytes[i] >> 4];
        out[i * 2u + 1u] = digits[bytes[i] & 0x0fu];
    }
    out[count * 2u] = '\0';
}

int engine_packages_json(engine_t *engine, const char *id,
                         char *out, size_t capacity)
{
    engine_t *e = engine;
    json_t j;
    uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE];
    char identity[VM_GUEST_PACKAGE_SHA256_HEX_SIZE];
    const vm_guest_package_t *appsync;
    bool manifest_ok;
    bool machine_ok = false;
    bool installed = false;
    uint64_t work_size = 0;
    uint64_t snapshots = 0;
    char machine[ENGINE_PATH_CAP];
    size_t count, i;

    if (!e || !out || capacity == 0) return -1;
    manifest_ok = vm_guest_package_manifest_sha256(digest);
    if (manifest_ok)
        hex_encode(digest, sizeof digest, identity);
    else
        identity[0] = '\0';
    appsync = vm_guest_package_appsync();
    machine[0] = '\0';

    if (id && id[0]) {
        vm_instance_paths_t paths;
        pthread_mutex_lock(&e->lock);
        if (instance_index_locked(e, id) >= 0 &&
            vm_instance_paths_derive(e->firmware, e->machines, id, &paths) ==
                VM_INSTANCE_PATHS_OK) {
            snprintf(machine, sizeof machine, "%s", paths.machine);
            machine_ok = true;
        }
        pthread_mutex_unlock(&e->lock);
    }
    if (machine_ok) {
        char path[ENGINE_PATH_CAP];
        char snaps[ENGINE_PATH_CAP];
        char snap_detail[128];
        vm_snapshot_info_t list[VM_SNAPSHOT_MAX];
        size_t snap_count = 0;
        bool work, marker;
        uint64_t size = 0;

        work = join_path(path, sizeof path, machine,
                         VM_GUEST_INSTALL_LIVE_FILE) &&
               file_present(path, &size);
        if (work) work_size = size;
        marker = join_path(path, sizeof path, machine,
                           VM_GUEST_INSTALL_MARKER_FILE) &&
                 file_present(path, NULL);
        installed = work && marker;
        if (vm_snapshot_dir(machine, snaps, sizeof snaps) == VM_SNAPSHOT_OK &&
            vm_snapshot_list(snaps, list, VM_SNAPSHOT_MAX, &snap_count,
                             snap_detail, sizeof snap_detail) ==
                VM_SNAPSHOT_OK)
            snapshots = snap_count;
    }

    json_reset(&j, out, capacity);
    json_printf(&j, "{\"ok\":%s,\"busy\":%s,\"identity\":",
                manifest_ok ? "true" : "false",
                atomic_load(&e->install_active) ? "true" : "false");
    json_string(&j, identity);
    json_raw(&j, ",\"machine\":");
    json_string(&j, machine);
    json_printf(&j,
        ",\"machineOk\":%s,\"installed\":%s,\"workImage\":%llu,"
        "\"snapshots\":%llu,\"totalBytes\":%llu,\"appsyncBytes\":%llu,"
        "\"packages\":[",
        machine_ok ? "true" : "false",
        installed ? "true" : "false",
        (unsigned long long)work_size,
        (unsigned long long)snapshots,
        (unsigned long long)vm_guest_package_total_download_bytes(),
        (unsigned long long)(appsync ? appsync->size : 0u));
    count = vm_guest_package_count();
    for (i = 0; i < count; i++) {
        const vm_guest_package_t *package = vm_guest_package_at(i);
        if (!package) continue;
        if (i) json_raw(&j, ",");
        package_json(&j, package);
    }
    json_raw(&j, "],\"appsync\":");
    if (appsync)
        package_json(&j, appsync);
    else
        json_raw(&j, "null");
    json_raw(&j, "}");
    return (int)j.len;
}

static void install_progress_cb(void *context,
                                vm_guest_install_build_phase_t phase,
                                uint64_t done, uint64_t total)
{
    engine_t *e = context;
    atomic_store(&e->install_phase, (int)phase);
    atomic_store(&e->install_done, done);
    atomic_store(&e->install_total, total);
}

/*
 * One blocking call.  `package_directory` may be empty: the builder then
 * verifies or repairs an existing jailbreak without needing downloads, and
 * answers ERR_ARGUMENT when a fresh install still requires packages.
 */
int engine_install_packages(engine_t *engine, const char *id,
                            const char *package_directory,
                            bool include_appsync,
                            char *out, size_t capacity,
                            char *detail, size_t detail_capacity)
{
    engine_t *e = engine;
    vm_instance_paths_t paths;
    vm_guest_install_build_result_t result;
    vm_guest_install_build_status_t status;
    char machine[ENGINE_PATH_CAP];
    char message[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    json_t j;
    int rc = -1;

    if (detail && detail_capacity) detail[0] = '\0';
    if (!e || !id || !id[0]) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "No machine was selected.");
        return -1;
    }

    pthread_mutex_lock(&e->lock);
    if (e->thread_live || e->phase != PHASE_IDLE) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "Stop the machine before installing packages.");
        return -1;
    }
    if (atomic_load(&e->install_active)) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "A package installation is already running.");
        return -1;
    }
    if (instance_index_locked(e, id) < 0) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "No such machine.");
        return -1;
    }
    if (vm_instance_paths_derive(e->firmware, e->machines, id, &paths) !=
        VM_INSTANCE_PATHS_OK) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "That machine's directory could not be resolved.");
        return -1;
    }
    snprintf(machine, sizeof machine, "%s", paths.machine);
    atomic_store(&e->install_active, true);
    atomic_store(&e->install_phase, VM_GUEST_INSTALL_BUILD_RECOVERING);
    atomic_store(&e->install_done, 0);
    atomic_store(&e->install_total, 0);
    pthread_mutex_unlock(&e->lock);

    message[0] = '\0';
    memset(&result, 0, sizeof result);
    status = vm_guest_install_build_after_force_off_with_appsync(
        machine,
        package_directory && package_directory[0] ? package_directory : NULL,
        include_appsync, install_progress_cb, e, &result,
        message, sizeof message);

    pthread_mutex_lock(&e->lock);
    atomic_store(&e->install_active, false);
    pthread_mutex_unlock(&e->lock);

    if (detail && detail_capacity) {
        snprintf(detail, detail_capacity, "%s",
                 message[0] ? message
                            : vm_guest_install_build_status_text(status));
    }

    if (out && capacity) {
        json_reset(&j, out, capacity);
        json_printf(&j, "{\"ok\":%s,\"status\":%d,\"statusText\":",
                    status == VM_GUEST_INSTALL_BUILD_OK ? "true" : "false",
                    (int)status);
        json_string(&j, vm_guest_install_build_status_text(status));
        json_raw(&j, ",\"detail\":");
        json_string(&j, detail && detail_capacity ? detail : message);
        json_printf(&j,
            ",\"alreadyInstalled\":%s,\"forcedPowerOff\":%s,"
            "\"storageUpgraded\":%s,\"cydiaPrivilegesRepaired\":%s,"
            "\"cydiaSourcesAdded\":%s,\"aptTrustInstalled\":%s,"
            "\"aptVerifierStaged\":%s,\"cydiaCacheStaged\":%s,"
            "\"filesystemRepaired\":%s,\"snapshots\":%llu}",
            result.already_installed ? "true" : "false",
            result.forced_power_off ? "true" : "false",
            result.storage_upgraded ? "true" : "false",
            result.cydia_privileges_repaired ? "true" : "false",
            result.cydia_sources_added ? "true" : "false",
            result.apt_trust_installed ? "true" : "false",
            result.apt_verifier_staged ? "true" : "false",
            result.cydia_cache_staged ? "true" : "false",
            result.filesystem_repaired ? "true" : "false",
            (unsigned long long)result.historical_snapshots);
    }
    if (status == VM_GUEST_INSTALL_BUILD_OK) rc = 0;
    console_printf(e, "[pkg] install %s: %s\n",
                   rc == 0 ? "complete" : "failed",
                   detail && detail_capacity ? detail : message);
    return rc;
}

void engine_install_progress(engine_t *engine, int *active, int *phase,
                             uint64_t *done, uint64_t *total)
{
    engine_t *e = engine;
    if (!e) return;
    if (active) *active = atomic_load(&e->install_active) ? 1 : 0;
    if (phase) *phase = atomic_load(&e->install_phase);
    if (done) *done = atomic_load(&e->install_done);
    if (total) *total = atomic_load(&e->install_total);
}

/* ------------------------------------------------------------- machines --- */

static void machine_json(engine_t *e, const vm_instance_t *instance, json_t *j)
{
    vm_instance_paths_t paths;
    vm_firmware_boot_paths_t boot_paths;
    vm_firmware_boot_state_t state;
    bool derived;
    vm_instance_paths_status_t ps;

    json_raw(j, "{\"id\":");
    json_string(j, instance->id);
    json_raw(j, ",\"name\":");
    json_string(j, instance->name);
    json_printf(j,
        ",\"created\":%llu,\"lastOpened\":%llu,\"retiredTotal\":%llu",
        (unsigned long long)instance->created_unix,
        (unsigned long long)instance->last_opened_unix,
        (unsigned long long)instance->retired_total);

    ps = vm_instance_paths_derive(e->firmware, e->machines, instance->id,
                                  &paths);
    derived = ps == VM_INSTANCE_PATHS_OK;
    memset(&state, 0, sizeof state);
    if (derived) {
        vm_instance_paths_to_boot(&paths, &boot_paths);
        vm_firmware_boot_probe(&boot_paths, &state);
    }
    uint64_t disk_capacity = 0;
    unsigned disk_gib = 2;
    if (derived && vm_disk_size_read(paths.machine, &disk_capacity) && disk_capacity > 0) {
        disk_gib = (unsigned)(disk_capacity / VM_DISK_GIB);
        if (disk_gib != 2 && disk_gib != 4 && disk_gib != 8) disk_gib = 2;
    }
    json_printf(j,
        ",\"ready\":%s,\"readyState\":%d,\"kernel\":%s,\"devicetree\":%s,"
        "\"rootfs\":%s,\"work\":%s,\"diskGiB\":%u,\"detail\":",
        derived && state.readiness == VM_FW_BOOT_READY ? "true" : "false",
        derived ? (int)state.readiness : 3,
        derived && state.kernel_present ? "true" : "false",
        derived && state.devicetree_present ? "true" : "false",
        derived && state.rootfs_present ? "true" : "false",
        derived && state.work_present ? "true" : "false",
        disk_gib);
    json_string(j, derived ? state.detail : vm_instance_paths_status_text(ps));
    json_raw(j, "}");
}

int engine_machines_json(engine_t *engine, char *out, size_t capacity)
{
    engine_t *e = engine;
    vm_instance_list_t snapshot;
    json_t j;
    unsigned i;

    if (!e || !out || capacity == 0) return -1;
    pthread_mutex_lock(&e->lock);
    snapshot = e->instances;
    pthread_mutex_unlock(&e->lock);

    json_reset(&j, out, capacity);
    json_printf(&j, "{\"ok\":true,\"runningId\":");
    pthread_mutex_lock(&e->lock);
    json_string(&j, e->phase != PHASE_IDLE ? e->instance_id : "");
    pthread_mutex_unlock(&e->lock);
    json_raw(&j, ",\"machines\":[");
    for (i = 0; i < snapshot.count; i++) {
        if (i) json_raw(&j, ",");
        machine_json(e, &snapshot.slot[i], &j);
    }
    json_raw(&j, "]}");
    return (int)j.len;
}

int engine_machine_create(engine_t *engine, const char *name,
                          const bool *options, unsigned option_count,
                          unsigned disk_size_gib,
                          char *new_id, size_t id_capacity,
                          char *detail, size_t detail_capacity)
{
    engine_t *e = engine;
    vm_instance_status_t st;
    char id[VM_INSTANCE_ID_LEN + 1u];
    char trimmed[VM_INSTANCE_NAME_MAX + 1u];
    bool values[VM_INSTANCE_OPTION_MAX];
    unsigned count, i, tries;
    int index = -1;

    if (detail && detail_capacity) detail[0] = '\0';
    if (!e || !name) {
        if (detail && detail_capacity) snprintf(detail, detail_capacity,
                                                "A machine needs a name.");
        return -1;
    }
    snprintf(trimmed, sizeof trimmed, "%s", name);
    while (trimmed[0] == ' ' || trimmed[0] == '\t') memmove(trimmed, trimmed + 1,
                                                            strlen(trimmed));
    {
        size_t len = strlen(trimmed);
        while (len && (trimmed[len - 1] == ' ' || trimmed[len - 1] == '\t'))
            trimmed[--len] = '\0';
    }
    st = vm_instance_name_check(trimmed);
    if (st != VM_INSTANCE_OK) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "%s", vm_instance_status_text(st));
        return -1;
    }

    count = vm_option_count();
    if (count > VM_INSTANCE_OPTION_MAX) count = VM_INSTANCE_OPTION_MAX;
    for (i = 0; i < count; i++) {
        values[i] = options && i < option_count
                        ? options[i]
                        : engine_new_machine_option_value(i);
    }

    pthread_mutex_lock(&e->lock);
    if (e->instances.count >= VM_INSTANCE_MAX) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "%s",
                     vm_instance_status_text(VM_INSTANCE_ERR_FULL));
        return -1;
    }
    for (tries = 0; tries < 100; tries++) {
        make_instance_id(e, id);
        if (instance_index_locked(e, id) < 0) break;
    }
    st = vm_instance_add(&e->instances, id, trimmed, values, count,
                         engine_unix_now(), NULL);
    if (st == VM_INSTANCE_OK) {
        index = instance_index_locked(e, id);
        if (index >= 0) e->instances.slot[index].last_opened_unix = 0;
        if (!persist_instances_locked(e)) st = VM_INSTANCE_ERR_PARSE;
    }
    pthread_mutex_unlock(&e->lock);

    if (st != VM_INSTANCE_OK) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "%s", vm_instance_status_text(st));
        return -1;
    }

    {
        vm_instance_paths_t paths;
        if (vm_instance_paths_derive(e->firmware, e->machines, id, &paths) ==
            VM_INSTANCE_PATHS_OK) {
            mkdir_p(paths.machine);
            unsigned gib = (disk_size_gib == 4 || disk_size_gib == 8) ? disk_size_gib : 2u;
            const char *rec = vm_disk_size_record(gib);
            if (rec) {
                char size_path[ENGINE_PATH_CAP];
                if (join_path(size_path, sizeof size_path, paths.machine, VM_DISK_SIZE_FILE)) {
                    FILE *sf = fopen(size_path, "wb");
                    if (sf) {
                        fputs(rec, sf);
                        fclose(sf);
                    }
                }
            }
        }
    }
    if (new_id && id_capacity) snprintf(new_id, id_capacity, "%s", id);
    return 0;
}

int engine_get_or_create_default_machine(engine_t *engine, char *out_id, size_t capacity)
{
    engine_t *e = engine;
    if (!e || !out_id || capacity == 0) return -1;
    out_id[0] = '\0';

    pthread_mutex_lock(&e->lock);
    if (e->instances.count > 0) {
        /* Prefer an instance that has an existing provisioned rootfs-work.img */
        int best_idx = -1;
        for (unsigned i = 0; i < e->instances.count; i++) {
            char img_path[ENGINE_PATH_CAP];
            struct stat st;
            if (join_path(img_path, sizeof img_path, e->machines, e->instances.slot[i].id) &&
                join_path(img_path, sizeof img_path, img_path, "rootfs-work.img") &&
                stat(img_path, &st) == 0 && st.st_size > 1000000) {
                best_idx = (int)i;
                break;
            }
        }
        if (best_idx < 0) best_idx = 0;
        snprintf(out_id, capacity, "%s", e->instances.slot[best_idx].id);
        pthread_mutex_unlock(&e->lock);
        return 0;
    }
    pthread_mutex_unlock(&e->lock);

    bool default_opts[VM_INSTANCE_OPTION_MAX] = {false};
    char detail[256] = {0};
    int rc = engine_machine_create(e, "iPhone 3G", default_opts, 0, 8,
                                   out_id, capacity, detail, sizeof detail);
    if (rc != 0) {
        ENGINE_WARN("Failed to create default machine: %s", detail);
        return -1;
    }
    return 0;
}

int engine_machine_set_disk_size(engine_t *engine, const char *id,
                                 unsigned disk_size_gib,
                                 char *detail, size_t detail_capacity)
{
    engine_t *e = engine;
    vm_instance_paths_t paths;
    unsigned gib;
    const char *rec;
    char size_path[ENGINE_PATH_CAP];
    FILE *f;

    if (detail && detail_capacity) detail[0] = '\0';
    if (!e || !id || !id[0]) return -1;
    gib = (disk_size_gib == 4 || disk_size_gib == 8) ? disk_size_gib : 2u;
    rec = vm_disk_size_record(gib);
    if (!rec) return -1;

    pthread_mutex_lock(&e->lock);
    if (e->phase != PHASE_IDLE && !strcmp(id, e->instance_id)) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "Stop the machine before changing disk size.");
        return -1;
    }
    pthread_mutex_unlock(&e->lock);

    if (vm_instance_paths_derive(e->firmware, e->machines, id, &paths) != VM_INSTANCE_PATHS_OK)
        return -1;
    mkdir_p(paths.machine);
    if (!join_path(size_path, sizeof size_path, paths.machine, VM_DISK_SIZE_FILE))
        return -1;
    f = fopen(size_path, "wb");
    if (!f) return -1;
    fputs(rec, f);
    fclose(f);
    return 0;
}

int engine_machine_rename(engine_t *engine, const char *id, const char *name,
                          char *detail, size_t detail_capacity)
{
    engine_t *e = engine;
    int index;
    vm_instance_status_t st;
    if (detail && detail_capacity) detail[0] = '\0';
    if (!e || !id || !name) return -1;
    pthread_mutex_lock(&e->lock);
    if (e->phase != PHASE_IDLE && !strcmp(id, e->instance_id)) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "Stop the machine before renaming it.");
        return -1;
    }
    index = instance_index_locked(e, id);
    if (index < 0) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "No such machine.");
        return -1;
    }
    st = vm_instance_rename(&e->instances, (unsigned)index, name);
    if (st == VM_INSTANCE_OK && !persist_instances_locked(e))
        st = VM_INSTANCE_ERR_PARSE;
    pthread_mutex_unlock(&e->lock);
    if (st != VM_INSTANCE_OK) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "%s", vm_instance_status_text(st));
        return -1;
    }
    return 0;
}

int engine_machine_delete(engine_t *engine, const char *id,
                          char *detail, size_t detail_capacity)
{
    engine_t *e = engine;
    int index;
    vm_instance_status_t st;
    vm_instance_paths_t paths;
    bool have_paths = false;

    if (detail && detail_capacity) detail[0] = '\0';
    if (!e || !id) return -1;

    pthread_mutex_lock(&e->lock);
    if (e->phase != PHASE_IDLE) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "Stop the running machine before deleting a machine.");
        return -1;
    }
    index = instance_index_locked(e, id);
    if (index < 0) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "No such machine.");
        return -1;
    }
    if (vm_instance_paths_derive(e->firmware, e->machines, id, &paths) ==
        VM_INSTANCE_PATHS_OK)
        have_paths = true;
    st = vm_instance_remove(&e->instances, (unsigned)index);
    if (st == VM_INSTANCE_OK && !persist_instances_locked(e))
        st = VM_INSTANCE_ERR_PARSE;
    pthread_mutex_unlock(&e->lock);

    if (st != VM_INSTANCE_OK) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "%s", vm_instance_status_text(st));
        return -1;
    }
    if (have_paths) {
        if (rm_rf(paths.machine) != 0 && detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "The machine was removed from the list, but its files "
                     "could not be deleted.");
    }
    return 0;
}

int engine_ipa_inspect_json(const char *ipa_path, char *out, size_t capacity)
{
    json_t j;
    engine_ipa_info_t info;
    char detail[320] = {0};
    if (!out || capacity == 0) return -1;
    json_reset(&j, out, capacity);
    if (!ipa_path || !ipa_path[0]) {
        json_raw(&j, "{\"ok\":false,\"detail\":\"No IPA path specified.\"}");
        return 0;
    }
    if (!engine_ipa_inspect_path(ipa_path, &info, detail, sizeof detail)) {
        json_raw(&j, "{\"ok\":false,\"detail\":");
        json_string(&j, detail[0] ? detail : "Failed to inspect IPA.");
        json_raw(&j, "}");
        return 0;
    }
    json_raw(&j, "{\"ok\":true,\"name\":");
    json_string(&j, info.display_name);
    json_raw(&j, ",\"bundleId\":");
    json_string(&j, info.bundle_id);
    json_raw(&j, ",\"version\":");
    json_string(&j, info.version);
    json_raw(&j, ",\"executable\":");
    json_string(&j, info.executable);
    json_raw(&j, ",\"minOS\":");
    json_string(&j, info.min_os);
    json_printf(&j, ",\"fileCount\":%u,\"uncompressedBytes\":%llu}",
                info.file_count, (unsigned long long)info.uncompressed_bytes);
    return 0;
}

int engine_machine_install_ipa(engine_t *engine, const char *id,
                               const char *cache_directory,
                               const char *ipa_path,
                               char *detail, size_t detail_capacity)
{
    engine_t *e = engine;
    vm_instance_paths_t paths;

    if (detail && detail_capacity) detail[0] = '\0';
    if (!e || !id || !id[0] || !ipa_path || !ipa_path[0]) {
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Invalid arguments.");
        return -1;
    }

    pthread_mutex_lock(&e->lock);
    if (e->phase != PHASE_IDLE && !strcmp(id, e->instance_id)) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "Stop the machine before installing an IPA.");
        return -1;
    }
    pthread_mutex_unlock(&e->lock);

    if (vm_instance_paths_derive(e->firmware, e->machines, id, &paths) != VM_INSTANCE_PATHS_OK) {
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Cannot derive machine paths.");
        return -1;
    }

    const char *cache_dir = (cache_directory && cache_directory[0]) ? cache_directory : e->root;
    return engine_ipa_install_path(paths.machine, cache_dir, ipa_path, detail, detail_capacity);
}

int engine_options_json(engine_t *engine, char *out, size_t capacity)
{
    json_t j;
    unsigned i, groups;
    (void)engine;
    if (!out || capacity == 0) return -1;
    json_reset(&j, out, capacity);
    json_raw(&j, "{\"groups\":[");
    groups = VM_OPT_GROUP_COUNT;
    for (i = 0; i < groups; i++) {
        const char *title = vm_option_group_title(i);
        const char *note = vm_option_group_note(i);
        if (i) json_raw(&j, ",");
        json_raw(&j, "{\"title\":");
        json_string(&j, title ? title : "");
        json_raw(&j, ",\"note\":");
        json_string(&j, note ? note : "");
        json_raw(&j, "}");
    }
    json_raw(&j, "],\"options\":[");
    for (i = 0; i < vm_option_count(); i++) {
        const vm_option_t *option = vm_option_at(i);
        if (!option) continue;
        if (i) json_raw(&j, ",");
        json_raw(&j, "{\"name\":");
        json_string(&j, option->name);
        json_raw(&j, ",\"title\":");
        json_string(&j, option->title);
        json_raw(&j, ",\"detail\":");
        json_string(&j, option->detail);
        json_printf(&j, ",\"default\":%s,\"group\":%u,\"implemented\":%s}",
                    option->def ? "true" : "false", (unsigned)option->group,
                    option->impl == VM_OPT_IMPL_HARNESS ? "true" : "false");
    }
    json_raw(&j, "]}");
    return (int)j.len;
}

int engine_machine_options_json(engine_t *engine, const char *id,
                                char *out, size_t capacity)
{
    engine_t *e = engine;
    vm_instance_t instance;
    bool found = false;
    unsigned count, i;
    json_t j;

    if (!e || !out || capacity == 0) return -1;
    pthread_mutex_lock(&e->lock);
    {
        int index = id ? instance_index_locked(e, id) : -1;
        if (index >= 0) {
            instance = e->instances.slot[index];
            found = true;
        }
    }
    pthread_mutex_unlock(&e->lock);

    count = vm_option_count();
    if (count > VM_INSTANCE_OPTION_MAX) count = VM_INSTANCE_OPTION_MAX;
    json_reset(&j, out, capacity);
    json_printf(&j, "{\"ok\":true,\"found\":%s,\"values\":[",
                found ? "true" : "false");
    for (i = 0; i < count; i++) {
        bool value = found ? (i < VM_INSTANCE_OPTION_MAX && instance.options[i])
                           : engine_new_machine_option_value(i);
        if (i) json_raw(&j, ",");
        json_raw(&j, value ? "true" : "false");
    }
    json_raw(&j, "]}");
    return (int)j.len;
}

int engine_machine_set_options(engine_t *engine, const char *id,
                               const bool *options, unsigned option_count,
                               char *detail, size_t detail_capacity)
{
    engine_t *e = engine;
    int index;
    unsigned count, i;

    if (detail && detail_capacity) detail[0] = '\0';
    if (!e || !id || !options) return -1;
    pthread_mutex_lock(&e->lock);
    if (e->phase != PHASE_IDLE && !strcmp(id, e->instance_id)) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "Stop the machine before changing its settings.");
        return -1;
    }
    index = instance_index_locked(e, id);
    if (index < 0) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "No such machine.");
        return -1;
    }
    count = vm_option_count();
    if (count > VM_INSTANCE_OPTION_MAX) count = VM_INSTANCE_OPTION_MAX;
    for (i = 0; i < VM_INSTANCE_OPTION_MAX; i++)
        e->instances.slot[index].options[i] = false;
    for (i = 0; i < count && i < option_count; i++)
        e->instances.slot[index].options[i] = options[i];
    if (!persist_instances_locked(e)) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "The settings could not be saved.");
        return -1;
    }
    pthread_mutex_unlock(&e->lock);
    return 0;
}

/* --------------------------------------------------------------- payload --- */

static void provision_progress_cb(void *context, uint64_t done, uint64_t total)
{
    engine_t *e = context;
    atomic_store(&e->prepare_done, done);
    atomic_store(&e->prepare_total, total);
}

static bool abort_cb(void *context)
{
    engine_t *e = context;
    return atomic_load(&e->abort_flag);
}

static bool arm_newest_overlay(engine_t *e, const vm_instance_paths_t *paths,
                               vm_firmware_boot_t *boot)
{
    /* Must run between vm_firmware_boot_create() and _start(): _start() is
     * where the overlay descriptor is opened and it refuses to boot if an
     * armed overlay cannot be opened. */
    char snapshots[ENGINE_PATH_CAP];
    vm_snapshot_info_t infos[VM_SNAPSHOT_MAX];
    size_t count = 0;
    char detail[VM_FW_BOOT_DETAIL_CAPACITY];
    char overlay[ENGINE_PATH_CAP];

    if (vm_snapshot_dir(paths->machine, snapshots, sizeof snapshots) !=
        VM_SNAPSHOT_OK)
        return true;
    if (vm_snapshot_list(snapshots, infos, VM_SNAPSHOT_MAX, &count,
                         detail, sizeof detail) != VM_SNAPSHOT_OK)
        return true;
    if (count == 0) return true;
    if (vm_snapshot_member_path(snapshots, infos[0].id, false,
                                VM_SNAPSHOT_COW_FILE, overlay,
                                sizeof overlay) != VM_SNAPSHOT_OK)
        return true;
    if (vm_firmware_boot_arm_overlay(boot, overlay)) {
        console_printf(e, "[vm] snapshot protection will record into %s\n",
                       infos[0].id);
        return true;
    }
    console_printf(e,
        "[vm] WARNING: snapshot protection could not be armed; writes from "
        "this run are not recorded\n");
    return false;
}

static bool install_payload(engine_t *e, s5l8900_t *machine,
                            vm_firmware_boot_t **out_boot)
{
    vm_instance_paths_t paths;
    vm_firmware_boot_paths_t boot_paths;
    vm_firmware_boot_state_t state;
    vm_instance_paths_status_t ps;
    vm_instance_work_plan_t plan;
    bool values[VM_INSTANCE_OPTION_MAX];
    unsigned count;
    const char *note = "";

    *out_boot = NULL;
    if (!e->instance_id[0]) {
        if (!vm_guest_install(machine)) return false;
        pthread_mutex_lock(&e->lock);
        snprintf(e->mode, sizeof e->mode, "built-in test guest");
        snprintf(e->note, sizeof e->note,
                 "This machine has no identity, so it has no writable root "
                 "filesystem. Open a machine from the list to boot iPhone OS.");
        pthread_mutex_unlock(&e->lock);
        return true;
    }

    ps = vm_instance_paths_derive(e->firmware, e->machines, e->instance_id,
                                  &paths);
    if (ps != VM_INSTANCE_PATHS_OK) {
        if (!vm_guest_install(machine)) return false;
        pthread_mutex_lock(&e->lock);
        snprintf(e->mode, sizeof e->mode, "built-in test guest");
        snprintf(e->note, sizeof e->note,
                 "This machine's files cannot be located: %s.",
                 vm_instance_paths_status_text(ps));
        pthread_mutex_unlock(&e->lock);
        return true;
    }
    if (!mkdir_p(paths.machine)) {
        if (!vm_guest_install(machine)) return false;
        pthread_mutex_lock(&e->lock);
        snprintf(e->mode, sizeof e->mode, "built-in test guest");
        snprintf(e->note, sizeof e->note,
                 "This machine's directory could not be created.");
        pthread_mutex_unlock(&e->lock);
        return true;
    }

    pthread_mutex_lock(&e->lock);
    snprintf(e->machine_log_path, sizeof e->machine_log_path, "%s/console.log", paths.machine);
    pthread_mutex_unlock(&e->lock);

    plan = vm_instance_work_plan(&paths);
    if (plan == VM_INSTANCE_WORK_ADOPT) {
        char adopted[VM_FW_BOOT_DETAIL_CAPACITY];
        bool ok = vm_instance_work_adopt(&paths, adopted, sizeof adopted);
        console_printf(e, "[vm] %s\n", adopted);
        if (!ok) note = adopted;
    }

    count = e->option_count;
    if (count > VM_INSTANCE_OPTION_MAX) count = VM_INSTANCE_OPTION_MAX;
    memcpy(values, e->options, sizeof values);

    vm_instance_paths_to_boot(&paths, &boot_paths);
    vm_firmware_boot_probe(&boot_paths, &state);

    if (state.readiness == VM_FW_BOOT_NEEDS_WORK_IMAGE) {
        char why[VM_FW_BOOT_DETAIL_CAPACITY] = {0};
        bool ok;
        pthread_mutex_lock(&e->lock);
        e->preparing_root = true;
        snprintf(e->status, sizeof e->status, "preparing the root filesystem");
        pthread_mutex_unlock(&e->lock);
        atomic_store(&e->prepare_done, 0);
        atomic_store(&e->prepare_total, 0);
        ok = vm_firmware_boot_provision_cancellable(
            &boot_paths, values, count, provision_progress_cb, e,
            why, sizeof why, abort_cb, e);
        if (!ok) {
            vm_firmware_boot_probe(&boot_paths, &state);
            if (state.work_present) {
                ok = true;
                snprintf(why, sizeof why,
                         "The root filesystem was already prepared.");
            }
        }
        pthread_mutex_lock(&e->lock);
        e->preparing_root = false;
        if (!ok) {
            snprintf(e->note, sizeof e->note, "%s",
                     why[0] ? why
                            : "The root filesystem could not be prepared.");
        }
        pthread_mutex_unlock(&e->lock);
        console_printf(e, "[vm] root filesystem: %s\n",
                       ok ? "ready" : "not prepared");
        if (!ok) {
            if (atomic_load(&e->abort_flag)) return false;
            vm_firmware_boot_probe(&boot_paths, &state);
        } else {
            vm_firmware_boot_probe(&boot_paths, &state);
        }
    }

    if (state.readiness == VM_FW_BOOT_READY) {
        vm_firmware_boot_t *boot = vm_firmware_boot_create();
        if (boot) {
            vm_firmware_boot_report_t report;
            /* Recording has to be armed before _start(), because _start() is
             * where the overlay is opened and where a failure to open it is
             * deliberately fatal rather than a silent unrecorded run. */
            (void)arm_newest_overlay(e, &paths, boot);
            memset(&report, 0, sizeof report);
            if (vm_firmware_boot_start(boot, machine, &boot_paths,
                                       values, count, &report)) {
                char warning[VM_BOOT_OPTIONS_SUMMARY_CAPACITY];
                warning[0] = '\0';
                vm_boot_options_startup_warning(&report.options, warning,
                                                 sizeof warning);
                console_printf(e,
                    "[vm] Apple firmware: kernel at pa 0x%08x, device tree at "
                    "0x%08x (%u bytes), boot_args at 0x%08x\n",
                    report.bringup.entry_pa, report.bringup.devicetree_pa,
                    report.bringup.devicetree_size,
                    report.bringup.boot_args_pa);
                console_printf(e,
                    "[vm] topOfKernelData 0x%08x, /vram pool 0x%08x+0x%x, "
                    "free page pool %.1f MB\n",
                    report.bringup.top_of_kernel_data_pa,
                    report.bringup.framebuffer_pa, report.bringup.vram_bytes,
                    report.bringup.free_pool_bytes / 1048576.0);
                console_printf(e, "[vm] boot arguments: \"%s\"\n",
                               report.bringup.cmdline);
                if (report.options.summary[0])
                    console_printf(e, "[vm] settings: %s\n",
                                   report.options.summary);
                pthread_mutex_lock(&e->lock);
                snprintf(e->mode, sizeof e->mode, "%s", report.summary);
                snprintf(e->note, sizeof e->note, "%s",
                         warning[0] ? warning : note);
                pthread_mutex_unlock(&e->lock);
                *out_boot = boot;
                return true;
            }
            console_printf(e,
                "[vm] FIRMWARE BOOT FAILED: %s\n"
                "[vm] falling back to the built-in test guest\n",
                report.detail);
            pthread_mutex_lock(&e->lock);
            snprintf(e->note, sizeof e->note, "%s",
                     report.detail[0] ? report.detail
                                      : "Apple's kernel could not be started.");
            pthread_mutex_unlock(&e->lock);
            vm_firmware_boot_destroy(&boot);
            s5l8900_free(machine);
            memset(machine, 0, sizeof *machine);
            if (!s5l8900_init(machine, VM_GUEST_RAM_BASE, VM_GUEST_RAM_SIZE))
                return false;
#if defined(S5LBOX_DYNARMIC_JIT)
            dynarmic_jit_t *jit = dynarmic_jit_create(machine);
            if (jit) {
                s5l8900_set_jit(machine, jit, &g_dynarmic_jit_ops);
            }
#endif
        } else {
            pthread_mutex_lock(&e->lock);
            snprintf(e->note, sizeof e->note,
                     "Not enough memory to start Apple's kernel.");
            pthread_mutex_unlock(&e->lock);
        }
    }

    pthread_mutex_lock(&e->lock);
    if (e->note[0] == '\0' && state.detail[0])
        snprintf(e->note, sizeof e->note, "%s", state.detail);
    pthread_mutex_unlock(&e->lock);

    if (!vm_guest_install(machine)) return false;
    pthread_mutex_lock(&e->lock);
    snprintf(e->mode, sizeof e->mode, "built-in test guest");
    if (e->note[0] == '\0') snprintf(e->note, sizeof e->note, "%s", note);
    pthread_mutex_unlock(&e->lock);
    return true;
}

/* ------------------------------------------------------------------ input --- */

static void drain_one_touch(engine_t *e, s5l8900_t *machine)
{
    s5l_mt_contact_t contact;
    pthread_mutex_lock(&e->lock);
    if (!vm_touch_queue_peek(&e->touch, &contact)) {
        pthread_mutex_unlock(&e->lock);
        return;
    }
    if (s5l8900_set_contacts(machine, &contact, 1u)) {
        vm_touch_queue_pop(&e->touch);
        vm_touch_delivery_note_accepted(&e->touch_delivery, &contact);
        e->touch_delivered++;
        pthread_mutex_unlock(&e->lock);
        s5l8900_tick(machine, 0);
        return;
    }
    if (s5l_mtz2_irq(&machine->mtz2)) {
        pthread_mutex_unlock(&e->lock);
        return;
    }
    vm_touch_queue_pop(&e->touch);
    e->touch.dropped++;
    e->touch_dropped++;
    pthread_mutex_unlock(&e->lock);
}

static void drain_one_button(engine_t *e, s5l8900_t *machine)
{
    vm_button_event_t event;
    bool have;
    vm_button_power_hold_t power;
    vm_button_momentary_holds_t momentary;

    pthread_mutex_lock(&e->lock);
    have = vm_button_queue_peek(&e->button_queue, &event);
    if (!have && vm_button_ringer_reconcile(&machine->buttons,
            e->ringer_silent, &event)) {
        vm_button_queue_push(&e->button_queue, event.which, event.pressed);
        have = true;
    }
    power = e->power_hold;
    momentary = e->momentary;
    pthread_mutex_unlock(&e->lock);
    if (!have) return;

    {
        uint64_t now_ns = engine_now_ns();
        uint64_t now_cycles = machine->cpu.cycles;
        bool display_running = s5l_clcd_running(&machine->clcd);
        bool waking_home;
        if (!vm_button_momentary_release_ready(&event, &momentary, now_ns,
                                               display_running))
            return;
        if (!vm_button_power_release_ready(&event, &power, now_ns, now_cycles,
                                           display_running))
            return;
        waking_home = vm_button_home_wake_hold_needed(&event, display_running,
            s5l_pcf50635_in_hibernation(&machine->pmu),
            s5l_pcf50635_in_standby(&machine->pmu));
        if (s5l8900_set_button(machine, event.which, event.pressed)) {
            if (now_ns == 0) now_ns = engine_now_ns();
            now_cycles = machine->cpu.cycles;
            pthread_mutex_lock(&e->lock);
            vm_button_queue_pop(&e->button_queue);
            e->button_delivered++;
            vm_button_momentary_note_accepted(&event, &e->momentary, now_ns,
                                              waking_home);
            if (event.which == S5L_BUTTON_HOLD) {
                if (event.pressed) {
                    e->power_hold.active = true;
                    e->power_hold.display_running_at_press = display_running;
                    e->power_hold.delivered_ns = now_ns;
                    e->power_hold.delivered_cycles = now_cycles;
                } else {
                    memset(&e->power_hold, 0, sizeof e->power_hold);
                }
            }
            pthread_mutex_unlock(&e->lock);
            return;
        }
    }
    pthread_mutex_lock(&e->lock);
    e->button_dropped++;
    pthread_mutex_unlock(&e->lock);
}

static void quiesce_input(engine_t *e, s5l8900_t *machine)
{
    s5l_mt_contact_t release;
    bool have_release = false;
    pthread_mutex_lock(&e->lock);
    vm_touch_queue_cancel_pending(&e->touch);
    vm_button_queue_cancel_pending(&e->button_queue);
    have_release = vm_touch_delivery_make_break(&e->touch_delivery, &release);
    pthread_mutex_unlock(&e->lock);
    if (have_release && s5l8900_set_contacts(machine, &release, 1u)) {
        pthread_mutex_lock(&e->lock);
        vm_touch_delivery_note_accepted(&e->touch_delivery, &release);
        pthread_mutex_unlock(&e->lock);
    }
    s5l8900_tick(machine, 0);
}

/* --------------------------------------------------------------- display --- */

static void publish_blank_locked(engine_t *e)
{
    if (!e->snapshot) return;
    memset(e->snapshot, 0, VM_FB_BYTES);
    e->snapshot_argb = false;
    e->snapshot_fresh = true;
    e->snapshot_blank = true;
    e->snap_w = VM_FB_WIDTH;
    e->snap_h = VM_FB_HEIGHT;
    e->snap_stride = VM_FB_WIDTH * VM_FB_BPP;
}

static void post_frame(engine_t *e)
{
    (void)e;
}

static const char *status_text(arm_status_t status, const s5l8900_t *machine)
{
    static char line[96];
    switch (status) {
    case ARM_OK: return "running";
    case ARM_UNDEFINED:
        snprintf(line, sizeof line, "undefined instruction at 0x%08x",
                 machine->cpu.r[15]);
        return line;
    case ARM_HALT:
        snprintf(line, sizeof line, "halted at 0x%08x", machine->cpu.r[15]);
        return line;
    default:
        snprintf(line, sizeof line, "cpu status %d", (int)status);
        return line;
    }
}

static void publish_retired(engine_t *e, s5l8900_t *machine,
                            uint64_t retired, double instant_rate,
                            arm_status_t status)
{
    uint32_t width = 0, height = 0, stride = 0;
    vm_pixel_order_t order = VM_ORDER_BGRA;
    const uint8_t *fb = vm_guest_display(machine, &width, &height,
                                         &stride, &order);
    size_t bytes = 0;
    double now = engine_now();

    if (!fb || width == 0 || height == 0 ||
        stride < (size_t)width * VM_FB_BPP ||
        (size_t)stride * height > VM_FB_BYTES ||
        (size_t)stride * height == 0) {
        fb = NULL;
    } else {
        bytes = (size_t)stride * height;
    }

    pthread_mutex_lock(&e->lock);
    if (machine->uart0.tx_len > 0) {
        size_t captured = machine->uart0.tx_len;
        if (captured > UART_TX_BUFFER - 1u) captured = UART_TX_BUFFER - 1u;
        console_append_len_locked(e, machine->uart0.tx, captured);
        web_log_post_raw("kernel", machine->uart0.tx, captured);
        machine->uart0.tx_len = 0;
    }
    if (fb && e->snapshot) {
        uint32_t guest_width = width, guest_height = height;
        bool changed = !e->have_frame || e->snapshot_blank ||
            e->snap_w != guest_width || e->snap_h != guest_height ||
            e->snap_stride != stride ||
            e->snapshot_argb != (order == VM_ORDER_ARGB) ||
            memcmp(e->snapshot, fb, bytes) != 0;
        if (changed) {
            memcpy(e->snapshot, fb, bytes);
            if (bytes < VM_FB_BYTES)
                memset(e->snapshot + bytes, 0, VM_FB_BYTES - bytes);
            e->snapshot_argb = order == VM_ORDER_ARGB;
            e->snap_w = guest_width;
            e->snap_h = guest_height;
            e->snap_stride = stride;
            e->snapshot_fresh = true;
            e->snapshot_blank = false;
            e->have_frame = true;
            e->fps_window_frames++;
        }
        if (e->fps_window_start <= 0.0) {
            e->fps_window_start = now;
        } else if (now - e->fps_window_start >= 0.5) {
            e->fps = (double)e->fps_window_frames /
                     (now - e->fps_window_start);
            e->fps_window_frames = 0;
            e->fps_window_start = now;
        }
    } else if (e->snapshot && !e->snapshot_blank) {
        publish_blank_locked(e);
    }

    e->retired = retired;
    if (e->paused && e->phase == PHASE_RUNNING) {
        e->rate = 0.0;
        snprintf(e->status, sizeof e->status, "paused");
    } else {
        e->rate = e->rate > 0.0 ? e->rate * 0.8 + instant_rate * 0.2
                                : instant_rate;
        snprintf(e->status, sizeof e->status, "%s",
                 status_text(status, machine));
    }
    e->last_arm_status = status;
    web_log_set_status(retired, e->fps, e->rate,
                       machine ? machine->cpu.cp15.ttbr0 : 0u, e->status);
    pthread_mutex_unlock(&e->lock);

    if (machine) {
        static uint64_t s_last_mbx_rejected = 0;
        static uint64_t s_last_mbx_completed = 0;
        static uint64_t s_last_mbx_degraded = 0;
        static uint64_t s_last_pre_step_matches = 0;
        static uint64_t s_last_pre_step_handled = 0;
        static double s_last_telemetry_time = 0.0;

        if (now - s_last_telemetry_time >= 2.5) {
            s_last_telemetry_time = now;

            if (machine->mbx_telemetry.rejected_3d != s_last_mbx_rejected) {
                uint64_t diff = machine->mbx_telemetry.rejected_3d - s_last_mbx_rejected;
                uint64_t seq = machine->mbx_telemetry.rejected_3d;
                s_last_mbx_rejected = machine->mbx_telemetry.rejected_3d;
                const s5l_mbx_3d_rejection_witness_t *w =
                    &machine->mbx_telemetry.rejected_3d_history[(seq - 1u) % S5L_MBX_3D_REJECTION_HISTORY];
                web_log_printf("gles", "[MBX3D] Reject +%llu (total %llu): fail_word=0x%08x fb_ctrl=0x%x target=0x%08x",
                               (unsigned long long)diff, (unsigned long long)seq,
                               w->ta_failure_word, w->framebuffer_control, w->target);
            }
            if (machine->mbx_telemetry.completed_3d != s_last_mbx_completed) {
                uint64_t diff = machine->mbx_telemetry.completed_3d - s_last_mbx_completed;
                s_last_mbx_completed = machine->mbx_telemetry.completed_3d;
                web_log_printf("gles", "[MBX3D] Complete +%llu (total %llu): pixels=%llu",
                               (unsigned long long)diff, (unsigned long long)machine->mbx_telemetry.completed_3d,
                               (unsigned long long)machine->mbx_telemetry.pixels_3d);
            }
            if (machine->mbx_telemetry.degraded_3d != s_last_mbx_degraded) {
                uint64_t diff = machine->mbx_telemetry.degraded_3d - s_last_mbx_degraded;
                s_last_mbx_degraded = machine->mbx_telemetry.degraded_3d;
                web_log_printf("gles", "[MBX3D] Degraded +%llu (total %llu)",
                               (unsigned long long)diff, (unsigned long long)machine->mbx_telemetry.degraded_3d);
            }

            uint64_t matches = s5l8900_pre_step_matches(machine);
            uint64_t handled = s5l8900_pre_step_handled(machine);
            if (matches != s_last_pre_step_matches || handled != s_last_pre_step_handled) {
                web_log_printf("gles", "[HLE GLES] matches=+%llu (total %llu), handled=+%llu (total %llu)",
                               (unsigned long long)(matches - s_last_pre_step_matches), (unsigned long long)matches,
                               (unsigned long long)(handled - s_last_pre_step_handled), (unsigned long long)handled);
                s_last_pre_step_matches = matches;
                s_last_pre_step_handled = handled;
            }
        }
    }

    post_frame(e);
}

/* ------------------------------------------------------------------- run --- */

static void engine_thread_main(engine_t *e)
{
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#else
    (void)setpriority(PRIO_PROCESS, 0, -4);
#endif

    s5l8900_t *machine = calloc(1, sizeof *machine);
    vm_firmware_boot_t *boot = NULL;
    bool ready = false;
    uint64_t retired = 0, retired_at_publish = 0;
    arm_status_t status = ARM_OK;
    double last_publish, next_publish;
    bool stopped_by_request = false;
    bool reached_cap = false;
    bool checkpoint_saved = false;
    bool pause_active = false;
    uint64_t pause_started_ns = 0;
    bool audio_running = false;
#if defined(__linux__)
    bool startup_display_seen = false;
    bool startup_wake_sent = false;
#endif

    if (!machine) {
        pthread_mutex_lock(&e->lock);
        e->note[0] = '\0';
        snprintf(e->note, sizeof e->note,
                 "Not enough memory for the emulated machine.");
        e->phase = PHASE_IDLE;
        e->thread_live = false;
        pthread_mutex_unlock(&e->lock);
        return;
    }
    if (!s5l8900_init(machine, VM_GUEST_RAM_BASE, VM_GUEST_RAM_SIZE)) {
        free(machine);
        pthread_mutex_lock(&e->lock);
        snprintf(e->note, sizeof e->note,
                 "Could not allocate 128 MB of guest DRAM.");
        e->phase = PHASE_IDLE;
        e->thread_live = false;
        pthread_mutex_unlock(&e->lock);
        return;
    }

#if defined(S5LBOX_DYNARMIC_JIT)
    dynarmic_jit_t *jit = dynarmic_jit_create(machine);
    if (jit) {
        s5l8900_set_jit(machine, jit, &g_dynarmic_jit_ops);
        console_append(e, "[jit] Dynarmic A32 JIT compiler activated\n");
    } else {
        console_append(e, "[jit] Dynarmic JIT initialization failed; using interpreter\n");
    }
#endif

    ready = install_payload(e, machine, &boot);
    if (!ready) {
        s5l8900_free(machine);
        free(machine);
        pthread_mutex_lock(&e->lock);
        if (e->note[0] == '\0')
            snprintf(e->note, sizeof e->note,
                     "The machine could not be started.");
        e->phase = PHASE_IDLE;
        e->machine_ready = false;
        e->thread_live = false;
        pthread_mutex_unlock(&e->lock);
        return;
    }

    if (e->audio_ok) {
        machine->i2s[0].frame = vm_audio_guest_frame;
        machine->i2s[0].frame_ctx = &e->audio;
        vm_audio_buffer_active(&e->audio, true);
        atomic_store_explicit(&e->audio.host_running, true, memory_order_release);
        audio_running = true;
    }

    e->usb_active = machine->usbotg.enabled != 0;
    if (e->usb_active) {
        bool was_connected = machine->usbotg.connected != 0;
        usb_host_disconnect(&e->usb_host, machine);
        if (!was_connected) usb_host_init(&e->usb_host, machine);
    }

    pthread_mutex_lock(&e->lock);
    e->machine_ready = true;
    e->phase = e->paused ? PHASE_PAUSED : PHASE_RUNNING;
    snprintf(e->status, sizeof e->status,
             e->paused ? "paused" : "running");
    pthread_mutex_unlock(&e->lock);

    console_printf(e, "[vm] guest DRAM: %u MB at 0x%08x, framebuffer at 0x%08x\n",
                   VM_GUEST_RAM_SIZE / (1024u * 1024u), VM_GUEST_RAM_BASE,
                   vm_guest_fb_pa(VM_GUEST_RAM_BASE, VM_GUEST_RAM_SIZE));

    init_hyperhle_gles(e);
    s5l8900_set_uart0_host(machine, engine_uart0_tx_handler, e);
    s5l8900_set_user_observers(machine, engine_user_svc_observer, NULL, e);

    last_publish = engine_now();
    next_publish = last_publish + ENGINE_PUBLISH_SECONDS;

    for (;;) {
        bool stop, paused;
        pthread_mutex_lock(&e->lock);
        stop = e->stop_requested;
        paused = e->paused;
        if (!stop)
            e->phase = paused ? PHASE_PAUSED : PHASE_RUNNING;
        pthread_mutex_unlock(&e->lock);

        if (stop) {
            stopped_by_request = true;
            break;
        }

        if (paused) {
            if (!pause_active) {
                pause_active = true;
                pause_started_ns = engine_now_ns();
            }
            usleep(50 * 1000);
            last_publish = engine_now();
            next_publish = last_publish + ENGINE_PUBLISH_SECONDS;
            retired_at_publish = retired;
            continue;
        }
        if (pause_active) {
            uint64_t resumed = engine_now_ns();
            if (pause_started_ns && resumed >= pause_started_ns) {
                (void)s5l8900_resume_active_host_clock(machine,
                                                       pause_started_ns,
                                                       resumed);
                pthread_mutex_lock(&e->lock);
                vm_button_holds_resume(&e->power_hold, &e->momentary,
                                       pause_started_ns, resumed);
                pthread_mutex_unlock(&e->lock);
            }
            pause_active = false;
            pause_started_ns = 0;
            last_publish = engine_now();
            next_publish = last_publish + ENGINE_PUBLISH_SECONDS;
            retired_at_publish = retired;
        }

        drain_one_touch(e, machine);
        drain_one_button(e, machine);

        retired += s5l8900_run(machine, ENGINE_CHUNK_INSTRUCTIONS, &status);
#if defined(__linux__)
        if (boot && s5l_clcd_running(&machine->clcd))
            startup_display_seen = true;
        if (boot && startup_display_seen && !startup_wake_sent &&
            s5l8900_hibernation_ready(machine)) {
            bool queued_press = engine_button(e, VM_BUTTON_POWER, true);
            bool queued_release = queued_press &&
                engine_button(e, VM_BUTTON_POWER, false);
            if (queued_press && queued_release) {
                startup_wake_sent = true;
                console_append(e,
                    "[vm] initial firmware boot reached sleep; "
                    "waking the lock screen once\n");
            }
        }
#endif


        if (s5l_pcf50635_in_standby(&machine->pmu)) {
            stopped_by_request = true;
            e->stop_mode = ENGINE_STOP_POWER_OFF;
            console_append(e, "[vm] guest standby confirmed; powering off\n");
            web_log_post("info", "[vm] guest standby confirmed; powering off");
            publish_retired(e, machine, retired, 0.0, ARM_OK);
            break;
        }

        if (e->usb_active) {
            usb_host_poll(&e->usb_host, machine);
            pthread_mutex_lock(&e->lock);
            e->usb_ready = usb_host_ready(&e->usb_host);
            pthread_mutex_unlock(&e->lock);
        }

        {
            uint64_t cap = atomic_load(&e->instruction_cap);
            double now = engine_now();
            double elapsed = now - last_publish;
            if (now >= next_publish || status != ARM_OK) {
                double instant = elapsed > 0.0
                    ? (double)(retired - retired_at_publish) / elapsed : 0.0;
                publish_retired(e, machine, retired, instant, status);
                last_publish = now;
                retired_at_publish = retired;
                next_publish += ENGINE_PUBLISH_SECONDS;
                if (next_publish <= now)
                    next_publish = now + ENGINE_PUBLISH_SECONDS;
            }
            if (status != ARM_OK) {
                console_printf(e, "[vm] CPU stop / fault: status=%d at PC=0x%08x, LR=0x%08x, CPSR=0x%08x, retired=%llu\n",
                               status, machine->cpu.r[15], machine->cpu.r[14], machine->cpu.cpsr, (unsigned long long)retired);
                web_log_printf("crash", "[CPU FAULT] status=%d at PC=0x%08x, LR=0x%08x, SP=0x%08x, CPSR=0x%08x, retired=%llu",
                               status, machine->cpu.r[15], machine->cpu.r[14], machine->cpu.r[13], machine->cpu.cpsr, (unsigned long long)retired);
                web_log_printf("crash", "r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x",
                               machine->cpu.r[0], machine->cpu.r[1], machine->cpu.r[2], machine->cpu.r[3],
                               machine->cpu.r[4], machine->cpu.r[5], machine->cpu.r[6], machine->cpu.r[7]);
                web_log_printf("crash", "r8=0x%08x r9=0x%08x r10=0x%08x r11=0x%08x r12=0x%08x",
                               machine->cpu.r[8], machine->cpu.r[9], machine->cpu.r[10], machine->cpu.r[11], machine->cpu.r[12]);
                break;
            }
            if (cap > 0 && retired >= cap) {
                reached_cap = true;
                publish_retired(e, machine, retired, 0.0, status);
                break;
            }
        }
    }

    if (stopped_by_request)
        publish_retired(e, machine, retired, 0.0, ARM_OK);

    if (e->stop_mode == ENGINE_STOP_CHECKPOINT && boot && !reached_cap) {
        quiesce_input(e, machine);
        {
            uint64_t deadline = engine_now_ns() + UINT64_C(15000000000);
            for (;;) {
                char why[VM_FW_BOOT_DETAIL_CAPACITY] = {0};
                vm_firmware_checkpoint_status_t saved =
                    vm_firmware_boot_save_resume_cancellable(
                        boot, machine, why, sizeof why, abort_cb, e);
                if (saved == VM_FW_CHECKPOINT_OK) {
                    checkpoint_saved = true;
                    console_append(e,
                        "[vm] automatic resume checkpoint saved\n");
                    web_log_post("info", "[vm] automatic resume checkpoint saved");
                    pthread_mutex_lock(&e->lock);
                    snprintf(e->note, sizeof e->note,
                             "The machine was saved and will resume here next "
                             "time.");
                    pthread_mutex_unlock(&e->lock);
                    break;
                }
                if (saved == VM_FW_CHECKPOINT_ERROR ||
                    atomic_load(&e->abort_flag) ||
                    engine_now_ns() >= deadline) {
                    pthread_mutex_lock(&e->lock);
                    snprintf(e->note, sizeof e->note, "%s",
                             why[0] ? why
                                    : "The checkpoint could not be saved.");
                    pthread_mutex_unlock(&e->lock);
                    if (why[0]) console_printf(e, "[vm] checkpoint failed: %s\n",
                                               why);
                    break;
                }
                /* BUSY: the memory-disk bridge is mid-continuation. */
                retired += s5l8900_run(machine, ENGINE_CHUNK_INSTRUCTIONS,
                                       &status);
                if (status != ARM_OK) {
                    console_printf(e, "[vm] CPU stop / fault during checkpoint: status=%d at PC=0x%08x, LR=0x%08x\n",
                                   status, machine->cpu.r[15], machine->cpu.r[14]);
                    break;
                }
            }
        }
    }

    if (audio_running) {
        atomic_store_explicit(&e->audio.host_running, false, memory_order_release);
        vm_audio_buffer_active(&e->audio, false);
        audio_running = false;
    }
    if (e->usb_active) {
        usb_host_disconnect(&e->usb_host, machine);
    }

    s5l8900_set_uart0_host(machine, NULL, NULL);
    s5l8900_set_user_observers(machine, NULL, NULL, NULL);
    term_hyperhle_gles();
    web_log_flush_raw();
    s5l8900_free(machine);
    free(machine);
    vm_firmware_boot_destroy(&boot);

    if (e->stop_mode == ENGINE_STOP_POWER_OFF) {
        vm_instance_paths_t paths;
        if (e->instance_id[0] &&
            vm_instance_paths_derive(e->firmware, e->machines,
                                     e->instance_id, &paths) ==
                VM_INSTANCE_PATHS_OK) {
            char why[VM_FW_BOOT_DETAIL_CAPACITY] = {0};
            (void)vm_resume_checkpoint_disarm(paths.machine, why, sizeof why);
        }
    }

    pthread_mutex_lock(&e->lock);
    {
        int index = e->instance_id[0] ? instance_index_locked(e, e->instance_id)
                                      : -1;
        if (index >= 0) {
            e->instances.slot[index].retired_total += retired;
            (void)persist_instances_locked(e);
        }
    }
    e->machine_ready = false;
    e->preparing_root = false;
    e->usb_active = false;
    e->usb_ready = false;
    e->boot = NULL;
    e->phase = PHASE_IDLE;
    e->instance_id[0] = '\0';
    e->machine_log_path[0] = '\0';
    e->thread_live = false;
    if (reached_cap)
        snprintf(e->status, sizeof e->status, "instruction cap reached");
    else if (checkpoint_saved)
        snprintf(e->status, sizeof e->status, "checkpoint saved");
    else if (e->stop_mode == ENGINE_STOP_POWER_OFF)
        snprintf(e->status, sizeof e->status, "powered off");
    else if (stopped_by_request)
        snprintf(e->status, sizeof e->status, "stopped");
    else if (status != ARM_OK)
        snprintf(e->status, sizeof e->status, "guest stopped (cpu status %d)",
                 (int)status);
    e->rate = 0.0;
    e->paused = false;
    e->stop_requested = false;
    e->stop_mode = ENGINE_STOP_PLAIN;
    publish_blank_locked(e);
    pthread_mutex_unlock(&e->lock);
    post_frame(e);
}

static void *engine_thread_entry(void *ctx)
{
    engine_thread_main(ctx);
    return NULL;
}

/* --------------------------------------------------------------- lifecycle --- */

engine_t *engine_create(const char *root_directory)
{
    engine_t *e;
    if (!root_directory || !root_directory[0]) return NULL;
    e = calloc(1, sizeof *e);
    if (!e) return NULL;
    if (snprintf(e->root, sizeof e->root, "%s", root_directory) <= 0 ||
        strlen(root_directory) >= sizeof e->root) {
        free(e);
        return NULL;
    }
    if (!join_path(e->firmware, sizeof e->firmware, e->root, "firmware") ||
        !join_path(e->machines, sizeof e->machines, e->root, "Machines")) {
        free(e);
        return NULL;
    }
    pthread_mutex_init(&e->lock, NULL);
    vm_touch_queue_reset(&e->touch);
    vm_touch_delivery_reset(&e->touch_delivery);
    vm_touch_tracker_reset(&e->tracker);
    vm_button_queue_reset(&e->button_queue);
    e->audio_ok = vm_audio_buffer_init(&e->audio);
    atomic_init(&e->abort_flag, false);
    atomic_init(&e->prepare_done, 0);
    atomic_init(&e->prepare_total, 0);
    atomic_init(&e->instruction_cap, 0);
    atomic_init(&e->import_active, false);
    atomic_init(&e->import_cancel, false);
    atomic_init(&e->import_ok, false);
    atomic_init(&e->import_artefact, 0);
    atomic_init(&e->import_stage, 0);
    atomic_init(&e->import_done, 0);
    atomic_init(&e->import_total, 0);
    atomic_init(&e->microphone_on, false);
    e->snapshot = calloc(1, VM_FB_BYTES);
    mkdir_p(e->root);
    mkdir_p(e->firmware);
    mkdir_p(e->machines);
    load_instances(e);
    e->phase = PHASE_IDLE;
    snprintf(e->status, sizeof e->status, "idle");
    snprintf(e->mode, sizeof e->mode, "not started");
    web_log_server_start(8080);
    return e;
}

static void join_worker(engine_t *e)
{
    bool join = false;
    pthread_mutex_lock(&e->lock);
    if (e->join_pending) {
        e->join_pending = false;
        join = true;
    }
    pthread_mutex_unlock(&e->lock);
    if (join) pthread_join(e->thread, NULL);
}

void engine_destroy(engine_t *engine)
{
    engine_t *e = engine;
    if (!e) return;
    pthread_mutex_lock(&e->lock);
    e->stop_requested = true;
    atomic_store(&e->abort_flag, true);
    atomic_store(&e->import_cancel, true);
    if (e->thread_live) e->phase = PHASE_STOPPING;
    pthread_mutex_unlock(&e->lock);

    if (e->thread_live || e->join_pending) {
        pthread_join(e->thread, NULL);
        e->thread_live = false;
        e->join_pending = false;
    }

    free(e->snapshot);
    pthread_mutex_destroy(&e->lock);
    web_log_server_stop();
    free(e);
}

int engine_start(engine_t *engine, const char *instance_id,
                 char *detail, size_t detail_capacity)
{
    engine_t *e = engine;
    int index = -1;
    unsigned i;
    pthread_t thread;

    if (detail && detail_capacity) detail[0] = '\0';
    if (!e) return -1;
    join_worker(e);

    pthread_mutex_lock(&e->lock);
    if (e->thread_live || e->phase != PHASE_IDLE) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "The previous machine is still stopping.");
        return -1;
    }
    if (instance_id && instance_id[0])
        index = instance_index_locked(e, instance_id);

    e->instance_id[0] = '\0';
    memset(e->options, 0, sizeof e->options);
    e->option_count = 0;
    if (instance_id && instance_id[0]) {
        if (index < 0) {
            pthread_mutex_unlock(&e->lock);
            if (detail && detail_capacity)
                snprintf(detail, detail_capacity, "No such machine.");
            return -1;
        }
        snprintf(e->instance_id, sizeof e->instance_id, "%s", instance_id);
        e->option_count = vm_option_count();
        if (e->option_count > VM_INSTANCE_OPTION_MAX)
            e->option_count = VM_INSTANCE_OPTION_MAX;
        for (i = 0; i < e->option_count; i++)
            e->options[i] = e->instances.slot[index].options[i];
        e->instances.slot[index].last_opened_unix = engine_unix_now();
        (void)persist_instances_locked(e);
    } else {
        e->option_count = vm_option_count();
        if (e->option_count > VM_INSTANCE_OPTION_MAX)
            e->option_count = VM_INSTANCE_OPTION_MAX;
        for (i = 0; i < e->option_count; i++)
            e->options[i] = engine_new_machine_option_value(i);
    }

    e->stop_requested = false;
    e->stop_mode = ENGINE_STOP_PLAIN;
    e->paused = false;
    e->machine_ready = false;
    e->preparing_root = false;
    e->phase = PHASE_STARTING;
    e->retired = 0;
    e->rate = 0.0;
    e->fps = 0.0;
    e->fps_window_start = 0.0;
    e->fps_window_frames = 0;
    e->have_frame = false;
    e->snapshot_fresh = false;
    e->snapshot_blank = false;
    e->touch_delivered = e->touch_dropped = 0;
    e->button_delivered = e->button_dropped = 0;
    e->last_arm_status = ARM_OK;
    atomic_store(&e->abort_flag, false);
    atomic_store(&e->prepare_done, 0);
    atomic_store(&e->prepare_total, 0);
    vm_touch_queue_reset(&e->touch);
    vm_touch_delivery_reset(&e->touch_delivery);
    vm_touch_tracker_reset(&e->tracker);
    vm_button_queue_reset(&e->button_queue);
    memset(e->buttons, 0, sizeof e->buttons);
    e->ringer_silent = false;
    memset(&e->power_hold, 0, sizeof e->power_hold);
    memset(&e->momentary, 0, sizeof e->momentary);
    e->console_len = 0;
    e->console[0] = '\0';
    e->note[0] = '\0';
    snprintf(e->status, sizeof e->status, "starting");
    snprintf(e->mode, sizeof e->mode, "starting");
    publish_blank_locked(e);
    pthread_mutex_unlock(&e->lock);

    if (pthread_create(&thread, NULL, engine_thread_entry, e) != 0) {
        pthread_mutex_lock(&e->lock);
        e->phase = PHASE_IDLE;
        snprintf(e->status, sizeof e->status, "stopped");
        snprintf(e->note, sizeof e->note,
                 "The emulator thread could not be started.");
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "The emulator thread could not be started.");
        return -1;
    }
    pthread_mutex_lock(&e->lock);
    e->thread = thread;
    e->thread_live = true;
    e->join_pending = true;
    pthread_mutex_unlock(&e->lock);
    return 0;
}

void engine_stop(engine_t *engine, engine_stop_mode_t mode)
{
    engine_t *e = engine;
    if (!e) return;
    pthread_mutex_lock(&e->lock);
    if (e->phase == PHASE_IDLE) {
        pthread_mutex_unlock(&e->lock);
        return;
    }
    if (mode == ENGINE_STOP_POWER_OFF) {
        e->stop_mode = ENGINE_STOP_POWER_OFF;
        atomic_store(&e->abort_flag, true);
        snprintf(e->status, sizeof e->status, "powering off");
    } else if (mode == ENGINE_STOP_CHECKPOINT &&
               e->stop_mode != ENGINE_STOP_POWER_OFF) {
        e->stop_mode = ENGINE_STOP_CHECKPOINT;
        snprintf(e->status, sizeof e->status, "saving checkpoint");
    } else {
        snprintf(e->status, sizeof e->status, "stopping");
    }
    e->stop_requested = true;
    e->paused = false;
    e->phase = PHASE_STOPPING;
    pthread_mutex_unlock(&e->lock);
}

void engine_set_paused(engine_t *engine, bool paused)
{
    engine_t *e = engine;
    if (!e) return;
    pthread_mutex_lock(&e->lock);
    if (e->phase == PHASE_IDLE) {
        pthread_mutex_unlock(&e->lock);
        return;
    }
    e->paused = paused;
    if (paused) snprintf(e->status, sizeof e->status, "paused");
    else if (e->machine_ready)
        snprintf(e->status, sizeof e->status, "running");
    pthread_mutex_unlock(&e->lock);
}

void engine_set_instruction_cap(engine_t *engine, uint64_t cap)
{
    if (engine) atomic_store(&engine->instruction_cap, cap);
}

int engine_status_json(engine_t *engine, char *out, size_t capacity)
{
    static const char *names[] = {
        "idle", "starting", "running", "paused", "stopping", "error"
    };
    engine_t *e = engine;
    json_t j;
    int phase;
    char status[192], mode[128], note[320], id[VM_INSTANCE_ID_LEN + 1u];
    bool ready, preparing, usb_ready;
    uint64_t retired, done, total, fps_frames;
    double rate, fps;
    arm_status_t arm_status;
    size_t console_len;

    if (!e || !out || capacity == 0) return -1;
    pthread_mutex_lock(&e->lock);
    phase = e->phase;
    if (phase < PHASE_IDLE || phase > PHASE_ERROR) phase = PHASE_ERROR;
    snprintf(status, sizeof status, "%s", e->status);
    snprintf(mode, sizeof mode, "%s", e->mode);
    snprintf(note, sizeof note, "%s", e->note);
    snprintf(id, sizeof id, "%s", e->instance_id);
    ready = e->machine_ready;
    preparing = e->preparing_root;
    usb_ready = e->usb_ready;
    retired = e->retired;
    rate = e->rate;
    fps = e->fps;
    fps_frames = e->fps_window_frames;
    arm_status = e->last_arm_status;
    console_len = e->console_len;
    pthread_mutex_unlock(&e->lock);

    done = atomic_load(&e->prepare_done);
    total = atomic_load(&e->prepare_total);

    json_reset(&j, out, capacity);
    json_printf(&j,
        "{\"phase\":\"%s\",\"phaseCode\":%d,\"running\":%s,\"paused\":%s,"
        "\"machineReady\":%s,\"preparing\":%s,\"instanceId\":",
        names[phase], phase, phase >= PHASE_RUNNING && phase <= PHASE_PAUSED &&
        ready ? "true" : "false",
        phase == PHASE_PAUSED ? "true" : "false",
        ready ? "true" : "false", preparing ? "true" : "false");
    json_string(&j, id);
    json_printf(&j,
        ",\"status\":");
    json_string(&j, status);
    json_raw(&j, ",\"mode\":");
    json_string(&j, mode);
    json_raw(&j, ",\"note\":");
    json_string(&j, note);
    json_printf(&j,
        ",\"retired\":%llu,\"rate\":%.3f,\"fps\":%.3f,\"fpsFrames\":%llu,"
        "\"prepareDone\":%llu,\"prepareTotal\":%llu,\"cpuStatus\":%d,"
        "\"usb\":%s,\"audio\":%s,\"consolePending\":%zu}",
        (unsigned long long)retired, rate, fps,
        (unsigned long long)fps_frames,
        (unsigned long long)done, (unsigned long long)total, (int)arm_status,
        usb_ready ? "true" : "false", e->audio_ok ? "true" : "false",
        console_len);
    return (int)j.len;
}

size_t engine_console_drain(engine_t *engine, char *out, size_t capacity)
{
    engine_t *e = engine;
    size_t take;
    if (!e || !out || capacity == 0) return 0;
    pthread_mutex_lock(&e->lock);
    take = e->console_len < capacity - 1u ? e->console_len : capacity - 1u;
    memcpy(out, e->console, take);
    out[take] = '\0';
    if (take < e->console_len) {
        memmove(e->console, e->console + take, e->console_len - take);
        e->console_len -= take;
        e->console[e->console_len] = '\0';
    } else {
        e->console_len = 0;
        e->console[0] = '\0';
    }
    pthread_mutex_unlock(&e->lock);
    return take;
}

uint16_t engine_get_web_log_port(engine_t *engine)
{
    (void)engine;
    return web_log_server_get_port();
}

size_t engine_log_entries_json(engine_t *engine, uint32_t last_id, char *out, size_t capacity)
{
    (void)engine;
    return web_log_get_entries_json(last_id, out, capacity);
}

/* ---------------------------------------------------------------- display --- */

bool engine_copy_frame(engine_t *engine, void *dst_pixels, size_t dst_capacity,
                       uint32_t *out_width, uint32_t *out_height)
{
    if (!engine || !dst_pixels) return false;
    pthread_mutex_lock(&engine->lock);
    if (!engine->snapshot || !engine->have_frame || !engine->snapshot_fresh) {
        pthread_mutex_unlock(&engine->lock);
        return false;
    }

    uint32_t width = engine->snap_w;
    uint32_t height = engine->snap_h;
    if (out_width) *out_width = width;
    if (out_height) *out_height = height;

    size_t needed = (size_t)width * height * 4u;
    if (dst_capacity < needed) {
        pthread_mutex_unlock(&engine->lock);
        return false;
    }

    const uint8_t *src = engine->snapshot;
    uint32_t stride = engine->snap_stride;
    bool argb = engine->snapshot_argb;
    uint32_t *dst32 = (uint32_t *)dst_pixels;

    for (uint32_t y = 0; y < height; y++) {
        const uint32_t *src32 = (const uint32_t *)(src + (size_t)y * stride);
        uint32_t *row_dst = dst32 + (size_t)y * width;
        if (argb) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t px = src32[x];
                uint8_t a = (uint8_t)(px >> 24);
                uint8_t r = (uint8_t)(px >> 16);
                uint8_t g = (uint8_t)(px >> 8);
                uint8_t b = (uint8_t)px;
                row_dst[x] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
            }
        } else {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t px = src32[x];
                uint8_t b = (uint8_t)px;
                uint8_t g = (uint8_t)(px >> 8);
                uint8_t r = (uint8_t)(px >> 16);
                row_dst[x] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xff000000u;
            }
        }
    }

    engine->snapshot_fresh = false;
    pthread_mutex_unlock(&engine->lock);
    return true;
}

bool engine_is_display_running(engine_t *engine)
{
    if (!engine) return false;
    pthread_mutex_lock(&engine->lock);
    bool running = engine->have_frame && !engine->snapshot_blank;
    pthread_mutex_unlock(&engine->lock);
    return running;
}

/* ------------------------------------------------------------------ audio --- */

size_t engine_read_audio_pcm(engine_t *engine, int16_t *dst_samples, size_t max_samples)
{
    if (!engine || !dst_samples || max_samples == 0 || !engine->audio_ok) return 0;

    unsigned frames = (unsigned)(max_samples / 2);
    if (frames > ENGINE_AUDIO_SCRATCH) frames = ENGINE_AUDIO_SCRATCH;

    vm_audio_render(&engine->audio, engine->audio_left, engine->audio_right, frames);

    for (unsigned i = 0; i < frames; i++) {
        float l = engine->audio_left[i];
        float r = engine->audio_right[i];
        if (l < -1.0f) l = -1.0f; else if (l > 1.0f) l = 1.0f;
        if (r < -1.0f) r = -1.0f; else if (r > 1.0f) r = 1.0f;
        dst_samples[i * 2]     = (int16_t)(l * 32767.0f);
        dst_samples[i * 2 + 1] = (int16_t)(r * 32767.0f);
    }

    return (size_t)frames * 2;
}

/* ------------------------------------------------------------------ input --- */

bool engine_touch(engine_t *engine, double x, double y,
                  double view_w, double view_h, int phase)
{
    engine_t *e = engine;
    vm_touch_point_t mapped;
    int guest_x = 0, guest_y = 0;
    s5l_mt_contact_t contact;
    bool queued = false;

    if (!e || phase < VM_TOUCH_BEGAN || phase > VM_TOUCH_CANCELLED)
        return false;
    mapped = vm_touch_map(view_w, view_h, VM_FB_WIDTH, VM_FB_HEIGHT, x, y);

    pthread_mutex_lock(&e->lock);
    if (!vm_touch_track(&e->tracker, (vm_touch_phase_t)phase, mapped,
                        &guest_x, &guest_y)) {
        pthread_mutex_unlock(&e->lock);
        return false;
    }
    if (!vm_touch_contact_from_ui((vm_touch_phase_t)phase, guest_x, guest_y,
                                  &contact)) {
        pthread_mutex_unlock(&e->lock);
        return false;
    }
    if (e->machine_ready && e->phase == PHASE_RUNNING)
        queued = vm_touch_queue_push(&e->touch, &contact);
    pthread_mutex_unlock(&e->lock);
    return queued;
}

bool engine_button(engine_t *engine, unsigned app_button, bool pressed)
{
    engine_t *e = engine;
    unsigned which = 0;
    bool guest_pressed = false;
    bool queued = false;
    if (!e || app_button >= VM_BUTTON_COUNT) return false;
    if (!vm_button_to_guest(app_button, pressed, &which, &guest_pressed))
        return false;
    pthread_mutex_lock(&e->lock);
    if (e->machine_ready && e->phase == PHASE_RUNNING)
        queued = vm_button_queue_push(&e->button_queue, which, guest_pressed);
    if (queued) {
        e->buttons[app_button] = pressed;
        if (app_button == VM_BUTTON_RINGER_SILENT)
            e->ringer_silent = pressed;
    }
    pthread_mutex_unlock(&e->lock);
    return queued;
}

bool engine_button_pressed(engine_t *engine, unsigned app_button)
{
    bool pressed = false;
    if (!engine || app_button >= VM_BUTTON_COUNT) return false;
    pthread_mutex_lock(&engine->lock);
    pressed = engine->buttons[app_button];
    pthread_mutex_unlock(&engine->lock);
    return pressed;
}

/* -------------------------------------------------------------- snapshots --- */

static bool list_snapshots_for(const vm_instance_paths_t *paths,
                               char *snapshots, size_t snapshots_cap,
                               vm_snapshot_info_t *infos, size_t capacity,
                               size_t *count, char *detail,
                               size_t detail_capacity)
{
    if (count) *count = 0;
    if (vm_snapshot_dir(paths->machine, snapshots, snapshots_cap) !=
        VM_SNAPSHOT_OK) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "This machine's snapshot directory could not be named.");
        return false;
    }
    if (!is_directory(snapshots)) return true;
    if (vm_snapshot_list(snapshots, infos, capacity, count,
                         detail, detail_capacity) != VM_SNAPSHOT_OK)
        return false;
    return true;
}

static int snapshot_index(const vm_snapshot_info_t *infos, size_t count,
                          const char *id)
{
    for (size_t i = 0; i < count; i++)
        if (!strcmp(infos[i].id, id)) return (int)i;
    return -1;
}

int engine_snapshots_json(engine_t *engine, const char *id,
                          char *out, size_t capacity)
{
    engine_t *e = engine;
    vm_instance_paths_t paths;
    vm_snapshot_info_t infos[VM_SNAPSHOT_MAX];
    size_t count = 0;
    char snapshots[ENGINE_PATH_CAP];
    char detail[VM_FW_BOOT_DETAIL_CAPACITY] = {0};
    json_t j;

    if (!e || !id || !out || capacity == 0) return -1;
    if (vm_instance_paths_derive(e->firmware, e->machines, id, &paths) !=
        VM_INSTANCE_PATHS_OK) {
        json_reset(&j, out, capacity);
        json_raw(&j, "{\"ok\":false,\"snapshots\":[],\"detail\":"
                     "\"No such machine.\"}");
        return (int)j.len;
    }
    if (!list_snapshots_for(&paths, snapshots, sizeof snapshots, infos,
                            VM_SNAPSHOT_MAX, &count, detail, sizeof detail)) {
        json_reset(&j, out, capacity);
        json_raw(&j, "{\"ok\":false,\"snapshots\":[],\"detail\":");
        json_string(&j, detail);
        json_raw(&j, "}");
        return (int)j.len;
    }

    json_reset(&j, out, capacity);
    json_raw(&j, "{\"ok\":true,\"snapshots\":[");
    for (size_t i = 0; i < count; i++) {
        char directory[ENGINE_PATH_CAP];
        if (i) json_raw(&j, ",");
        if (vm_snapshot_path(snapshots, infos[i].id, false, directory,
                             sizeof directory) != VM_SNAPSHOT_OK)
            directory[0] = '\0';
        json_raw(&j, "{\"id\":");
        json_string(&j, infos[i].id);
        json_printf(&j,
            ",\"created\":%llu,\"retired\":%llu,\"bytes\":%llu,"
            "\"complete\":%s,\"directoryBytes\":%llu}",
            (unsigned long long)infos[i].created_unix,
            (unsigned long long)infos[i].retired,
            (unsigned long long)infos[i].bytes,
            infos[i].complete ? "true" : "false",
            (unsigned long long)directory_size(directory));
    }
    json_raw(&j, "]}");
    return (int)j.len;
}

static bool create_cow_header(const vm_instance_paths_t *paths,
                              const char *cow_path,
                              char *detail, size_t detail_capacity)
{
    struct stat st;
    file_block_t *adapter = NULL;
    vm_cow_t *cow = NULL;
    bool ok = false;

    if (stat(paths->work_image, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "The machine has no writable disk yet.");
        return false;
    }
    adapter = file_block_create();
    if (!adapter) return false;
    if (file_block_open(adapter, paths->work_image, (uint64_t)st.st_size) !=
        FILE_BLOCK_STATUS_OK) {
        file_block_destroy(&adapter);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "The writable disk could not be opened.");
        return false;
    }
    if (vm_cow_open(&cow, file_block_get(adapter), cow_path,
                    detail, detail_capacity) == VM_COW_OK) {
        ok = vm_cow_close(&cow) == VM_COW_OK;
    }
    if (cow) vm_cow_close(&cow);
    file_block_close(adapter);
    file_block_destroy(&adapter);
    return ok;
}

int engine_snapshot_take(engine_t *engine, const char *id,
                         char *snapshot_id, size_t id_capacity,
                         char *detail, size_t detail_capacity)
{
    engine_t *e = engine;
    vm_instance_paths_t paths;
    vm_snapshot_info_t infos[VM_SNAPSHOT_MAX];
    size_t count = 0;
    char snapshots[ENGINE_PATH_CAP];
    char partial[ENGINE_PATH_CAP], final_path[ENGINE_PATH_CAP];
    char member[ENGINE_PATH_CAP];
    char new_id[VM_SNAPSHOT_ID_CAPACITY];
    char why[VM_FW_BOOT_DETAIL_CAPACITY] = {0};
    uint64_t now = engine_unix_now();
    uint64_t retired = 0;
    unsigned sequence;
    bool ok = false;
    vm_snapshot_info_t info;

    if (detail && detail_capacity) detail[0] = '\0';
    if (!e || !id) return -1;
    pthread_mutex_lock(&e->lock);
    if (e->phase != PHASE_IDLE) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "Stop the machine and save its checkpoint before taking "
                     "a snapshot.");
        return -1;
    }
    retired = e->retired;
    pthread_mutex_unlock(&e->lock);

    if (vm_instance_paths_derive(e->firmware, e->machines, id, &paths) !=
        VM_INSTANCE_PATHS_OK) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "No such machine.");
        return -1;
    }
    if (!file_present(paths.state, NULL) ||
        !file_present(paths.state_md, NULL)) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "There is no saved checkpoint for this machine. Stop it "
                     "with \"Save state\" first.");
        return -1;
    }
    if (!file_present(paths.work_image, NULL)) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "This machine has no writable disk to snapshot.");
        return -1;
    }
    if (!mkdir_p(paths.machine) ||
        vm_snapshot_dir(paths.machine, snapshots, sizeof snapshots) !=
            VM_SNAPSHOT_OK ||
        !mkdir_p(snapshots)) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "The snapshot directory could not be created.");
        return -1;
    }
    if (!list_snapshots_for(&paths, snapshots, sizeof snapshots, infos,
                            VM_SNAPSHOT_MAX, &count, why, sizeof why)) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "%s", why);
        return -1;
    }
    for (sequence = 0; sequence < 100; sequence++) {
        if (vm_snapshot_id_make(now, sequence, new_id, sizeof new_id) !=
            VM_SNAPSHOT_OK)
            break;
        if (snapshot_index(infos, count, new_id) < 0)
            break;
    }
    if (sequence >= 100 ||
        vm_snapshot_id_make(now, sequence, new_id, sizeof new_id) !=
            VM_SNAPSHOT_OK) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "A snapshot identifier could not be made.");
        return -1;
    }
    if (vm_snapshot_path(snapshots, new_id, true, partial, sizeof partial) !=
            VM_SNAPSHOT_OK ||
        vm_snapshot_path(snapshots, new_id, false, final_path,
                         sizeof final_path) != VM_SNAPSHOT_OK) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "The snapshot path could not be built.");
        return -1;
    }
    rm_rf(partial);
    if (!mkdir_p(partial)) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "The snapshot directory could not be created.");
        return -1;
    }

    if (vm_snapshot_member_path(snapshots, new_id, true,
                                VM_SNAPSHOT_STATE_FILE, member,
                                sizeof member) == VM_SNAPSHOT_OK &&
        copy_file(paths.state, member) &&
        vm_snapshot_member_path(snapshots, new_id, true,
                                VM_SNAPSHOT_MD_FILE, member,
                                sizeof member) == VM_SNAPSHOT_OK &&
        copy_file(paths.state_md, member) &&
        vm_snapshot_member_path(snapshots, new_id, true,
                                VM_SNAPSHOT_COW_FILE, member,
                                sizeof member) == VM_SNAPSHOT_OK &&
        create_cow_header(&paths, member, why, sizeof why)) {
        memset(&info, 0, sizeof info);
        snprintf(info.id, sizeof info.id, "%s", new_id);
        info.created_unix = now;
        info.retired = retired;
        info.bytes = directory_size(partial);
        info.complete = true;
        if (vm_snapshot_member_path(snapshots, new_id, true,
                                    VM_SNAPSHOT_META_FILE, member,
                                    sizeof member) == VM_SNAPSHOT_OK &&
            vm_snapshot_meta_write(member, &info, why, sizeof why) ==
                VM_SNAPSHOT_OK) {
            if (rename(partial, final_path) == 0) {
                ok = true;
            } else {
                snprintf(why, sizeof why,
                         "The snapshot could not be published.");
            }
        }
    }
    if (!ok) {
        rm_rf(partial);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "%s",
                     why[0] ? why : "The snapshot could not be written.");
        return -1;
    }
    if (snapshot_id && id_capacity) snprintf(snapshot_id, id_capacity, "%s",
                                             new_id);
    console_printf(e, "[vm] snapshot %s saved\n", new_id);
    return 0;
}

bool engine_snapshot_restore_needs_unlock(engine_t *engine, const char *id,
                                          const char *snapshot_id)
{
    engine_t *e = engine;
    vm_instance_paths_t paths;
    vm_snapshot_info_t infos[VM_SNAPSHOT_MAX];
    size_t count = 0;
    char snapshots[ENGINE_PATH_CAP];
    char detail[VM_FW_BOOT_DETAIL_CAPACITY] = {0};
    int index;
    if (!e || !id || !snapshot_id) return false;
    if (vm_instance_paths_derive(e->firmware, e->machines, id, &paths) !=
        VM_INSTANCE_PATHS_OK)
        return false;
    if (!list_snapshots_for(&paths, snapshots, sizeof snapshots, infos,
                            VM_SNAPSHOT_MAX, &count, detail, sizeof detail))
        return false;
    index = snapshot_index(infos, count, snapshot_id);
    return index > 0;
}

int engine_snapshot_restore(engine_t *engine, const char *id,
                            const char *snapshot_id,
                            char *detail, size_t detail_capacity)
{
    engine_t *e = engine;
    vm_instance_paths_t paths;
    vm_snapshot_info_t infos[VM_SNAPSHOT_MAX];
    size_t count = 0;
    char snapshots[ENGINE_PATH_CAP];
    char why[VM_FW_BOOT_DETAIL_CAPACITY] = {0};
    char member[ENGINE_PATH_CAP];
    int target;
    struct stat st;
    file_block_t *adapter = NULL;
    bool ok = false;
    unsigned deleted = 0;

    if (detail && detail_capacity) detail[0] = '\0';
    if (!e || !id || !snapshot_id) return -1;
    pthread_mutex_lock(&e->lock);
    if (e->phase != PHASE_IDLE) {
        pthread_mutex_unlock(&e->lock);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "Stop the machine before restoring a snapshot.");
        return -1;
    }
    pthread_mutex_unlock(&e->lock);

    if (vm_instance_paths_derive(e->firmware, e->machines, id, &paths) !=
        VM_INSTANCE_PATHS_OK) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "No such machine.");
        return -1;
    }
    if (!list_snapshots_for(&paths, snapshots, sizeof snapshots, infos,
                            VM_SNAPSHOT_MAX, &count, why, sizeof why)) {
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "%s",
                                                why);
        return -1;
    }
    target = snapshot_index(infos, count, snapshot_id);
    if (target < 0) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "That snapshot no longer exists.");
        return -1;
    }
    if (stat(paths.work_image, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "This machine has no writable disk to restore onto.");
        return -1;
    }
    adapter = file_block_create();
    if (!adapter) return -1;
    if (file_block_open(adapter, paths.work_image, (uint64_t)st.st_size) !=
        FILE_BLOCK_STATUS_OK) {
        file_block_destroy(&adapter);
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "The writable disk could not be opened.");
        return -1;
    }

    for (int i = 0; i <= target; i++) {
        if (vm_snapshot_member_path(snapshots, infos[i].id, false,
                                    VM_SNAPSHOT_COW_FILE, member,
                                    sizeof member) != VM_SNAPSHOT_OK)
            continue;
        if (stat(member, &st) != 0) continue;
        if (vm_cow_replay(member, file_block_get(adapter), NULL, NULL,
                          why, sizeof why) != VM_COW_OK) {
            goto done;
        }
    }
    /* The historical contents are only real once they have reached the media;
     * publishing the CPU state next to an unsynced disk is how a restore
     * survives a power loss as RAM at S and disk at "somewhere after S". */
    if (file_block_flush(adapter) != FILE_BLOCK_STATUS_OK) {
        snprintf(why, sizeof why,
                 "The restored disk could not be written back.");
        goto done;
    }

    if (vm_snapshot_member_path(snapshots, infos[target].id, false,
                                VM_SNAPSHOT_STATE_FILE, member,
                                sizeof member) != VM_SNAPSHOT_OK ||
        !copy_file(member, paths.state_tmp)) {
        snprintf(why, sizeof why,
                 "The snapshot's CPU state could not be installed.");
        goto done;
    }
    if (vm_snapshot_member_path(snapshots, infos[target].id, false,
                                VM_SNAPSHOT_MD_FILE, member,
                                sizeof member) != VM_SNAPSHOT_OK ||
        !copy_file(member, paths.state_md_tmp)) {
        unlink(paths.state_tmp);
        snprintf(why, sizeof why,
                 "The snapshot's bridge state could not be installed.");
        goto done;
    }
    if (rename(paths.state_tmp, paths.state) != 0 ||
        rename(paths.state_md_tmp, paths.state_md) != 0) {
        snprintf(why, sizeof why,
                 "The restored state could not be published.");
        goto done;
    }
    {
        char marker[ENGINE_PATH_CAP];
        if (!join_path(marker, sizeof marker, paths.machine,
                       VM_FW_BOOT_RESTORE_ONCE_FILE) ||
            !write_marker(marker)) {
            snprintf(why, sizeof why,
                     "The one-shot restore marker could not be written.");
            goto done;
        }
    }

    /* Everything newer than the chosen snapshot is now folded into the live
     * disk by the replay above; those history layers are no longer needed. */
    for (int i = 0; i < target; i++) {
        char directory[ENGINE_PATH_CAP];
        if (vm_snapshot_path(snapshots, infos[i].id, false, directory,
                             sizeof directory) == VM_SNAPSHOT_OK &&
            rm_rf(directory) == 0)
            deleted++;
    }
    ok = true;
done:
    file_block_close(adapter);
    file_block_destroy(&adapter);
    if (!ok) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "%s",
                     why[0] ? why : "The snapshot could not be restored.");
        return -1;
    }
    console_printf(e, "[vm] snapshot %s restored (%u newer removed)\n",
                   snapshot_id, deleted);
    if (detail && detail_capacity)
        snprintf(detail, detail_capacity,
                 "Restored. %u newer snapshot%s removed.", deleted,
                 deleted == 1 ? "" : "s");
    return 0;
}

int engine_snapshot_delete(engine_t *engine, const char *id,
                           const char *snapshot_id,
                           char *detail, size_t detail_capacity)
{
    engine_t *e = engine;
    vm_instance_paths_t paths;
    char snapshots[ENGINE_PATH_CAP];
    char directory[ENGINE_PATH_CAP];

    if (detail && detail_capacity) detail[0] = '\0';
    if (!e || !id || !snapshot_id) return -1;
    if (vm_instance_paths_derive(e->firmware, e->machines, id, &paths) !=
        VM_INSTANCE_PATHS_OK) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "No such machine.");
        return -1;
    }
    if (vm_snapshot_dir(paths.machine, snapshots, sizeof snapshots) !=
            VM_SNAPSHOT_OK ||
        vm_snapshot_path(snapshots, snapshot_id, false, directory,
                         sizeof directory) != VM_SNAPSHOT_OK) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "That snapshot's name is not one this app wrote.");
        return -1;
    }
    if (!is_directory(directory)) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity, "That snapshot no longer exists.");
        return -1;
    }
    if (rm_rf(directory) != 0) {
        if (detail && detail_capacity)
            snprintf(detail, detail_capacity,
                     "The snapshot's files could not be removed.");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ audio --- */

int engine_render_audio(engine_t *engine, float *interleaved, unsigned frames)
{
    engine_t *e = engine;
    unsigned done = 0;
    if (!e || !interleaved || !e->audio_ok ||
        !atomic_load_explicit(&e->audio.host_running, memory_order_acquire))
        return 0;
    while (frames > 0) {
        unsigned chunk = frames > ENGINE_AUDIO_SCRATCH
                             ? ENGINE_AUDIO_SCRATCH : frames;
        vm_audio_render(&e->audio, e->audio_left, e->audio_right, chunk);
        for (unsigned i = 0; i < chunk; i++) {
            interleaved[2u * i] = e->audio_left[i];
            interleaved[2u * i + 1u] = e->audio_right[i];
        }
        interleaved += 2u * chunk;
        frames -= chunk;
        done += chunk;
    }
    return (int)done;
}

void engine_set_microphone(engine_t *engine, bool enabled)
{
    if (!engine) return;
    atomic_store(&engine->microphone_on, enabled);
    atomic_store(&engine->audio.microphone_enabled, enabled);
}

/* -------------------------------------------------------------------- USB --- */

bool engine_usb_ready(engine_t *engine)
{
    bool ready;
    if (!engine) return false;
    pthread_mutex_lock(&engine->lock);
    ready = engine->usb_ready;
    pthread_mutex_unlock(&engine->lock);
    return ready;
}

int engine_usb_open(engine_t *engine, const char *service,
                    char *detail, size_t detail_capacity)
{
    (void)engine;
    (void)service;
    if (detail && detail_capacity)
        snprintf(detail, detail_capacity,
                 "Virtual USB services are not connected in this build yet.");
    return -1;
}
