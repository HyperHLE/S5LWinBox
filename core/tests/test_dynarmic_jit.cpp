#include "dynarmic_jit.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <cstring>

static unsigned failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
    ++failures; } } while (0)

struct Machine {
    std::unique_ptr<s5l8900_t> m = std::make_unique<s5l8900_t>();
    dynarmic_jit_t* jit = nullptr;

    Machine() {
        if (!s5l8900_init(m.get(), 0, 2u << 20)) std::abort();
        arm_set_mode(&m->cpu, ARM_MODE_USR);
        m->cpu.cpsr = ARM_MODE_USR;
        m->cpu.r[15] = 0x1000;
        // A writable identity-mapped section; the following section faults.
        put(0x4000, 0x00000c02);
        m->cpu.cp15.ttbr0 = 0x4000;
        m->cpu.cp15.dacr = 1;
        m->cpu.cp15.sctlr |= ARM_SCTLR_M | ARM_SCTLR_U;
    }
    ~Machine() {
        dynarmic_jit_destroy(jit);
        s5l8900_free(m.get());
    }
    void put(uint32_t addr, uint32_t word) { m->bus.write32(m->bus.ctx, addr, word); }
    uint32_t run(unsigned budget) {
        if (!jit) jit = dynarmic_jit_create(m.get());
        if (!jit) std::abort();
        return dynarmic_jit_run(jit, budget);
    }
};

static void test_exclusive_across_slices() {
    Machine f;
    f.m->cpu.r[0] = 0x2000;
    f.put(0x2000, 7);
    f.put(0x1000, 0xe1901f9f); // LDREX r1, [r0]
    f.put(0x1004, 0xea00003d); // B 0x1100
    f.put(0x1100, 0xe2811001); // ADD r1, r1, #1
    f.put(0x1104, 0xe1802f91); // STREX r2, r1, [r0]
    f.put(0x1108, 0xef000080); // SVC
    f.run(1);
    CHECK(f.m->cpu.r[15] == 0x1100);
    f.run(16);
    CHECK(f.m->cpu.r[2] == 0);
    CHECK(f.m->bus.read32(f.m->bus.ctx, 0x2000) == 8);
}

static void test_exclusive_detects_changed_value() {
    Machine f;
    f.m->cpu.r[0] = 0x2000;
    f.put(0x2000, 7);
    f.put(0x1000, 0xe1901f9f); // LDREX r1, [r0]
    f.put(0x1004, 0xe3a03009); // MOV r3, #9
    f.put(0x1008, 0xe5803000); // STR r3, [r0]
    f.put(0x100c, 0xe1802f91); // STREX r2, r1, [r0]
    f.put(0x1010, 0xef000080);
    f.run(32);
    CHECK(f.m->cpu.r[2] == 1);
    CHECK(f.m->bus.read32(f.m->bus.ctx, 0x2000) == 9);
}

static void test_data_abort(uint32_t instruction, uint32_t address, bool prefix = false) {
    Machine ref, f;
    for (auto* machine : {&ref, &f}) {
        if (prefix) machine->put(0x1000, 0xe3a0204d); // MOV r2, #77
        machine->put(prefix ? 0x1004 : 0x1000, instruction);
        machine->put(prefix ? 0x1008 : 0x1004, 0xef000080);
        machine->put(0xffffc, 0x12345678);
        machine->m->cpu.r[0] = address;
        machine->m->cpu.r[1] = 0xaabbccdd;
        machine->m->cpu.r[2] = 0x55667788;
    }
    if (prefix) CHECK(arm_step(&ref.m->cpu) == ARM_OK);
    CHECK(arm_step(&ref.m->cpu) == ARM_OK);
    f.run(32);
    for (unsigned i = 0; i < 16; ++i) {
        if (f.m->cpu.r[i] != ref.m->cpu.r[i])
            std::fprintf(stderr, "insn=%08x r%u: JIT=%08x interpreter=%08x\n", instruction, i, f.m->cpu.r[i], ref.m->cpu.r[i]);
        CHECK(f.m->cpu.r[i] == ref.m->cpu.r[i]);
    }
    CHECK(f.m->cpu.cpsr == ref.m->cpu.cpsr);
    CHECK(f.m->cpu.cp15.dfar == ref.m->cpu.cp15.dfar);
    CHECK(f.m->cpu.cp15.dfsr == ref.m->cpu.cp15.dfsr);
    CHECK(f.m->cpu.cycles == ref.m->cpu.cycles);
}

