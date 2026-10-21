/*
 * S5LBox — Dynarmic A32 JIT Bridge Interface.
 *
 * Integrates Lioncash Dynarmic A32 JIT compiler for ARMv6 / ARM1176JZF-S.
 */
#ifndef DYNARMIC_JIT_H
#define DYNARMIC_JIT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "soc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dynarmic_jit dynarmic_jit_t;

/* Create a Dynarmic A32 JIT compiler bound to an S5L8900 machine */
dynarmic_jit_t *dynarmic_jit_create(s5l8900_t *machine);

/* Destroy the JIT compiler */
void dynarmic_jit_destroy(dynarmic_jit_t *jit);

/* Run compiled user code for up to max_ticks. Returns instructions retired. */
uint32_t dynarmic_jit_run(dynarmic_jit_t *jit, uint32_t max_ticks);

/* Clear entire code cache (e.g. on TLB flush or TTBR change) */
void dynarmic_jit_clear_cache(dynarmic_jit_t *jit);

/* Invalidate a specific range of addresses in the code cache */
void dynarmic_jit_invalidate_range(dynarmic_jit_t *jit, uint32_t start_addr, size_t length);

/* The s5l_jit_ops table for wiring directly into s5l8900_set_jit */
extern const s5l_jit_ops_t g_dynarmic_jit_ops;

#ifdef __cplusplus
}
#endif

#endif /* DYNARMIC_JIT_H */
