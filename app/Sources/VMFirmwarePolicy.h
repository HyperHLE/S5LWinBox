// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#ifndef S5LBOX_VM_FIRMWARE_POLICY_H
#define S5LBOX_VM_FIRMWARE_POLICY_H
#include <stdbool.h>
#include <stdint.h>

#define VM_FW_POLICY_FILE "import-policy-v1"
typedef struct {
    bool skip_stock_checksums;
    uint64_t sizes[3]; /* kernel, device tree, root filesystem */
} vm_fw_policy_t;
/* 0: legacy/no record (strict), 1: valid record, -1: invalid/unreadable.
 * The record belongs to one immutable imported generation, not app defaults. */
int vm_fw_policy_read(const char *directory, vm_fw_policy_t *policy);
/* Only call for an unpublished, owned staging directory. */
bool vm_fw_policy_write(const char *directory, const vm_fw_policy_t *policy);
#endif