static void test_lazy_vfp() {
    Machine ref, f;
    for (auto* machine : {&ref, &f}) {
        machine->put(0x1000, 0xee000a10); // VMOV s0, r0
        machine->put(0x1004, 0xef000080);
        machine->m->cpu.r[0] = 0x3f800000;
        machine->m->cpu.cp15.cpacr = 0x00f00000;
        machine->m->cpu.vfp_fpexc = 0;
    }
    CHECK(arm_step(&ref.m->cpu) == ARM_OK);
    f.run(32);
    CHECK(f.m->cpu.r[15] == ref.m->cpu.r[15]);
    CHECK(f.m->cpu.cpsr == ref.m->cpu.cpsr);
    CHECK(f.m->cpu.vfp_s[0] == ref.m->cpu.vfp_s[0]);
    CHECK(f.m->cpu.cycles == ref.m->cpu.cycles);
}

static void test_alignment(uint32_t instruction, uint32_t control) {
    Machine ref, f;
    for (auto* machine : {&ref, &f}) {
        machine->put(0x1000, instruction);
        machine->put(0x1004, 0xef000080);
        machine->put(0x2000, 0x12345678);
        machine->put(0x2004, 0x9abcdef0);
        machine->m->cpu.r[0] = 0x2001;
        machine->m->cpu.r[1] = 0xaabbccdd;
        machine->m->cpu.cp15.sctlr &= ~(ARM_SCTLR_A | ARM_SCTLR_U);
        machine->m->cpu.cp15.sctlr |= control;
    }
    CHECK(arm_step(&ref.m->cpu) == ARM_OK);
    if ((ref.m->cpu.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_USR)
        CHECK(arm_step(&ref.m->cpu) == ARM_OK);
    f.run(32);
    CHECK(f.m->cpu.r[1] == ref.m->cpu.r[1]);
    CHECK(f.m->cpu.cpsr == ref.m->cpu.cpsr);
    CHECK(f.m->cpu.r[15] == ref.m->cpu.r[15]);
    CHECK(f.m->cpu.cp15.dfar == ref.m->cpu.cp15.dfar);
    CHECK(f.m->cpu.cp15.dfsr == ref.m->cpu.cp15.dfsr);
    CHECK(f.m->bus.read32(f.m->bus.ctx, 0x2000) == ref.m->bus.read32(ref.m->bus.ctx, 0x2000));
    CHECK(f.m->bus.read32(f.m->bus.ctx, 0x2004) == ref.m->bus.read32(ref.m->bus.ctx, 0x2004));
    CHECK(f.m->cpu.cycles == ref.m->cpu.cycles);
}

static void test_native_instruction(uint32_t instruction, uint32_t fpscr = 0, bool set_vector_mode = false) {
    Machine ref, f;
    for (auto* machine : {&ref, &f}) {
        if (set_vector_mode) machine->put(0x1000, 0xeee10a10); // VMSR FPSCR, r0
        machine->put(set_vector_mode ? 0x1004 : 0x1000, instruction);
        machine->put(set_vector_mode ? 0x1008 : 0x1004, 0xef000080);
        machine->m->cpu.cp15.cpacr = 0x00f00000;
        machine->m->cpu.vfp_fpexc = 0x40000000;
        machine->m->cpu.vfp_fpscr = fpscr;
        machine->m->cpu.r[0] = set_vector_mode ? 0x03030000 : 0x2000;
        machine->m->cpu.r[1] = 0x3ff00000;
        for (unsigned i = 0; i < 32; ++i)
            machine->m->cpu.vfp_s[i] = 0x3f800000 + (i % 4) * 0x800000;
    }
    if (set_vector_mode) CHECK(arm_step(&ref.m->cpu) == ARM_OK);
    CHECK(arm_step(&ref.m->cpu) == ARM_OK);
    CHECK(arm_step(&ref.m->cpu) == ARM_OK);
    unsigned before = failures;
    for (unsigned slices = 0; slices < 32 &&
         (f.m->cpu.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_USR; ++slices)
        f.run(32);
    CHECK(std::memcmp(f.m->cpu.r, ref.m->cpu.r, sizeof ref.m->cpu.r) == 0);
    CHECK(std::memcmp(f.m->cpu.vfp_s, ref.m->cpu.vfp_s, sizeof ref.m->cpu.vfp_s) == 0);
    CHECK(f.m->cpu.cpsr == ref.m->cpu.cpsr);
    CHECK(f.m->cpu.vfp_fpscr == ref.m->cpu.vfp_fpscr);
    CHECK(f.m->cpu.cycles == ref.m->cpu.cycles);
    CHECK(f.m->bus.read32(f.m->bus.ctx, 0x2000) == ref.m->bus.read32(ref.m->bus.ctx, 0x2000));
    if (failures != before)
        std::fprintf(stderr, "native mismatch insn=%08x fpscr=%08x actual_fpscr=%08x expected_fpscr=%08x actual_cycles=%llu expected_cycles=%llu\n",
            instruction, fpscr, f.m->cpu.vfp_fpscr, ref.m->cpu.vfp_fpscr,
            (unsigned long long)f.m->cpu.cycles, (unsigned long long)ref.m->cpu.cycles);
}

static void test_code_range_invalidation() {
    Machine f;
    f.put(0x1000, 0xe3a00001); // MOV r0, #1
    f.put(0x1004, 0xef000080);
    f.run(32);
    CHECK(f.m->cpu.r[0] == 1);
    arm_set_mode(&f.m->cpu, ARM_MODE_USR);
    f.m->cpu.cpsr = ARM_MODE_USR;
    f.m->cpu.r[15] = 0x1000;
    f.put(0x1000, 0xe3a00002); // MOV r0, #2
    dynarmic_jit_invalidate_range(f.jit, 0x1000, 4);
    f.run(32);
    CHECK(f.m->cpu.r[0] == 2);
}

static void test_kernel_code_invalidation() {
    Machine f;
    f.put(0x1000, 0xe3a00001);
    f.put(0x1004, 0xef000080);
    f.run(32);
    s5l8900_set_jit(f.m.get(), f.jit, &g_dynarmic_jit_ops);
    f.put(0x1000, 0xe3a00002);
    f.put(0x1100, 0xee070f15); // MCR p15, 0, r0, c7, c5, 0 (ICIALLU)
    f.m->cpu.r[15] = 0x1100;
    CHECK(arm_step(&f.m->cpu) == ARM_OK);
    arm_set_mode(&f.m->cpu, ARM_MODE_USR);
    f.m->cpu.cpsr = ARM_MODE_USR;
    f.m->cpu.r[15] = 0x1000;
    f.run(32);
    CHECK(f.m->cpu.r[0] == 2);
    f.jit = nullptr;
}

static void test_fatal_fallback_status() {
    Machine ref, f;
    for (auto* machine : {&ref, &f}) {
        machine->m->cpu.cp15.sctlr &= ~(ARM_SCTLR_A | ARM_SCTLR_U);
        machine->m->cpu.r[0] = 0x2001;
        machine->put(0x1000, 0xe1d010b0); // unaligned legacy LDRH is unsupported
    }
    CHECK(arm_step(&ref.m->cpu) == ARM_UNDEFINED);
    f.jit = dynarmic_jit_create(f.m.get());
    s5l8900_set_jit(f.m.get(), f.jit, &g_dynarmic_jit_ops);
    arm_status_t status = ARM_OK;
    CHECK(s5l8900_run(f.m.get(), 32, &status) == 0);
    CHECK(status == ARM_UNDEFINED);
    CHECK(f.m->cpu.r[15] == ref.m->cpu.r[15]);
    CHECK(f.m->cpu.cycles == ref.m->cpu.cycles);
    f.jit = nullptr;
}

static bool hook(void* opaque) {
    auto* m = static_cast<s5l8900_t*>(opaque);
    m->cpu.r[0] = 42;
    m->cpu.r[15] += 4;
    return true;
}

static void test_vfp_disabled_after_cached_execution() {
    Machine f;
    f.put(0x1000, 0xee000a10); // VMOV s0, r0
    f.put(0x1004, 0xef000080);
    f.m->cpu.cp15.cpacr = 0x00f00000;
    f.m->cpu.vfp_fpexc = 0x40000000;
    f.m->cpu.r[0] = 0x3f800000;
    f.run(32);
    CHECK(f.m->cpu.vfp_s[0] == 0x3f800000);
    arm_set_mode(&f.m->cpu, ARM_MODE_USR);
    f.m->cpu.cpsr = ARM_MODE_USR;
    f.m->cpu.r[15] = 0x1000;
    f.m->cpu.r[0] = 0x40000000;
    f.m->cpu.vfp_fpexc = 0;
    f.run(32);
    CHECK((f.m->cpu.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND);
    CHECK(f.m->cpu.r[15] == 4);
    CHECK(f.m->cpu.vfp_s[0] == 0x3f800000);
}

static void test_branch_to_hook() {
    Machine f;
    f.put(0x1000, 0xea00003e); // B 0x1100
    f.put(0x1100, 0xe3a00063); // MOV r0, #99; replaced by hook
    f.put(0x1104, 0xef000080);
    const uint32_t target = 0x1100;
    CHECK(s5l8900_set_pre_step_hook(f.m.get(), hook, f.m.get(), &target, 1));
    f.jit = dynarmic_jit_create(f.m.get());
    CHECK(f.jit != nullptr);
    s5l8900_set_jit(f.m.get(), f.jit, &g_dynarmic_jit_ops);
    arm_status_t status = ARM_OK;
    s5l8900_run(f.m.get(), 4, &status);
    CHECK(f.m->cpu.r[0] == 42);
    CHECK(s5l8900_pre_step_handled(f.m.get()) == 1);
    f.jit = nullptr; // machine now owns it
}

int main() {
    test_exclusive_across_slices();
    test_exclusive_detects_changed_value();
    test_data_abort(0xe4901004, 0x100000); // LDR r1, [r0], #4
    test_data_abort(0xe8b00006, 0xffffc); // LDMIA r0!, {r1, r2}, second word faults
    test_data_abort(0xe5901000, 0x100000); // LDR r1, [r0]
    test_data_abort(0xe5901000, 0x100000, true); // retired prefix before a native fault
    test_data_abort(0xe1c000d0, 0xffffc); // LDRD r0, r1, [r0], second word faults
    test_lazy_vfp();
    test_vfp_disabled_after_cached_execution();
    test_code_range_invalidation();
    test_kernel_code_invalidation();
    test_fatal_fallback_status();
    test_native_instruction(0xe580f000); // STR pc, [r0]: ARM1176 stores PC+12
    test_native_instruction(0xf10c0080); // CPSID i is ignored in user mode
    test_native_instruction(0xee366b07, 0, true); // vector mode set inside a native slice
    for (uint32_t instruction : {0xee302a20u, 0xee302a60u, 0xee202a20u,
         0xee002a20u, 0xee802a20u, 0xeeb12ac0u, 0xeeb02ac0u, 0xeeb02a40u,
         0xee304b01u, 0xee804b01u, 0xee274b10u, 0xee070b10u, 0xee366b07u})
        for (uint32_t fpscr : {0u, 0x03000000u, 0x03030000u})
            test_native_instruction(instruction, fpscr);
    for (uint32_t instruction : {0xe5901000u, 0xe5801000u, 0xe1d010b0u, 0xe1c010b0u})
        for (uint32_t control : {0u, ARM_SCTLR_U, ARM_SCTLR_A | ARM_SCTLR_U})
            if ((instruction & 0x0c000000u) == 0x04000000u || control != 0)
                test_alignment(instruction, control);
    test_branch_to_hook();
    std::printf("Dynarmic integration: %u failures\n", failures);
    return failures ? 1 : 0;
}
