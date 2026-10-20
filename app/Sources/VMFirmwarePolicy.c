// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#include "VMFirmwarePolicy.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const unsigned char magic[16] = "S5LBoxFWPolicy1";
static bool valid(const vm_fw_policy_t *p) {
    static const uint64_t limits[3] = {UINT64_C(67108864), UINT64_C(4194304), UINT64_C(17179869184)};
    if (!p) return false;
    for (unsigned i = 0; i < 3; i++)
        if (!p->sizes[i] || p->sizes[i] > limits[i]) return false;
    return true;
}
static bool path_for(const char *directory, char path[2048]) {
    if (!directory || !*directory) return false;
    int n = snprintf(path, 2048, "%s/%s", directory, VM_FW_POLICY_FILE);
    return n > 0 && n < 2048;
}
int vm_fw_policy_read(const char *directory, vm_fw_policy_t *policy) {
    char path[2048];
    unsigned char bytes[48];
    if (!policy) return -1;
    memset(policy, 0, sizeof *policy);
    if (!path_for(directory, path)) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? 0 : -1;
    size_t n = fread(bytes, 1, sizeof bytes, f);
    bool complete = n == sizeof bytes && fgetc(f) == EOF && !ferror(f);
    if (fclose(f) != 0) complete = false;
    if (!complete || memcmp(bytes, magic, sizeof magic) || bytes[16] > 1) return -1;
    for (unsigned i = 17; i < 24; i++) if (bytes[i]) return -1;
    vm_fw_policy_t parsed = {0};
    parsed.skip_stock_checksums = bytes[16] != 0;
    for (unsigned i = 0; i < 3; i++)
        for (unsigned j = 0; j < 8; j++)
            parsed.sizes[i] |= (uint64_t)bytes[24 + i * 8 + j] << (j * 8);
    if (!valid(&parsed)) return -1;
    *policy = parsed;
    return 1;
}
bool vm_fw_policy_write(const char *directory, const vm_fw_policy_t *policy) {
    char path[2048];
    unsigned char bytes[48] = {0};
    if (!valid(policy) || !path_for(directory, path)) return false;
    memcpy(bytes, magic, sizeof magic);
    bytes[16] = policy->skip_stock_checksums ? 1 : 0;
    for (unsigned i = 0; i < 3; i++)
        for (unsigned j = 0; j < 8; j++)
            bytes[24 + i * 8 + j] = (unsigned char)(policy->sizes[i] >> (j * 8));
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(bytes, 1, sizeof bytes, f) == sizeof bytes;
    if (fclose(f) != 0) ok = false;
    return ok;
}
