/* bios_interp_dispatch.c -- top-level dispatcher for interpreted BIOS ROMs.
 *
 * Targets opting into PSXRT_BIOS_INTERPRETER do not link a generated BIOS
 * translation. BIOS ROM and relocated BIOS RAM execute through
 * dirty_ram_interp; once the BIOS reaches a clean game entry, the same path
 * selects the game's statically recompiled dispatcher.
 */

#include "cpu_state.h"
#include "dirty_ram_interp.h"
#include "fntrace.h"

#include <stdint.h>

extern void psx_unknown_dispatch(CPUState* cpu, uint32_t addr, uint32_t phys);
extern void psx_check_interrupts(CPUState* cpu);
extern void psx_check_interrupts_at(CPUState* cpu, uint32_t resume_pc);

/* memory.c's kernel-bless table is meaningful only for a generated BIOS. Keep
 * the ABI available but empty in interpreter mode. */
typedef struct {
    uint32_t key;
    uint32_t body_lo;
    uint32_t body_hi;
} PsxKernelBody;

const PsxKernelBody psx_bios_kernel_bodies[1] = {{0u, 0u, 0u}};
const uint32_t psx_bios_kernel_body_count = 0u;

int g_psx_dispatch_depth = 0;

static void psx_dispatch_check_return_boundary(CPUState* cpu,
                                               uint32_t stop_addr) {
    if (stop_addr != 0u) {
        psx_check_interrupts_at(cpu, stop_addr);
        if (((cpu->pc ^ stop_addr) & 0x1FFFFFFFu) == 0u) cpu->pc = 0u;
    } else {
        psx_check_interrupts(cpu);
    }
}

static void psx_dispatch_impl(CPUState* cpu, uint32_t addr,
                              uint32_t stop_addr) {
    const int outermost = (g_psx_dispatch_depth++ == 0);
    const uint32_t sp_at_call = cpu->gpr[29];

    for (;;) {
        fntrace_record(cpu, addr);
        cpu->pc = 0u;

        if (!dirty_ram_dispatch(cpu, addr, stop_addr)) {
            psx_unknown_dispatch(cpu, addr, addr & 0x1FFFFFFFu);
        }

        psx_rfe_escape_check(cpu);
        if (g_psx_call_bail) {
            if (stop_addr != 0u &&
                ((cpu->pc ^ stop_addr) & 0x1FFFFFFFu) == 0u &&
                cpu->gpr[29] == sp_at_call) {
                g_psx_call_bail = 0;
                g_psx_bail_resolved++;
                cpu->pc = 0u;
                --g_psx_dispatch_depth;
                if (outermost)
                    psx_dispatch_check_return_boundary(cpu, stop_addr);
                return;
            }
            if (!outermost) {
                --g_psx_dispatch_depth;
                return;
            }
            g_psx_call_bail = 0;
            g_psx_bail_flattened++;
            addr = cpu->pc;
            continue;
        }

        if (cpu->pc == 0u) {
            if (stop_addr != 0u &&
                (cpu->gpr[29] != sp_at_call ||
                 ((cpu->gpr[31] ^ stop_addr) & 0x1FFFFFFFu) != 0u)) {
                g_psx_call_bail = 1;
                g_psx_bail_first++;
                cpu->pc = cpu->gpr[31];
                if (outermost) {
                    g_psx_call_bail = 0;
                    g_psx_bail_flattened++;
                    addr = cpu->pc;
                    continue;
                }
                --g_psx_dispatch_depth;
                return;
            }
            --g_psx_dispatch_depth;
            if (outermost)
                psx_dispatch_check_return_boundary(cpu, stop_addr);
            return;
        }

        if (stop_addr != 0u && cpu->pc == stop_addr) {
            if (cpu->gpr[29] != sp_at_call) {
                addr = cpu->pc;
                continue;
            }
            cpu->pc = 0u;
            --g_psx_dispatch_depth;
            if (outermost)
                psx_dispatch_check_return_boundary(cpu, stop_addr);
            return;
        }

        addr = cpu->pc;
    }
}

void psx_dispatch(CPUState* cpu, uint32_t addr) {
    psx_dispatch_impl(cpu, addr, 0u);
}

void psx_dispatch_call(CPUState* cpu, uint32_t addr, uint32_t return_addr) {
    psx_dispatch_impl(cpu, addr, return_addr);
}

__attribute__((constructor)) static void psx_cps_mark_bios_interpreter(void) {
    extern int g_psx_cps_mode;
    g_psx_cps_mode = 1;
}
