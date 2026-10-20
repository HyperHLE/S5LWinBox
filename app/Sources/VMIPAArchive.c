/* Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "VMIPAArchive.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char *out, size_t cap, const char *message) {
    if (out && cap) (void)snprintf(out, cap, "%s", message);
    return false;
}

bool vm_ipa_safe_path(const char *path) {
    if (!path || !*path || *path == '/') return false;
    const char *part = path;
    for (const char *p = path;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c && (c < 32 || c > 126 || c == '\\' || c == ':')) return false;
        if (c == '/' || c == 0) {
            size_t n = (size_t)(p - part);
            if (!n || (n == 1 && part[0] == '.') ||
                (n == 2 && part[0] == '.' && part[1] == '.')) return false;
            if (!c || !p[1]) return true;
            part = p + 1;
        }
    }
}

typedef struct {
    vm_ipa_archive_t *plan;
    const char *error;
} walk_t;

bool vm_ipa_entry_is_symlink(const vmfw_zip_entry_t *entry) {
    return entry && (entry->unix_mode & 0170000u) == 0120000u;
}

static unsigned char path_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static bool path_prefix(const char *a, const char *b, size_t length) {
    for (size_t i = 0; i < length; i++)
        if (!b[i] || path_lower((unsigned char)a[i]) != path_lower((unsigned char)b[i])) return false;
    return true;
}

static bool visit(void *opaque, const vmfw_zip_entry_t *entry, uint32_t index) {
    (void)index;
    walk_t *walk = opaque;
    vm_ipa_archive_t *p = walk->plan;
    if (!vm_ipa_safe_path(entry->name)) {
        walk->error = "The IPA contains an unsafe or unsupported filename.";
        return false;
    }
    if (strncmp(entry->name, "Payload/", 8) || !entry->name[8]) return true;
    const char *slash = strchr(entry->name + 8, '/');
    size_t root_len = slash ? (size_t)(slash - entry->name) : strlen(entry->name);
    if (root_len < 13 || strncmp(entry->name + root_len - 4, ".app", 4)) {
        walk->error = "Payload must contain exactly one .app bundle.";
        return false;
    }
    if (!p->app_root[0]) {
        memcpy(p->app_root, entry->name, root_len);
        p->app_root[root_len] = 0;
    } else if (strlen(p->app_root) != root_len ||
               strncmp(p->app_root, entry->name, root_len)) {
        walk->error = "An IPA containing multiple apps is not supported.";
        return false;
    }
    unsigned type = entry->unix_mode & 0170000u;
    if ((type && type != 0100000u && type != 0040000u && type != 0120000u) ||
        (type == 0040000u && !entry->is_directory) ||
        ((type == 0100000u || type == 0120000u) && entry->is_directory)) {
        walk->error = "The IPA contains a special file or inconsistent file type.";
        return false;
    }
    if ((!slash || !slash[1]) && !entry->is_directory) {
        walk->error = "The app bundle is not a directory.";
        return false;
    }
    if (p->count == VM_IPA_MAX_FILES ||
        entry->uncompressed_size > VM_IPA_MAX_FILE_BYTES ||
        entry->uncompressed_size > VM_IPA_MAX_BYTES - p->unpacked_bytes ||
        (entry->is_directory && entry->uncompressed_size)) {
        walk->error = "The IPA exceeds the supported size or file-count limit.";
        return false;
    }
    for (unsigned i = 0; i < p->count; i++) {
        const char *a = p->files[i].name, *b = entry->name;
        size_t al = strlen(a), bl = strlen(b);
        if (al && a[al - 1] == '/') al--;
        if (bl && b[bl - 1] == '/') bl--;
        bool same = al == bl;
        for (size_t n = 0; same && n < al; n++) {
            unsigned char ac = (unsigned char)a[n], bc = (unsigned char)b[n];
            if (ac >= 'A' && ac <= 'Z') ac += 'a' - 'A';
            if (bc >= 'A' && bc <= 'Z') bc += 'a' - 'A';
            same = ac == bc;
        }
        if (same) {
            walk->error = "The IPA contains conflicting or duplicate paths.";
            return false;
        }
    }
    if (slash && !strcmp(slash + 1, "Info.plist") && !entry->is_directory)
        p->info_index = p->count;
    p->files[p->count++] = *entry;
    p->unpacked_bytes += entry->uncompressed_size;
    return true;
}

static bool validate_links(const vmfw_zip_t *zip, vm_ipa_archive_t *p,
                           char *detail, size_t cap) {
    /* A link (or regular file) must never be used as an extraction directory,
     * regardless of ZIP ordering or case differences on the guest filesystem. */
    for (unsigned i = 0; i < p->count; i++) {
        if (p->files[i].is_directory) continue;
        size_t length = strlen(p->files[i].name);
        for (unsigned j = 0; j < p->count; j++) {
            if (i != j && path_prefix(p->files[i].name, p->files[j].name, length) &&
                p->files[j].name[length] == '/')
                return fail(detail, cap, "The IPA uses a file or symbolic link as a parent directory.");
        }
    }
    p->content_indices = calloc(p->count, sizeof *p->content_indices);
    if (!p->content_indices) return fail(detail, cap, "Not enough memory to inspect IPA links.");
    for (unsigned i = 0; i < p->count; i++) p->content_indices[i] = i;
    for (unsigned i = 0; i < p->count; i++) {
        const vmfw_zip_entry_t *entry = &p->files[i];
        if (!vm_ipa_entry_is_symlink(entry)) continue;
        char target[VMFW_ZIP_MAX_NAME], path[VMFW_ZIP_MAX_NAME];
        if (!entry->uncompressed_size || entry->uncompressed_size >= sizeof target)
            return fail(detail, cap, "The IPA contains an empty or oversized symbolic link.");
        vmfw_mem_sink_t sink;
        vmfw_mem_sink_init(&sink, (uint8_t *)target, sizeof target - 1);
        vmfw_zip_status_t status = vmfw_zip_extract(zip, entry, vmfw_mem_sink_write, &sink);
        if (status != VMFW_ZIP_OK) return fail(detail, cap, vmfw_zip_strerror(status));
        target[sink.len] = 0;
        if (sink.len != entry->uncompressed_size || strlen(target) != sink.len ||
            !vm_ipa_safe_path(target) || target[sink.len - 1] == '/')
            return fail(detail, cap, "IPA links must use relative file paths inside the app, without dot components.");
        const char *slash = strrchr(entry->name, '/');
        size_t parent = slash ? (size_t)(slash - entry->name + 1) : 0;
        if (parent <= strlen(p->app_root) || parent + sink.len >= sizeof path)
            return fail(detail, cap, "The IPA contains an invalid symbolic link path.");
        memcpy(path, entry->name, parent);
        memcpy(path + parent, target, sink.len + 1);
        unsigned destination = p->count;
        for (unsigned j = 0; j < p->count; j++) {
            /* Require the exact stored spelling; never guess a target using
             * host filesystem case or Unicode normalization rules. */
            if (!strcmp(path, p->files[j].name)) { destination = j; break; }
        }
        if (destination == p->count || p->files[destination].is_directory)
            return fail(detail, cap, "The IPA contains a missing target or unsupported directory link.");
        p->content_indices[i] = destination;
    }
    /* Bound chain traversal without recursion. Do not collapse this map until
     * all chains pass, so order cannot hide an overlong chain or a cycle. */
    for (unsigned i = 0; i < p->count; i++) {
        unsigned current = i, hops = 0;
        while (vm_ipa_entry_is_symlink(&p->files[current])) {
            if (++hops > 16)
                return fail(detail, cap, "The IPA contains a symbolic link cycle or a chain longer than 16 links.");
            current = p->content_indices[current];
        }
    }
    for (unsigned i = 0; i < p->count; i++) {
        unsigned current = i;
        while (vm_ipa_entry_is_symlink(&p->files[current])) current = p->content_indices[current];
        p->content_indices[i] = current;
    }
    return true;
}

