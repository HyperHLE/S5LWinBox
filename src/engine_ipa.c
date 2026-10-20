/* S5LAndBox — untrusted guest IPA inspection and installation. MIT licensed. */
#include "engine_ipa.h"

#include "VMIPAArchive.h"
#include "VMGuest.h"
#include "VMGuestInstall.h"
#include "VMResumeCheckpoint.h"
#include "VMSnapshotStore.h"
#include "VMFirmwareFormats.h"
#include "rootfs_work.h"
#include "sha256.h"
#include "platform_compat.h"
#include "web_log_server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef struct {
    int fd;
} ipa_file_reader_t;

static size_t ipa_pread(void *ctx, uint64_t offset, uint8_t *bytes, size_t length) {
    int fd = ((ipa_file_reader_t *)ctx)->fd;
    ptrdiff_t got = portable_pread(fd, bytes, length, (int64_t)offset);
    return got < 0 ? 0 : (size_t)got;
}

static bool ipa_sink_file(void *ctx, const uint8_t *bytes, size_t size) {
    return fwrite(bytes, 1, size, (FILE *)ctx) == size;
}

static uint64_t bplist_read_uint(const uint8_t *p, unsigned size) {
    uint64_t val = 0;
    for (unsigned i = 0; i < size; i++) val = (val << 8) | p[i];
    return val;
}

