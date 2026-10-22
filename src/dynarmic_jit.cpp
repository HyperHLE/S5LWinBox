/*
 * S5LBox — Dynarmic A32 JIT Implementation for ARMv6 (ARM1176JZF-S)
 *
 * Translates ARMv6 user mode code to host machine code with native execution speed.
 */

#include "dynarmic_jit.h"
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/interface/optimization_flags.h>

#include <memory>
#include <cstring>
#include <cstdio>

class DynarmicCP15 final : public Dynarmic::A32::Coprocessor {
public:
    using CoprocReg = Dynarmic::A32::CoprocReg;

    s5l8900_t* machine = nullptr;
    Dynarmic::A32::Jit* jit = nullptr;
    uint32_t dummy_sink = 0;

    static std::uint64_t Ignore(void*, std::uint32_t, std::uint32_t) {
        return 0;
    }

    static Callback CallbackForIgnoredOperation() {
        return Callback{&Ignore, std::nullopt};
    }

    static std::uint64_t InvalidateCacheCallback(void* arg, std::uint32_t, std::uint32_t) {
        auto* jit_instance = static_cast<Dynarmic::A32::Jit*>(arg);
        if (jit_instance) {
            jit_instance->ClearCache();
            jit_instance->HaltExecution(Dynarmic::HaltReason::CacheInvalidation);
        }
        return 0;
    }

    CallbackOrAccessOneWord CompileGetOneWord(bool /*two*/, unsigned /*opc1*/, CoprocReg CRn,
                                             CoprocReg CRm, unsigned opc2) override {
        unsigned n = static_cast<unsigned>(CRn);
        unsigned m = static_cast<unsigned>(CRm);

        if (n == 13 && m == 0) {
            if (opc2 == 3) {
                // TPIDRURO: Read-only thread ID (cthread_self / pthread_self)
                return &machine->cpu.cp15.tpidruro;
            } else if (opc2 == 2) {
                // TPIDRURW: Read-write thread ID
                return &machine->cpu.cp15.tpidrurw;
            } else if (opc2 == 1) {
                // Context ID
                return &machine->cpu.cp15.context_id;
            }
        }
        return CallbackForIgnoredOperation();
    }

    CallbackOrAccessOneWord CompileSendOneWord(bool /*two*/, unsigned /*opc1*/, CoprocReg CRn,
                                              CoprocReg CRm, unsigned opc2) override {
        unsigned n = static_cast<unsigned>(CRn);
        unsigned m = static_cast<unsigned>(CRm);

        if (n == 13 && m == 0 && opc2 == 2) {
            // TPIDRURW: Read-write thread ID
            return &machine->cpu.cp15.tpidrurw;
        } else if (n == 7) {
            if (m == 5) {
                // Instruction cache invalidate: clear JIT translation cache
                return Callback{&InvalidateCacheCallback, jit};
            }
            // Other cache/barrier ops are no-ops
            return &dummy_sink;
        }
        return CallbackForIgnoredOperation();
    }

    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg,
                                                    CoprocReg, CoprocReg, unsigned) override {
        return CallbackForIgnoredOperation();
    }

    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override {
        return CallbackForIgnoredOperation();
    }

    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override {
        return CallbackForIgnoredOperation();
    }

    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg,
                                             std::optional<std::uint8_t>) override {
        return CallbackForIgnoredOperation();
    }

    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg,
                                              std::optional<std::uint8_t>) override {
        return CallbackForIgnoredOperation();
    }
};

class DynarmicCallbacks final : public Dynarmic::A32::UserCallbacks {
public:
    s5l8900_t* machine = nullptr;
    Dynarmic::A32::Jit* jit = nullptr;
    uint64_t ticks_remaining = 0;
    uint64_t ticks_executed = 0;