bool vm_ipa_archive_open(const vmfw_zip_t *zip, vm_ipa_archive_t *p,
                         char *detail, size_t cap) {
    if (!p) return fail(detail, cap, "No IPA inspection result was supplied.");
    memset(p, 0, sizeof *p);
    p->info_index = UINT32_MAX;
    p->files = calloc(VM_IPA_MAX_FILES, sizeof *p->files);
    if (!p->files) return fail(detail, cap, "Not enough memory to inspect this IPA.");
    walk_t walk = {p, NULL};
    vmfw_zip_status_t status = vmfw_zip_iterate(zip, visit, &walk);
    if (status != VMFW_ZIP_OK || walk.error || p->info_index == UINT32_MAX) {
        const char *message = walk.error ? walk.error : status != VMFW_ZIP_OK
            ? vmfw_zip_strerror(status) : "No Payload/*.app/Info.plist was found.";
        fail(detail, cap, message);
        vm_ipa_archive_close(p);
        return false;
    }
    if (!validate_links(zip, p, detail, cap)) {
        vm_ipa_archive_close(p);
        return false;
    }
    if (detail && cap) detail[0] = 0;
    return true;
}

void vm_ipa_archive_close(vm_ipa_archive_t *p) {
    if (!p) return;
    free(p->files);
    free(p->content_indices);
    memset(p, 0, sizeof *p);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}