/* Parse a binary property list (bplist00) Info.plist */
static bool parse_bplist(const uint8_t *bytes, size_t len, engine_ipa_info_t *info, bool *out_armv7) {
    if (len < 32 || memcmp(bytes, "bplist00", 8) != 0) return false;
    const uint8_t *trailer = bytes + len - 32;
    uint8_t offset_int_size = trailer[6];
    uint8_t object_ref_size = trailer[7];
    uint64_t num_objects = bplist_read_uint(trailer + 8, 8);
    uint64_t top_object = bplist_read_uint(trailer + 16, 8);
    uint64_t table_offset = bplist_read_uint(trailer + 24, 8);

    if (offset_int_size == 0 || offset_int_size > 8 ||
        object_ref_size == 0 || object_ref_size > 8 ||
        num_objects == 0 || top_object >= num_objects)
        return false;
    if (table_offset > len || num_objects > (len - table_offset) / offset_int_size)
        return false;

    #define GET_OFFSET(idx) (bplist_read_uint(bytes + table_offset + (idx) * offset_int_size, offset_int_size))
    #define READ_REF(p) (bplist_read_uint((p), object_ref_size))

    uint64_t root_off = GET_OFFSET(top_object);
    if (root_off >= len) return false;

    uint8_t tag = bytes[root_off];
    if ((tag & 0xF0) != 0xD0) return false;

    size_t cursor = root_off + 1;
    uint64_t count = tag & 0x0F;
    if (count == 0x0F) {
        if (cursor >= len) return false;
        uint8_t int_tag = bytes[cursor++];
        if ((int_tag & 0xF0) != 0x10) return false;
        unsigned int_len = 1u << (int_tag & 0x0F);
        if (int_len > 8 || cursor + int_len > len) return false;
        count = bplist_read_uint(bytes + cursor, int_len);
        cursor += int_len;
    }

    uint64_t keys_start = cursor;
    uint64_t vals_start = keys_start + count * object_ref_size;
    if (vals_start + count * object_ref_size > len) return false;

    for (uint64_t k = 0; k < count; k++) {
        uint64_t key_ref = READ_REF(bytes + keys_start + k * object_ref_size);
        uint64_t val_ref = READ_REF(bytes + vals_start + k * object_ref_size);
        if (key_ref >= num_objects || val_ref >= num_objects) continue;

        uint64_t key_off = GET_OFFSET(key_ref);
        if (key_off >= len) continue;

        char key_str[128] = {0};
        uint8_t k_tag = bytes[key_off];
        size_t k_cur = key_off + 1;
        uint64_t k_len = k_tag & 0x0F;
        if (k_len == 0x0F && k_cur < len) {
            uint8_t itag = bytes[k_cur++];
            unsigned ilen = 1u << (itag & 0x0F);
            if (ilen <= 8 && k_cur + ilen <= len) {
                k_len = bplist_read_uint(bytes + k_cur, ilen);
                k_cur += ilen;
            }
        }
        if ((k_tag & 0xF0) == 0x50 && k_cur + k_len <= len) {
            size_t copy_n = k_len < sizeof(key_str) - 1 ? k_len : sizeof(key_str) - 1;
            memcpy(key_str, bytes + k_cur, copy_n);
            key_str[copy_n] = 0;
        }

        if (!key_str[0]) continue;

        uint64_t val_off = GET_OFFSET(val_ref);
        if (val_off >= len) continue;
        uint8_t v_tag = bytes[val_off];

        char val_str[256] = {0};
        size_t v_cur = val_off + 1;
        uint64_t v_len = v_tag & 0x0F;
        if (v_len == 0x0F && v_cur < len) {
            uint8_t vitag = bytes[v_cur++];
            unsigned vilen = 1u << (vitag & 0x0F);
            if (vilen <= 8 && v_cur + vilen <= len) {
                v_len = bplist_read_uint(bytes + v_cur, vilen);
                v_cur += vilen;
            }
        }
        if ((v_tag & 0xF0) == 0x50 && v_cur + v_len <= len) {
            size_t copy_n = v_len < sizeof(val_str) - 1 ? v_len : sizeof(val_str) - 1;
            memcpy(val_str, bytes + v_cur, copy_n);
            val_str[copy_n] = 0;
        } else if ((v_tag & 0xF0) == 0x60 && v_cur + v_len * 2 <= len) {
            size_t out_idx = 0;
            for (uint64_t u = 0; u < v_len && out_idx < sizeof(val_str) - 1; u++) {
                uint16_t ch = (uint16_t)((bytes[v_cur + u * 2] << 8) | bytes[v_cur + u * 2 + 1]);
                val_str[out_idx++] = ch < 128 ? (char)ch : '?';
            }
            val_str[out_idx] = 0;
        }

        if (!strcmp(key_str, "CFBundleIdentifier")) {
            snprintf(info->bundle_id, sizeof info->bundle_id, "%s", val_str);
        } else if (!strcmp(key_str, "CFBundleExecutable")) {
            snprintf(info->executable, sizeof info->executable, "%s", val_str);
        } else if (!strcmp(key_str, "CFBundleDisplayName")) {
            snprintf(info->display_name, sizeof info->display_name, "%s", val_str);
        } else if (!strcmp(key_str, "CFBundleName") && !info->display_name[0]) {
            snprintf(info->display_name, sizeof info->display_name, "%s", val_str);
        } else if (!strcmp(key_str, "CFBundleVersion") && !info->version[0]) {
            snprintf(info->version, sizeof info->version, "%s", val_str);
        } else if (!strcmp(key_str, "CFBundleShortVersionString")) {
            snprintf(info->version, sizeof info->version, "%s", val_str);
        } else if (!strcmp(key_str, "MinimumOSVersion")) {
            snprintf(info->min_os, sizeof info->min_os, "%s", val_str);
        } else if (!strcmp(key_str, "UIRequiredDeviceCapabilities")) {
            if ((v_tag & 0xF0) == 0xA0) {
                for (uint64_t a = 0; a < v_len; a++) {
                    uint64_t aref = READ_REF(bytes + v_cur + a * object_ref_size);
                    if (aref < num_objects) {
                        uint64_t aoff = GET_OFFSET(aref);
                        if (aoff < len && (bytes[aoff] & 0xF0) == 0x50 && (bytes[aoff] & 0x0F) == 5) {
                            if (aoff + 6 <= len && !memcmp(bytes + aoff + 1, "armv7", 5)) {
                                if (out_armv7) *out_armv7 = true;
                            }
                        }
                    }
                }
            }
        }
    }
    #undef GET_OFFSET
    #undef READ_REF
    return true;
}

