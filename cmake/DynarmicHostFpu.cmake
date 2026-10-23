# The x64 dispatcher can compile a new block while guest MXCSR is active.
# Host constant folding must preserve guest rounding and exception flags.
# The VMSR -> overlapping VFP vector integration test covers this boundary.
function(s5l_patch_dynarmic_host_fpu source_dir)
    set(path "${source_dir}/src/dynarmic/backend/x64/a32_interface.cpp")
    file(READ "${path}" source)
    if(source MATCHES "S5LWinBox: isolate host compilation from guest MXCSR")
        return()
    endif()
    set(anchor "        constexpr size_t MINIMUM_REMAINING_CODESIZE = 1 * 1024 * 1024;")
    string(FIND "${source}" "${anchor}" found)
    if(found LESS 0)
        message(FATAL_ERROR "Dynarmic's x64 compilation entry changed; update the host-FPU patch.")
    endif()
    string(REPLACE "#include <mutex>" "#include <mutex>\n#include <xmmintrin.h>" source "${source}")
    string(REPLACE "${anchor}"
        "        // S5LWinBox: isolate host compilation from guest MXCSR.\n        const u32 s5l_saved_mxcsr = _mm_getcsr();\n        _mm_setcsr(0x1f80);\n        SCOPE_EXIT { _mm_setcsr(s5l_saved_mxcsr); };\n\n${anchor}"
        source "${source}")
    file(WRITE "${path}" "${source}")
endfunction()
