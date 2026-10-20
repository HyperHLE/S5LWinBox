/*
 * S5LWinBox — cross-platform compatibility layer (Windows / POSIX).
 * MIT licensed.
 */
#ifndef S5L_PLATFORM_COMPAT_H
#define S5L_PLATFORM_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#include <io.h>
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <BaseTsd.h>

#if !defined(_SSIZE_T_DEFINED) && !defined(ssize_t)
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif

#define mkdir(path, mode) _mkdir(path)
#define strdup _strdup
#define fileno _fileno
#define unlink _unlink
#define fsync(fd) _commit(fd)
#define usleep(us) Sleep((DWORD)(((us) + 999) / 1000))

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifndef O_CLOEXEC
#if defined(_O_NOINHERIT)
#define O_CLOEXEC _O_NOINHERIT
#else
#define O_CLOEXEC 0
#endif
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif

/* Pthread shim using Win32 SRWLOCK (static initialization compatible) */
typedef SRWLOCK pthread_mutex_t;
typedef void *pthread_mutexattr_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

static inline int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    (void)a;
    InitializeSRWLock(m);
    return 0;
}
static inline int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m;
    return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t *m) {
    AcquireSRWLockExclusive(m);
    return 0;
}
static inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    ReleaseSRWLockExclusive(m);
    return 0;
}

typedef struct s5l_thread_shim {
    HANDLE handle;
    void *(*func)(void *);
    void *arg;
    void *ret;
} s5l_thread_shim_t;

typedef s5l_thread_shim_t *pthread_t;
typedef void *pthread_attr_t;

static inline unsigned __stdcall _s5l_thread_shim_proc(void *p) {
    s5l_thread_shim_t *t = (s5l_thread_shim_t *)p;
    t->ret = t->func(t->arg);
    return 0;
}

static inline int pthread_create(pthread_t *th, const pthread_attr_t *a, void *(*func)(void *), void *arg) {
    (void)a;
    s5l_thread_shim_t *t = (s5l_thread_shim_t *)malloc(sizeof(s5l_thread_shim_t));
    if (!t) return -1;
    t->func = func;
    t->arg = arg;
    t->ret = NULL;
    t->handle = (HANDLE)_beginthreadex(NULL, 0, _s5l_thread_shim_proc, t, 0, NULL);
    if (!t->handle) {
        free(t);
        return -1;
    }
    *th = t;
    return 0;
}

static inline int pthread_join(pthread_t th, void **retval) {
    if (!th) return -1;
    WaitForSingleObject(th->handle, INFINITE);
    CloseHandle(th->handle);
    if (retval) *retval = th->ret;
    free(th);
    return 0;
}

/* Portable pread */
static inline ptrdiff_t portable_pread(int fd, void *buf, size_t count, int64_t offset) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        __int64 pos = _lseeki64(fd, (__int64)offset, 0);
        if (pos < 0) return -1;
        return (ptrdiff_t)_read(fd, buf, (unsigned int)count);
    }
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)((offset >> 32) & 0xFFFFFFFF);
    DWORD read_bytes = 0;
    if (!ReadFile(h, buf, (DWORD)count, &read_bytes, &ov)) {
        DWORD err = GetLastError();
        if (err == ERROR_HANDLE_EOF) return 0;
        return -1;
    }
    return (ptrdiff_t)read_bytes;
}

/* Dynamic library loading */
typedef HMODULE s5l_dynlib_t;
#define S5L_DYNLIB_OPEN(path) LoadLibraryA(path)
#define S5L_DYNLIB_SYM(handle, sym) (void *)GetProcAddress((HMODULE)(handle), sym)
#define S5L_DYNLIB_CLOSE(handle) FreeLibrary((HMODULE)(handle))

/* Directory cleanup */
static inline int portable_rm_rf(const char *path) {
    if (!path || !path[0]) return 0;
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return 0;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        char search_path[MAX_PATH];
        snprintf(search_path, sizeof(search_path), "%s\\*.*", path);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(search_path, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
                char child[MAX_PATH];
                snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
                portable_rm_rf(child);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryA(path);
    } else {
        DeleteFileA(path);
    }
    return 0;
}

#else

#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <dlfcn.h>

#define portable_pread pread

typedef void *s5l_dynlib_t;
#define S5L_DYNLIB_OPEN(path) dlopen(path, RTLD_NOW | RTLD_LOCAL)
#define S5L_DYNLIB_SYM(handle, sym) dlsym(handle, sym)
#define S5L_DYNLIB_CLOSE(handle) dlclose(handle)

static inline int portable_rm_rf(const char *path) {
    struct stat st;
    if (!path || lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *ent;
        if (!d) return -1;
        while ((ent = readdir(d)) != NULL) {
            char child[1024];
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            portable_rm_rf(child);
        }
        closedir(d);
        return rmdir(path);
    }
    return unlink(path);
}

#endif

#endif /* S5L_PLATFORM_COMPAT_H */