/* Parse an XML Info.plist using vmfw_plist_t */
static bool parse_xml_plist(const uint8_t *bytes, size_t len, engine_ipa_info_t *info) {
    vmfw_plist_t pl;
    if (vmfw_plist_init(&pl, bytes, len) != VMFW_PLIST_OK) return false;

    vmfw_plist_get_string(&pl, "CFBundleIdentifier", info->bundle_id, sizeof info->bundle_id);
    vmfw_plist_get_string(&pl, "CFBundleExecutable", info->executable, sizeof info->executable);
    vmfw_plist_get_string(&pl, "CFBundleDisplayName", info->display_name, sizeof info->display_name);
    if (!info->display_name[0]) {
        vmfw_plist_get_string(&pl, "CFBundleName", info->display_name, sizeof info->display_name);
    }
    vmfw_plist_get_string(&pl, "CFBundleVersion", info->version, sizeof info->version);
    if (!info->version[0]) {
        vmfw_plist_get_string(&pl, "CFBundleShortVersionString", info->version, sizeof info->version);
    }
    vmfw_plist_get_string(&pl, "MinimumOSVersion", info->min_os, sizeof info->min_os);
    return info->bundle_id[0] && info->executable[0];
}

static bool parse_info_plist(const uint8_t *bytes, size_t len, engine_ipa_info_t *info, bool *out_armv7) {
    if (len >= 8 && memcmp(bytes, "bplist00", 8) == 0) {
        return parse_bplist(bytes, len, info, out_armv7);
    }
    return parse_xml_plist(bytes, len, info);
}

static bool ipa_version_supported(const char *version) {
    if (!version || !*version) return true;
    unsigned result = 0;
    const char *p = version;
    for (int part = 0; part < 3; part++) {
        unsigned val = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (unsigned)(*p - '0');
            p++;
        }
        if (val > 255) return false;
        result = (result << 8) | val;
        if (*p == '.') p++;
        else if (*p == 0) {
            while (++part < 3) result <<= 8;
            break;
        } else return false;
    }
    return result <= 0x030103u;
}

static bool is_valid_bundle_id(const char *id) {
    if (!id || !*id || strlen(id) > 200 || strstr(id, "com.apple.") == id) return false;
    for (const char *p = id; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return false;
    }
    return true;
}