    bool svc_taken = false;
    uint32_t svc_swi = 0;
    bool exception_taken = false;
    uint32_t exception_pc = 0;
    bool abort_taken = false;
    uint32_t abort_far = 0;
    uint32_t abort_fsr = 0;

    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A32::VAddr vaddr) override {
        uint32_t pa = 0;
        bool priv = (machine->cpu.cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
        uint32_t fsr = arm_mmu_translate(&machine->cpu, vaddr, ARM_ACCESS_FETCH, priv, &pa);
        if (fsr != 0) {
            return std::nullopt;
        }

        if ((vaddr & 0xfffu) > 0x1000u - 4u) {
            uint32_t val = 0;
            for (size_t i = 0; i < 4; ++i) {
                uint32_t byte_pa = 0;
                uint32_t byte_va = vaddr + static_cast<uint32_t>(i);
                uint32_t byte_fsr = arm_mmu_translate(&machine->cpu, byte_va, ARM_ACCESS_FETCH, priv, &byte_pa);
                if (byte_fsr != 0) return std::nullopt;
                uint8_t b = 0;
                if (byte_pa >= machine->ram_base && byte_pa < machine->ram_base + machine->ram_size) {
                    b = machine->ram[byte_pa - machine->ram_base];
                } else if (machine->cpu.bus && machine->cpu.bus->read8) {
                    b = machine->cpu.bus->read8(machine->cpu.bus->ctx, byte_pa);
                }
                val |= static_cast<uint32_t>(b) << (8 * i);
            }
            return val;
        }

        if (pa >= machine->ram_base && pa + 4 <= machine->ram_base + machine->ram_size) {
            uint32_t val = 0;
            std::memcpy(&val, &machine->ram[pa - machine->ram_base], 4);
            return val;
        }

        if (machine->cpu.bus && machine->cpu.bus->read32) {
            return machine->cpu.bus->read32(machine->cpu.bus->ctx, pa);
        }

        return std::nullopt;
    }

    bool PreCodeReadHook(bool /*is_thumb*/, Dynarmic::A32::VAddr /*pc*/, Dynarmic::A32::IREmitter& /*ir*/) override {
        return true;
    }

    void PreCodeTranslationHook(bool /*is_thumb*/, Dynarmic::A32::VAddr /*pc*/, Dynarmic::A32::IREmitter& /*ir*/) override {
    }

    void InstructionSynchronizationBarrierRaised() override {
        if (jit) {
            jit->ClearCache();
            jit->HaltExecution(Dynarmic::HaltReason::CacheInvalidation);
        }
    }

    std::uint8_t MemoryRead8(Dynarmic::A32::VAddr vaddr) override { return ReadMemory<uint8_t>(vaddr); }
    std::uint16_t MemoryRead16(Dynarmic::A32::VAddr vaddr) override { return ReadMemory<uint16_t>(vaddr); }
    std::uint32_t MemoryRead32(Dynarmic::A32::VAddr vaddr) override { return ReadMemory<uint32_t>(vaddr); }
    std::uint64_t MemoryRead64(Dynarmic::A32::VAddr vaddr) override { return ReadMemory<uint64_t>(vaddr); }

    void MemoryWrite8(Dynarmic::A32::VAddr vaddr, std::uint8_t value) override { WriteMemory<uint8_t>(vaddr, value); }
    void MemoryWrite16(Dynarmic::A32::VAddr vaddr, std::uint16_t value) override { WriteMemory<uint16_t>(vaddr, value); }
    void MemoryWrite32(Dynarmic::A32::VAddr vaddr, std::uint32_t value) override { WriteMemory<uint32_t>(vaddr, value); }
    void MemoryWrite64(Dynarmic::A32::VAddr vaddr, std::uint64_t value) override { WriteMemory<uint64_t>(vaddr, value); }

    void InterpreterFallback(Dynarmic::A32::VAddr pc, size_t num_instructions) override {
        auto& regs = jit->Regs();
        for (int i = 0; i < 16; ++i) machine->cpu.r[i] = regs[i];
        machine->cpu.cpsr = jit->Cpsr();
        auto& ext = jit->ExtRegs();
        for (int i = 0; i < 32; ++i) machine->cpu.vfp_s[i] = ext[i];
        machine->cpu.vfp_fpscr = jit->Fpscr();

        machine->cpu.r[15] = pc;

        for (size_t i = 0; i < num_instructions; ++i) {
            arm_status_t st = arm_step(&machine->cpu);
            ticks_executed++;
            if (st != ARM_OK || (machine->cpu.cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR) {
                for (int j = 0; j < 16; ++j) regs[j] = machine->cpu.r[j];
                jit->SetCpsr(machine->cpu.cpsr);
                for (int j = 0; j < 32; ++j) ext[j] = machine->cpu.vfp_s[j];
                jit->SetFpscr(machine->cpu.vfp_fpscr);
                jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
                return;
            }
        }

        for (int i = 0; i < 16; ++i) regs[i] = machine->cpu.r[i];
        jit->SetCpsr(machine->cpu.cpsr);
        for (int i = 0; i < 32; ++i) ext[i] = machine->cpu.vfp_s[i];
        jit->SetFpscr(machine->cpu.vfp_fpscr);
    }

    void CallSVC(std::uint32_t swi) override {
        svc_taken = true;
        svc_swi = swi;
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    }

    void ExceptionRaised(Dynarmic::A32::VAddr pc, Dynarmic::A32::Exception exception) override {
        switch (exception) {
        case Dynarmic::A32::Exception::WaitForInterrupt:
        case Dynarmic::A32::Exception::WaitForEvent:
        case Dynarmic::A32::Exception::Yield:
            machine->wfi_pace_yield = true;
            jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
            break;
        case Dynarmic::A32::Exception::UndefinedInstruction:
        case Dynarmic::A32::Exception::UnpredictableInstruction:
        case Dynarmic::A32::Exception::DecodeError:
        case Dynarmic::A32::Exception::NoExecuteFault:
        default:
            exception_taken = true;
            exception_pc = pc;
            jit->HaltExecution(Dynarmic::HaltReason::UserDefined3);
            break;
        }
    }

    void AddTicks(std::uint64_t ticks) override {
        ticks_executed += ticks;
        if (ticks >= ticks_remaining) {
            ticks_remaining = 0;
        } else {
            ticks_remaining -= ticks;
        }

        if (machine->restart_requested || machine->level_dirty ||
            (machine->cpu.irq_line && !(jit->Cpsr() & ARM_CPSR_I)) ||
            (machine->cpu.fiq_line && !(jit->Cpsr() & ARM_CPSR_F))) {
            jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        }
    }

    std::uint64_t GetTicksRemaining() override {
        return ticks_remaining;
    }

private:
    template <typename T>
    T ReadMemory(Dynarmic::A32::VAddr vaddr) {
        bool priv = (machine->cpu.cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;

        if constexpr (sizeof(T) > 1) {
            if (((vaddr & 0xfffu) + sizeof(T)) > 0x1000u) {
                T result = 0;
                for (size_t i = 0; i < sizeof(T); ++i) {
                    uint32_t pa = 0;
                    uint32_t a = vaddr + static_cast<uint32_t>(i);
                    uint32_t fsr = arm_mmu_translate(&machine->cpu, a, ARM_ACCESS_READ, priv, &pa);
                    if (fsr != 0) {
                        abort_taken = true;
                        abort_fsr = fsr;
                        abort_far = a;
                        jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
                        return 0;
                    }
                    uint8_t b = 0;
                    if (pa >= machine->ram_base && pa < machine->ram_base + machine->ram_size) {
                        b = machine->ram[pa - machine->ram_base];
                    } else if (machine->cpu.bus && machine->cpu.bus->read8) {
                        b = machine->cpu.bus->read8(machine->cpu.bus->ctx, pa);
                    }
                    result |= static_cast<T>(b) << (8 * i);
                }
                return result;
            }
        }

        uint32_t pa = 0;
        uint32_t fsr = arm_mmu_translate(&machine->cpu, vaddr, ARM_ACCESS_READ, priv, &pa);
        if (fsr != 0) {
            abort_taken = true;
            abort_fsr = fsr;
            abort_far = vaddr;
            jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
            return 0;
        }

        if (pa >= machine->ram_base && pa + sizeof(T) <= machine->ram_base + machine->ram_size) {
            T val{};
            std::memcpy(&val, &machine->ram[pa - machine->ram_base], sizeof(T));
            return val;
        }

        T val{};
        if (machine->cpu.bus) {
            if constexpr (sizeof(T) == 1) {
                val = machine->cpu.bus->read8(machine->cpu.bus->ctx, pa);
            } else if constexpr (sizeof(T) == 2) {
                val = machine->cpu.bus->read16(machine->cpu.bus->ctx, pa);
            } else if constexpr (sizeof(T) == 4) {
                val = machine->cpu.bus->read32(machine->cpu.bus->ctx, pa);
            } else if constexpr (sizeof(T) == 8) {
                uint32_t lo = machine->cpu.bus->read32(machine->cpu.bus->ctx, pa);
                uint32_t hi = machine->cpu.bus->read32(machine->cpu.bus->ctx, pa + 4);
                val = (static_cast<uint64_t>(hi) << 32) | lo;
            }
        }

        if (machine->level_dirty) {
            jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        }
        return val;
    }

    template <typename T>
    void WriteMemory(Dynarmic::A32::VAddr vaddr, T val) {
        bool priv = (machine->cpu.cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;

        if constexpr (sizeof(T) > 1) {
            if (((vaddr & 0xfffu) + sizeof(T)) > 0x1000u) {
                for (size_t i = 0; i < sizeof(T); ++i) {
                    uint32_t pa = 0;
                    uint32_t a = vaddr + static_cast<uint32_t>(i);
                    uint32_t fsr = arm_mmu_translate(&machine->cpu, a, ARM_ACCESS_WRITE, priv, &pa);
                    if (fsr != 0) {
                        abort_taken = true;
                        abort_fsr = fsr;
                        abort_far = a;
                        jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
                        return;
                    }
                    uint8_t b = static_cast<uint8_t>((val >> (8 * i)) & 0xff);
                    if (pa >= machine->ram_base && pa < machine->ram_base + machine->ram_size) {
                        if (machine->ram_watch) {
                            s5l8900_ram_changed(machine, pa, 1);
                        }
                        machine->ram[pa - machine->ram_base] = b;
                    } else if (machine->cpu.bus && machine->cpu.bus->write8) {
                        machine->cpu.bus->write8(machine->cpu.bus->ctx, pa, b);
                    }
                }
                return;
            }
        }

        uint32_t pa = 0;
        uint32_t fsr = arm_mmu_translate(&machine->cpu, vaddr, ARM_ACCESS_WRITE, priv, &pa);
        if (fsr != 0) {
            abort_taken = true;
            abort_fsr = fsr;
            abort_far = vaddr;
            jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
            return;
        }

        if (pa >= machine->ram_base && pa + sizeof(T) <= machine->ram_base + machine->ram_size) {
            if (machine->ram_watch) {
                s5l8900_ram_changed(machine, pa, sizeof(T));
            }
            std::memcpy(&machine->ram[pa - machine->ram_base], &val, sizeof(T));
            return;
        }

        if (machine->cpu.bus) {
            if constexpr (sizeof(T) == 1) {
                machine->cpu.bus->write8(machine->cpu.bus->ctx, pa, static_cast<uint8_t>(val));
            } else if constexpr (sizeof(T) == 2) {
                machine->cpu.bus->write16(machine->cpu.bus->ctx, pa, static_cast<uint16_t>(val));
            } else if constexpr (sizeof(T) == 4) {
                machine->cpu.bus->write32(machine->cpu.bus->ctx, pa, static_cast<uint32_t>(val));
            } else if constexpr (sizeof(T) == 8) {
                machine->cpu.bus->write32(machine->cpu.bus->ctx, pa, static_cast<uint32_t>(val));
                machine->cpu.bus->write32(machine->cpu.bus->ctx, pa + 4, static_cast<uint32_t>(val >> 32));
            }
        }

        if (machine->level_dirty) {
            jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        }
    }
};

struct dynarmic_jit {
    s5l8900_t* machine = nullptr;
    std::shared_ptr<DynarmicCP15> cp15;
    std::unique_ptr<DynarmicCallbacks> callbacks;
    std::unique_ptr<Dynarmic::ExclusiveMonitor> monitor;
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    uint32_t last_ttbr0 = 0;
    uint32_t last_context_id = 0;
    uint64_t last_tlb_flushes = 0;
};

extern "C" dynarmic_jit_t *dynarmic_jit_create(s5l8900_t *machine) {
    if (!machine) return nullptr;

    auto state = std::make_unique<dynarmic_jit>();
    state->machine = machine;
    state->cp15 = std::make_shared<DynarmicCP15>();
    state->cp15->machine = machine;
    state->callbacks = std::make_unique<DynarmicCallbacks>();
    state->callbacks->machine = machine;
    state->monitor = std::make_unique<Dynarmic::ExclusiveMonitor>(1);

    Dynarmic::A32::UserConfig conf;
    conf.callbacks = state->callbacks.get();
    conf.coprocessors[15] = state->cp15;
    conf.arch_version = Dynarmic::A32::ArchVersion::v6K;
    conf.optimizations = Dynarmic::all_safe_optimizations;
    conf.enable_cycle_counting = true;
    conf.always_little_endian = true;
    conf.check_halt_on_memory_access = true;
    conf.hook_isb = true;
    conf.global_monitor = state->monitor.get();
    conf.code_cache_size = 64 * 1024 * 1024;

    try {
        state->jit = std::make_unique<Dynarmic::A32::Jit>(conf);
        state->callbacks->jit = state->jit.get();
        state->cp15->jit = state->jit.get();
    } catch (...) {
        return nullptr;
    }

    state->last_ttbr0 = machine->cpu.cp15.ttbr0;
    state->last_context_id = machine->cpu.cp15.context_id;
    state->last_tlb_flushes = machine->cpu.tlb_flushes;

    return state.release();
}

extern "C" void dynarmic_jit_destroy(dynarmic_jit_t *jit) {
    delete jit;
}

extern "C" void dynarmic_jit_clear_cache(dynarmic_jit_t *jit) {
    if (jit && jit->jit) {
        jit->jit->ClearCache();
    }
}

extern "C" void dynarmic_jit_invalidate_range(dynarmic_jit_t *jit, uint32_t start_addr, size_t length) {
    if (jit && jit->jit) {
        jit->jit->InvalidateCacheRange(start_addr, length);
    }
}

extern "C" uint32_t dynarmic_jit_run(dynarmic_jit_t *jit_wrapper, uint32_t max_ticks) {
    if (!jit_wrapper || !jit_wrapper->jit || !jit_wrapper->machine) return 0;

    s5l8900_t* machine = jit_wrapper->machine;
    auto& jit = *jit_wrapper->jit;
    auto& cb = *jit_wrapper->callbacks;

    if (machine->cpu.cp15.ttbr0 != jit_wrapper->last_ttbr0 ||
        machine->cpu.cp15.context_id != jit_wrapper->last_context_id ||
        machine->cpu.tlb_flushes != jit_wrapper->last_tlb_flushes) {
        jit.ClearCache();
        jit_wrapper->last_ttbr0 = machine->cpu.cp15.ttbr0;
        jit_wrapper->last_context_id = machine->cpu.cp15.context_id;
        jit_wrapper->last_tlb_flushes = machine->cpu.tlb_flushes;
    }

    auto& regs = jit.Regs();
    for (int i = 0; i < 16; ++i) regs[i] = machine->cpu.r[i];
    jit.SetCpsr(machine->cpu.cpsr);
    auto& ext = jit.ExtRegs();
    for (int i = 0; i < 32; ++i) ext[i] = machine->cpu.vfp_s[i];
    jit.SetFpscr(machine->cpu.vfp_fpscr);

    cb.ticks_remaining = max_ticks;
    cb.ticks_executed = 0;
    cb.svc_taken = false;
    cb.svc_swi = 0;
    cb.exception_taken = false;
    cb.exception_pc = 0;
    cb.abort_taken = false;
    cb.abort_fsr = 0;
    cb.abort_far = 0;

    jit.ClearHalt(Dynarmic::HaltReason::UserDefined1 |
                  Dynarmic::HaltReason::UserDefined2 |
                  Dynarmic::HaltReason::UserDefined3 |
                  Dynarmic::HaltReason::Step |
                  Dynarmic::HaltReason::MemoryAbort |
                  Dynarmic::HaltReason::CacheInvalidation);

    jit.Run();

    if (cb.svc_taken) {
        cb.svc_taken = false;
        uint32_t saved_cpsr = jit.Cpsr();
        bool is_thumb = (saved_cpsr & ARM_CPSR_T) != 0;
        uint32_t ret_addr = regs[15];

        for (int i = 0; i < 15; ++i) machine->cpu.r[i] = regs[i];
        machine->cpu.cpsr = saved_cpsr;
        for (int i = 0; i < 32; ++i) machine->cpu.vfp_s[i] = ext[i];
        machine->cpu.vfp_fpscr = jit.Fpscr();

        if (machine->cpu.bus && machine->cpu.bus->user_svc_observer) {
            uint32_t svc_pc = ret_addr - (is_thumb ? 2u : 4u);
            machine->cpu.bus->user_svc_observer(machine->cpu.bus->user_svc_ctx, &machine->cpu, svc_pc, cb.svc_swi);
        }

        arm_set_mode(&machine->cpu, ARM_MODE_SVC);
        machine->cpu.spsr[arm_bank_of_mode(ARM_MODE_SVC)] = saved_cpsr;
        machine->cpu.r[14] = ret_addr;
        machine->cpu.cpsr |= ARM_CPSR_I;
        machine->cpu.cpsr &= ~ARM_CPSR_T;
        if (machine->cpu.cp15.sctlr & ARM_SCTLR_EE) {
            machine->cpu.cpsr |= ARM_CPSR_E;
        } else {
            machine->cpu.cpsr &= ~ARM_CPSR_E;
        }
        machine->cpu.excl_valid = false;
        if (jit_wrapper->monitor) {
            jit_wrapper->monitor->ClearProcessor(0);
        }
        uint32_t vector = (machine->cpu.cp15.sctlr & ARM_SCTLR_V) ? 0xFFFF0008u : 0x00000008u;
        machine->cpu.r[15] = vector;
    } else if (cb.abort_taken) {
        cb.abort_taken = false;
        for (int i = 0; i < 15; ++i) machine->cpu.r[i] = regs[i];
        machine->cpu.r[15] = regs[15];
        machine->cpu.cpsr = jit.Cpsr();
        for (int i = 0; i < 32; ++i) machine->cpu.vfp_s[i] = ext[i];
        machine->cpu.vfp_fpscr = jit.Fpscr();
        if (jit_wrapper->monitor) {
            jit_wrapper->monitor->ClearProcessor(0);
        }
        arm_step(&machine->cpu);
        cb.ticks_executed++;
    } else if (cb.exception_taken) {
        cb.exception_taken = false;
        for (int i = 0; i < 15; ++i) machine->cpu.r[i] = regs[i];
        machine->cpu.r[15] = cb.exception_pc;
        machine->cpu.cpsr = jit.Cpsr();
        for (int i = 0; i < 32; ++i) machine->cpu.vfp_s[i] = ext[i];
        machine->cpu.vfp_fpscr = jit.Fpscr();
        arm_step(&machine->cpu);
        cb.ticks_executed++;
    } else {
        for (int i = 0; i < 16; ++i) machine->cpu.r[i] = regs[i];
        machine->cpu.cpsr = jit.Cpsr();
        for (int i = 0; i < 32; ++i) machine->cpu.vfp_s[i] = ext[i];
        machine->cpu.vfp_fpscr = jit.Fpscr();
    }

    machine->cpu.cycles += cb.ticks_executed;
    return static_cast<uint32_t>(cb.ticks_executed);
}

static uint32_t shim_run(void *jit, uint32_t max_ticks) {
    return dynarmic_jit_run(static_cast<dynarmic_jit_t*>(jit), max_ticks);
}

static void shim_clear_cache(void *jit) {
    dynarmic_jit_clear_cache(static_cast<dynarmic_jit_t*>(jit));
}

static void shim_destroy(void *jit) {
    dynarmic_jit_destroy(static_cast<dynarmic_jit_t*>(jit));
}

extern "C" const s5l_jit_ops_t g_dynarmic_jit_ops = {
    shim_run,
    shim_clear_cache,
    shim_destroy,
};
