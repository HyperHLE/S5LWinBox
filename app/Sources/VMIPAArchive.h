/* S5LBox -- bounded, untrusted guest IPA inspection. MIT licensed. */
#ifndef S5LBOX_VM_IPA_ARCHIVE_H
#define S5LBOX_VM_IPA_ARCHIVE_H
#include "VMFirmwareFormats.h"

#define VM_IPA_MAX_FILES 4096u
#define VM_IPA_MAX_BYTES (256u * 1024u * 1024u)
#define VM_IPA_MAX_FILE_BYTES (128u * 1024u * 1024u)

typedef struct {
    vmfw_zip_entry_t *files;
    /* For a validated file link, the final regular-file entry. Other entries
     * map to themselves. Original link bytes are retained for guest install. */
    unsigned *content_indices;
    unsigned count;
    unsigned info_index;
    uint64_t unpacked_bytes;
    char app_root[VMFW_ZIP_MAX_NAME];
} vm_ipa_archive_t;

/* Reads bounded link targets into memory; never extracts paths onto the host.
 * Supports canonical relative file links inside this app (including chains).
 * Directory links, dot components, dangling links and cycles fail closed.
 * Free a successful plan with close. */
bool vm_ipa_archive_open(const vmfw_zip_t *zip, vm_ipa_archive_t *plan,
                         char *detail, size_t capacity);
void vm_ipa_archive_close(vm_ipa_archive_t *plan);
bool vm_ipa_entry_is_symlink(const vmfw_zip_entry_t *entry);
bool vm_ipa_safe_path(const char *path);
/* A bounded PNG header check before handing untrusted icon bytes to the native
 * image decoder. Accepts both ordinary PNG and Apple's legacy CgBI prefix. */
bool vm_ipa_icon_png_dimensions(const uint8_t *bytes, size_t size,
                                uint32_t *width, uint32_t *height);
/* Main executable must have an unencrypted ARMv6-compatible Mach-O slice,
 * with a deployment target no newer than iPhone OS 3.1.3. */
bool vm_ipa_check_executable(const uint8_t *bytes, size_t size,
                             char *detail, size_t capacity);
#endif