bool engine_ipa_inspect_path(const char *ipa_path,
                             engine_ipa_info_t *out_info,
                             char *detail, size_t detail_capacity)
{
    if (detail && detail_capacity) detail[0] = 0;
    if (!ipa_path || !out_info) {
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Invalid parameters.");
        return false;
    }
    memset(out_info, 0, sizeof *out_info);

    int fd = open(ipa_path, O_RDONLY);
    if (fd < 0) {
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Could not open IPA file: %s", strerror(errno));
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > 512u * 1024u * 1024u) {
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "IPA file must be regular and <= 512 MB.");
        return false;
    }

    ipa_file_reader_t reader = { .fd = fd };
    vmfw_zip_t zip;
    vmfw_zip_status_t zst = vmfw_zip_open(&zip, ipa_pread, &reader, (uint64_t)st.st_size);
    if (zst != VMFW_ZIP_OK) {
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "%s", vmfw_zip_strerror(zst));
        return false;
    }

    vm_ipa_archive_t plan;
    char subdetail[320] = {0};
    if (!vm_ipa_archive_open(&zip, &plan, subdetail, sizeof subdetail)) {
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "%s", subdetail);
        return false;
    }

    unsigned info_idx = plan.content_indices[plan.info_index];
    size_t info_size = (size_t)plan.files[info_idx].uncompressed_size;
    if (info_size > 1024u * 1024u) {
        vm_ipa_archive_close(&plan);
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Info.plist is too large (> 1 MB).");
        return false;
    }

    uint8_t *info_bytes = malloc(info_size);
    if (!info_bytes) {
        vm_ipa_archive_close(&plan);
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Out of memory extracting Info.plist.");
        return false;
    }

    vmfw_mem_sink_t sink;
    vmfw_mem_sink_init(&sink, info_bytes, info_size);
    zst = vmfw_zip_extract(&zip, &plan.files[info_idx], vmfw_mem_sink_write, &sink);
    if (zst != VMFW_ZIP_OK) {
        free(info_bytes);
        vm_ipa_archive_close(&plan);
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Failed extracting Info.plist: %s", vmfw_zip_strerror(zst));
        return false;
    }

    bool armv7_required = false;
    bool parsed = parse_info_plist(info_bytes, info_size, out_info, &armv7_required);
    free(info_bytes);

    if (!parsed || !out_info->bundle_id[0] || !out_info->executable[0]) {
        vm_ipa_archive_close(&plan);
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Could not parse Info.plist identifier or executable.");
        return false;
    }

    if (!is_valid_bundle_id(out_info->bundle_id)) {
        vm_ipa_archive_close(&plan);
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Unsupported or reserved bundle identifier: %s", out_info->bundle_id);
        return false;
    }

    if (!vm_ipa_safe_path(out_info->executable) || strchr(out_info->executable, '/')) {
        vm_ipa_archive_close(&plan);
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Unsafe executable name: %s", out_info->executable);
        return false;
    }

    if (armv7_required) {
        vm_ipa_archive_close(&plan);
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "App requires ARMv7. The guest emulates an ARMv6 iPhone 3G.");
        return false;
    }

    if (!ipa_version_supported(out_info->min_os)) {
        vm_ipa_archive_close(&plan);
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "App requires iOS %s (> 3.1.3).", out_info->min_os);
        return false;
    }

    /* Check executable Mach-O slice */
    char main_path[512];
    snprintf(main_path, sizeof main_path, "%s/%s", plan.app_root, out_info->executable);
    unsigned exec_idx = UINT32_MAX;
    for (unsigned i = 0; i < plan.count; i++) {
        if (!strcmp(plan.files[i].name, main_path)) {
            exec_idx = plan.content_indices[i];
            break;
        }
    }
    if (exec_idx == UINT32_MAX) {
        vm_ipa_archive_close(&plan);
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "The executable '%s' is missing from the bundle.", out_info->executable);
        return false;
    }

    size_t exec_size = (size_t)plan.files[exec_idx].uncompressed_size;
    uint8_t *exec_bytes = malloc(exec_size);
    if (!exec_bytes) {
        vm_ipa_archive_close(&plan);
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Out of memory loading executable.");
        return false;
    }

    vmfw_mem_sink_init(&sink, exec_bytes, exec_size);
    zst = vmfw_zip_extract(&zip, &plan.files[exec_idx], vmfw_mem_sink_write, &sink);
    if (zst != VMFW_ZIP_OK) {
        free(exec_bytes);
        vm_ipa_archive_close(&plan);
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Failed extracting executable: %s", vmfw_zip_strerror(zst));
        return false;
    }

    if (!vm_ipa_check_executable(exec_bytes, exec_size, subdetail, sizeof subdetail)) {
        free(exec_bytes);
        vm_ipa_archive_close(&plan);
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "%s", subdetail);
        return false;
    }
    free(exec_bytes);

    if (!out_info->display_name[0]) {
        snprintf(out_info->display_name, sizeof out_info->display_name, "%s", out_info->bundle_id);
    }
    snprintf(out_info->app_root, sizeof out_info->app_root, "%s", plan.app_root);
    out_info->file_count = plan.count;
    out_info->uncompressed_bytes = plan.unpacked_bytes;

    vm_ipa_archive_close(&plan);
    close(fd);
    return true;
}

static int entry_cmp(const void *a, const void *b) {
    const rootfs_work_entry_t *ea = a;
    const rootfs_work_entry_t *eb = b;
    return strcmp(ea->path, eb->path);
}