bool vm_ipa_icon_png_dimensions(const uint8_t *bytes, size_t size,
                                uint32_t *width, uint32_t *height) {
    static const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!bytes || size < 33 || size > 2u * 1024u * 1024u ||
        memcmp(bytes, signature, sizeof signature)) return false;
    size_t offset = 8;
    if (!memcmp(bytes + offset + 4, "CgBI", 4)) {
        uint32_t length = be32(bytes + offset);
        if (length > 64 || (size_t)length + 12 > size - offset) return false;
        offset += (size_t)length + 12;
    }
    if (size - offset < 25 || be32(bytes + offset) != 13 ||
        memcmp(bytes + offset + 4, "IHDR", 4)) return false;
    uint32_t w = be32(bytes + offset + 8), h = be32(bytes + offset + 12);
    if (!w || !h || w > 2048 || h > 2048 || bytes[offset + 18] != 0 ||
        bytes[offset + 19] != 0 || bytes[offset + 20] > 1) return false;
    if (width) *width = w;
    if (height) *height = h;
    return true;
}
static bool armv6(uint32_t subtype) {
    subtype &= 0xffffffu;
    return subtype == 0 || subtype == 5 || subtype == 6 || subtype == 7;
}
static bool thin(const uint8_t *b, size_t n, char *out, size_t cap) {
    if (n < 28 || le32(b) != 0xfeedface || le32(b + 4) != 12 ||
        !armv6(le32(b + 8)) || le32(b + 12) != 2)
        return fail(out, cap, "This app needs an ARMv6 iPhone executable; ARMv7-only and ARM64 apps cannot run here.");
    uint32_t commands = le32(b + 16), bytes = le32(b + 20);
    if (bytes > n - 28 || commands > bytes / 8)
        return fail(out, cap, "The app executable has invalid load commands.");
    size_t cursor = 28, end = 28 + bytes;
    for (uint32_t i = 0; i < commands; i++) {
        if (end - cursor < 8) return fail(out, cap, "The app executable is truncated.");
        uint32_t cmd = le32(b + cursor), size = le32(b + cursor + 4);
        if (size < 8 || size % 4 || size > end - cursor)
            return fail(out, cap, "The app executable has an invalid command length.");
        if (cmd == 0x21 || cmd == 0x2c) {
            if (size < 20 || le32(b + cursor + 16))
                return fail(out, cap, "This app is encrypted. S5LBox does not decrypt protected apps.");
        }
        if (cmd == 0x25 && (size < 16 || le32(b + cursor + 8) > 0x00030103))
            return fail(out, cap, "This app requires a newer iOS version than 3.1.3.");
        if (cmd == 0x32 || cmd == 0x24 || cmd == 0x2f || cmd == 0x30)
            return fail(out, cap, "This executable targets an unsupported Apple platform.");
        cursor += size;
    }
    if (cursor != end) return fail(out, cap, "The executable command table is inconsistent.");
    return true;
}
bool vm_ipa_check_executable(const uint8_t *b, size_t n, char *out, size_t cap) {
    if (!b || n < 4) return fail(out, cap, "The app executable is missing or truncated.");
    if (be32(b) != 0xcafebabe) return thin(b, n, out, cap);
    if (n < 8) return fail(out, cap, "The universal executable is truncated.");
    uint32_t count = be32(b + 4);
    if (!count || count > 128 || count > (n - 8) / 20)
        return fail(out, cap, "The universal executable has an invalid slice table.");
    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *a = b + 8 + i * 20;
        uint32_t offset = be32(a + 8), size = be32(a + 12);
        if (offset < 8 + count * 20 || offset > n || size > n - offset)
            return fail(out, cap, "The universal executable has an out-of-bounds slice.");
        for (uint32_t j = 0; j < i; j++) {
            const uint8_t *previous = b + 8 + j * 20;
            uint32_t start = be32(previous + 8), length = be32(previous + 12);
            if ((uint64_t)offset < (uint64_t)start + length &&
                (uint64_t)start < (uint64_t)offset + size)
                return fail(out, cap, "The universal executable has overlapping slices.");
        }
        if (be32(a) == 12 && armv6(be32(a + 4))) {
            if (!thin(b + offset, size, out, cap)) return false;
            if (be32(a + 4) != le32(b + offset + 8))
                return fail(out, cap, "The executable slice does not match its architecture table.");
            found = true;
        }
    }
    if (found) return true;
    return fail(out, cap, "This IPA has no ARMv6-compatible executable.");
}
