/* S5LWinBox — untrusted guest IPA inspection and installation. MIT licensed. */
#ifndef S5L_ENGINE_IPA_H
#define S5L_ENGINE_IPA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char bundle_id[256];
    char display_name[256];
    char version[64];
    char executable[256];
    char app_root[256];
    char min_os[64];
    uint32_t file_count;
    uint64_t uncompressed_bytes;
} engine_ipa_info_t;

/* Inspect an IPA file without modifying any guest filesystem.
 * Returns true if valid ARMv6 iOS 3.1.3-compatible app, false with error in detail. */
bool engine_ipa_inspect_path(const char *ipa_path,
                             engine_ipa_info_t *out_info,
                             char *detail, size_t detail_capacity);

/* Install an IPA into the guest machine directory.
 * Machine must be powered off with 0 snapshots and jailbroken. */
int engine_ipa_install_path(const char *machine_directory,
                            const char *cache_directory,
                            const char *ipa_path,
                            char *detail, size_t detail_capacity);

#ifdef __cplusplus
}
#endif

#endif /* S5L_ENGINE_IPA_H */