int engine_ipa_install_path(const char *machine_directory,
                            const char *cache_directory,
                            const char *ipa_path,
                            char *detail, size_t detail_capacity)
{
    if (detail && detail_capacity) detail[0] = 0;
    if (!machine_directory || !cache_directory || !ipa_path) {
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Invalid parameters.");
        return -1;
    }

    /* 1. Pre-flight checks on guest machine state */
    uint8_t jb_id[32];
    if (vm_guest_install_probe(machine_directory, jb_id, detail, detail_capacity) != VM_GUEST_INSTALL_PROBE_VALID) {
        if (detail && detail_capacity && !detail[0]) {
            snprintf(detail, detail_capacity, "Jailbreak (Cydia) must be installed first before installing IPAs.");
        }
        return -1;
    }

    if (vm_guest_maintenance_recover(machine_directory, NULL, NULL, NULL, detail, detail_capacity) != VM_GUEST_INSTALL_OK) {
        return -1;
    }

    char live_img[VM_GUEST_INSTALL_PATH_CAPACITY];
    snprintf(live_img, sizeof live_img, "%s/%s", machine_directory, VM_GUEST_INSTALL_LIVE_FILE);
    struct stat live_st;
    if (stat(live_img, &live_st) != 0 || live_st.st_size <= 0) {
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Machine rootfs-work.img not found. Boot it once first.");
        return -1;
    }

    vm_resume_checkpoint_state_t cs = vm_resume_checkpoint_probe_state(
        machine_directory, (uint64_t)live_st.st_size,
        VM_GUEST_RAM_BASE, VM_GUEST_RAM_SIZE, detail, detail_capacity);
    if (cs == VM_RESUME_CHECKPOINT_RUNNING || cs == VM_RESUME_CHECKPOINT_INVALID) {
        char disarm_why[VM_FW_BOOT_DETAIL_CAPACITY] = {0};
        (void)vm_resume_checkpoint_disarm(machine_directory, disarm_why, sizeof disarm_why);
    }

    char snapshots[VM_GUEST_INSTALL_PATH_CAPACITY];
    vm_snapshot_info_t snap_items[VM_SNAPSHOT_MAX];
    size_t snap_count = 0;
    if (vm_snapshot_dir(machine_directory, snapshots, sizeof snapshots) != VM_SNAPSHOT_OK ||
        vm_snapshot_list(snapshots, snap_items, VM_SNAPSHOT_MAX, &snap_count, detail, detail_capacity) != VM_SNAPSHOT_OK ||
        snap_count > 0) {
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Historical snapshots prevent changing this disk. Resolve snapshots first.");
        return -1;
    }

    /* 2. Inspect IPA and unpack into temporary contents file */
    engine_ipa_info_t info;
    if (!engine_ipa_inspect_path(ipa_path, &info, detail, detail_capacity)) {
        return -1;
    }

    int fd = open(ipa_path, O_RDONLY);
    if (fd < 0) {
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Failed to reopen IPA: %s", strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Failed to fstat IPA.");
        return -1;
    }

    ipa_file_reader_t reader = { .fd = fd };
    vmfw_zip_t zip;
    if (vmfw_zip_open(&zip, ipa_pread, &reader, (uint64_t)st.st_size) != VMFW_ZIP_OK) {
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Failed to reopen ZIP.");
        return -1;
    }

    vm_ipa_archive_t plan;
    char subdetail[320] = {0};
    if (!vm_ipa_archive_open(&zip, &plan, subdetail, sizeof subdetail)) {
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "%s", subdetail);
        return -1;
    }

    char packed_path[1024];
    snprintf(packed_path, sizeof packed_path, "%s/ipa_contents_%u.bin", cache_directory, (unsigned)getpid());
    FILE *packed_file = fopen(packed_path, "wb+");
    if (!packed_file) {
        vm_ipa_archive_close(&plan);
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Cannot create temporary file in %s", cache_directory);
        return -1;
    }

    uint64_t *offsets = calloc(plan.count, sizeof(uint64_t));
    if (!offsets) {
        fclose(packed_file);
        unlink(packed_path);
        vm_ipa_archive_close(&plan);
        close(fd);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Out of memory allocating offset table.");
        return -1;
    }

    uint64_t packed_offset = 0;
    for (unsigned i = 0; i < plan.count; i++) {
        offsets[i] = packed_offset;
        if (plan.files[i].is_directory) continue;
        vmfw_zip_status_t zst = vmfw_zip_extract(&zip, &plan.files[i], ipa_sink_file, packed_file);
        if (zst != VMFW_ZIP_OK) {
            free(offsets);
            fclose(packed_file);
            unlink(packed_path);
            vm_ipa_archive_close(&plan);
            close(fd);
            if (detail && detail_capacity) snprintf(detail, detail_capacity, "Error extracting %s: %s", plan.files[i].name, vmfw_zip_strerror(zst));
            return -1;
        }
        packed_offset += plan.files[i].uncompressed_size;
    }

    fflush(packed_file);
    uint8_t *packed_bytes = NULL;
    if (packed_offset > 0) {
        packed_bytes = malloc((size_t)packed_offset);
        if (!packed_bytes) {
            free(offsets);
            fclose(packed_file);
            unlink(packed_path);
            vm_ipa_archive_close(&plan);
            close(fd);
            if (detail && detail_capacity) snprintf(detail, detail_capacity, "Out of memory mapping %llu bytes.", (unsigned long long)packed_offset);
            return -1;
        }
        fseek(packed_file, 0, SEEK_SET);
        if (fread(packed_bytes, 1, (size_t)packed_offset, packed_file) != packed_offset) {
            free(packed_bytes);
            free(offsets);
            fclose(packed_file);
            unlink(packed_path);
            vm_ipa_archive_close(&plan);
            close(fd);
            if (detail && detail_capacity) snprintf(detail, detail_capacity, "Failed reading packed IPA file.");
            return -1;
        }
    }
    fclose(packed_file);
    unlink(packed_path);
    close(fd);

    /* 3. Compute slug and transaction identity */
    uint8_t id_hash[32];
    ios3_sha256((const uint8_t *)info.bundle_id, strlen(info.bundle_id), id_hash);
    char slug[64];
    snprintf(slug, sizeof slug, "s5lbox-");
    for (unsigned i = 0; i < 16; i++) {
        snprintf(slug + 7 + i * 2, 3, "%02x", id_hash[i]);
    }

    uint8_t identity[32];
    ios3_sha256_context_t sha_ctx;
    ios3_sha256_init(&sha_ctx);
    if (packed_bytes && packed_offset > 0) {
        ios3_sha256_update(&sha_ctx, packed_bytes, (size_t)packed_offset);
    }
    for (unsigned i = 0; i < plan.count; i++) {
        ios3_sha256_update(&sha_ctx, plan.files[i].name, strlen(plan.files[i].name) + 1);
    }
    ios3_sha256_final(&sha_ctx, identity);

    /* 4. Prepare staging disk */
    vm_guest_install_result_t transaction;
    if (vm_guest_app_prepare_stage(machine_directory, &transaction, detail, detail_capacity) != VM_GUEST_INSTALL_OK) {
        free(packed_bytes);
        free(offsets);
        vm_ipa_archive_close(&plan);
        return -1;
    }

    char stage_img[VM_GUEST_INSTALL_PATH_CAPACITY];
    if (!vm_guest_app_stage_image_path(stage_img, sizeof stage_img, machine_directory)) {
        vm_guest_app_discard_stage(machine_directory, NULL, 0);
        free(packed_bytes);
        free(offsets);
        vm_ipa_archive_close(&plan);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Failed deriving stage image path.");
        return -1;
    }

    if (vm_guest_app_clone_live_to_stage(machine_directory, detail, detail_capacity) != VM_GUEST_INSTALL_OK) {
        vm_guest_app_discard_stage(machine_directory, NULL, 0);
        free(packed_bytes);
        free(offsets);
        vm_ipa_archive_close(&plan);
        return -1;
    }

    /* 5. Build directory and file entries */
    char apps_base[512] = "/Applications";
    (void)rootfs_work_resolve_parent_symlinks(stage_img, "/Applications", apps_base, sizeof apps_base);

    char app_dir[512];
    snprintf(app_dir, sizeof app_dir, "%s/%s.app", apps_base, slug);

    char script_path[512];
    snprintf(script_path, sizeof script_path, "/usr/libexec/%s", slug);

    char daemon_path[512];
    snprintf(daemon_path, sizeof daemon_path, "/Library/LaunchDaemons/com.s5lbox.%s.plist", slug);

    char script_buf[1024];
    snprintf(script_buf, sizeof script_buf,
        "#!/bin/sh\n"
        "[ -f '/private/var/mobile/Library/%s.registered' ] && exit 0\n"
        "[ -f /var/mobile/Library/Caches/com.apple.mobile.installation.plist ] || exit 1\n"
        "[ -x /usr/bin/uicache ] || exit 1\n"
        "/bin/su --login --command /usr/bin/uicache mobile || exit 1\n"
        "touch '/private/var/mobile/Library/%s.registered'\n",
        slug, slug);

    char daemon_buf[1024];
    snprintf(daemon_buf, sizeof daemon_buf,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "\t<key>Label</key>\n"
        "\t<string>com.s5lbox.%s</string>\n"
        "\t<key>ProgramArguments</key>\n"
        "\t<array>\n"
        "\t\t<string>/bin/sh</string>\n"
        "\t\t<string>/usr/libexec/%s</string>\n"
        "\t</array>\n"
        "\t<key>RunAtLoad</key>\n"
        "\t<true/>\n"
        "\t<key>StartInterval</key>\n"
        "\t<integer>30</integer>\n"
        "\t<key>UserName</key>\n"
        "\t<string>root</string>\n"
        "</dict>\n"
        "</plist>\n",
        slug, slug);

    /* Allocate entries */
    size_t max_entries = plan.count + 64;
    rootfs_work_entry_t *entries = calloc(max_entries, sizeof(rootfs_work_entry_t));
    char **entry_paths = calloc(max_entries, sizeof(char *));
    if (!entries || !entry_paths) {
        free(entries);
        free(entry_paths);
        vm_guest_app_discard_stage(machine_directory, NULL, 0);
        free(packed_bytes);
        free(offsets);
        vm_ipa_archive_close(&plan);
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Out of memory allocating provision entries.");
        return -1;
    }

    size_t ecount = 0;

    /* Base dirs: app_dir is the new bundle directory. Pre-existing system directories
     * (/Applications, /Library, /Library/LaunchDaemons, /usr, /usr/libexec) must not be
     * provisioned as entries to prevent rejecting stashed symlinks like Cydia's /Applications. */
    #define ADD_DIR(p) do { \
        entry_paths[ecount] = strdup(p); \
        entries[ecount].kind = ROOTFS_WORK_ENTRY_DIRECTORY; \
        entries[ecount].path = entry_paths[ecount]; \
        entries[ecount].permissions = 0755; \
        entries[ecount].existing_policy = ROOTFS_WORK_EXISTING_REUSE_DIRECTORY; \
        ecount++; \
    } while (0)

    ADD_DIR(app_dir);

    /* Script and Daemon */
    entry_paths[ecount] = strdup(script_path);
    entries[ecount].kind = ROOTFS_WORK_ENTRY_FILE;
    entries[ecount].path = entry_paths[ecount];
    entries[ecount].content = (const uint8_t *)script_buf;
    entries[ecount].content_size = strlen(script_buf);
    entries[ecount].permissions = 0755;
    entries[ecount].existing_policy = ROOTFS_WORK_EXISTING_REFUSE;
    ecount++;

    entry_paths[ecount] = strdup(daemon_path);
    entries[ecount].kind = ROOTFS_WORK_ENTRY_FILE;
    entries[ecount].path = entry_paths[ecount];
    entries[ecount].content = (const uint8_t *)daemon_buf;
    entries[ecount].content_size = strlen(daemon_buf);
    entries[ecount].permissions = 0644;
    entries[ecount].existing_policy = ROOTFS_WORK_EXISTING_REFUSE;
    ecount++;

    /* Find executable index for permissions */
    char main_path[512];
    snprintf(main_path, sizeof main_path, "%s/%s", plan.app_root, info.executable);
    unsigned exec_idx = UINT32_MAX;
    for (unsigned i = 0; i < plan.count; i++) {
        if (!strcmp(plan.files[i].name, main_path)) {
            exec_idx = plan.content_indices[i];
            break;
        }
    }

    /* Add app bundle files */
    size_t app_root_len = strlen(plan.app_root);
    for (unsigned i = 0; i < plan.count; i++) {
        const char *src = plan.files[i].name;
        if (!strncmp(src, plan.app_root, app_root_len)) {
            const char *sub = src + app_root_len;
            if (*sub == '/') sub++;
            if (!*sub) continue; /* app root itself */

            char dest_path[1024];
            snprintf(dest_path, sizeof dest_path, "%s/%s", app_dir, sub);
            size_t dlen = strlen(dest_path);
            if (dlen > 0 && dest_path[dlen - 1] == '/') dest_path[dlen - 1] = 0;

            if (ecount >= max_entries) break;

            entry_paths[ecount] = strdup(dest_path);
            entries[ecount].path = entry_paths[ecount];
            if (plan.files[i].is_directory) {
                entries[ecount].kind = ROOTFS_WORK_ENTRY_DIRECTORY;
                entries[ecount].permissions = 0755;
                entries[ecount].existing_policy = ROOTFS_WORK_EXISTING_REUSE_DIRECTORY;
            } else if (vm_ipa_entry_is_symlink(&plan.files[i])) {
                entries[ecount].kind = ROOTFS_WORK_ENTRY_SYMLINK;
                entries[ecount].content = packed_bytes + offsets[i];
                entries[ecount].content_size = (size_t)plan.files[i].uncompressed_size;
                entries[ecount].permissions = 0777;
                entries[ecount].existing_policy = ROOTFS_WORK_EXISTING_REFUSE;
            } else {
                entries[ecount].kind = ROOTFS_WORK_ENTRY_FILE;
                entries[ecount].content = packed_bytes + offsets[i];
                entries[ecount].content_size = (size_t)plan.files[i].uncompressed_size;
                entries[ecount].permissions = ((plan.files[i].unix_mode & 0111u) || i == exec_idx) ? 0755 : 0644;
                entries[ecount].existing_policy = ROOTFS_WORK_EXISTING_REFUSE;
            }
            ecount++;
        }
    }
    #undef ADD_DIR

    /* Sort entries lexicographically */
    qsort(entries, ecount, sizeof(rootfs_work_entry_t), entry_cmp);

    /* Deduplicate identical paths (keeping last) */
    size_t unique = 0;
    for (size_t i = 0; i < ecount; i++) {
        if (unique > 0 && !strcmp(entries[i].path, entries[unique - 1].path)) {
            continue;
        }
        entries[unique++] = entries[i];
    }

    /* 6. Repair clone (best effort) and provision */
    rootfs_work_result_t rw_result;
    bool provision_ok = false;
    rootfs_work_status_t repair_st = rootfs_work_repair_powered_off_clone(stage_img, &rw_result);
    if (repair_st != ROOTFS_WORK_OK) {
        /* Powered-off allocation recovery is only supported when the extents-overflow
         * tree is empty. If the guest disk has fragmented files or active overflow extents,
         * skip repair and proceed directly with provisioning (allow_unclean=true). */
        web_log_printf("warn",
            "engine_ipa: repair clone skipped (%s), proceeding to provision",
            rw_result.detail[0] ? rw_result.detail : "unsupported repair state");
    }
    if (rootfs_work_provision_clone(stage_img, entries, unique, true, &rw_result) == ROOTFS_WORK_OK) {
        provision_ok = true;
    } else {
        if (detail && detail_capacity) snprintf(detail, detail_capacity, "Provision failed: %s", rw_result.detail);
    }

    /* Free entries */
    for (size_t i = 0; i < ecount; i++) free(entry_paths[i]);
    free(entry_paths);
    free(entries);
    free(packed_bytes);
    free(offsets);
    vm_ipa_archive_close(&plan);

    if (!provision_ok) {
        vm_guest_app_discard_stage(machine_directory, NULL, 0);
        return -1;
    }

    /* 7. Publish stage to live */
    vm_guest_install_status_t pub_st = vm_guest_app_publish(machine_directory, identity, &transaction, detail, detail_capacity);
    if (pub_st != VM_GUEST_INSTALL_OK || !transaction.committed) {
        if (detail && detail_capacity && !detail[0]) {
            snprintf(detail, detail_capacity, "Failed to commit app into guest rootfs.");
        }
        return -1;
    }

    return 0;
}
