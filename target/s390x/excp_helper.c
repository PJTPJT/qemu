/*
 * s390x exception / interrupt helpers
 *
 *  Copyright (c) 2009 Ulrich Hecht
 *  Copyright (c) 2011 Alexander Graf
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "internal.h"
#include "qemu/timer.h"
#include "exec/exec-all.h"
#include "exec/cpu_ldst.h"
#include "hw/s390x/ioinst.h"
#include "exec/address-spaces.h"
#ifndef CONFIG_USER_ONLY
#include "sysemu/sysemu.h"
#include "hw/s390x/s390_flic.h"
#endif

/* #define DEBUG_S390 */
/* #define DEBUG_S390_STDOUT */

#ifdef DEBUG_S390
#ifdef DEBUG_S390_STDOUT
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); \
         if (qemu_log_separate()) { qemu_log(fmt, ##__VA_ARGS__); } } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { qemu_log(fmt, ## __VA_ARGS__); } while (0)
#endif
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#if defined(CONFIG_USER_ONLY)

void s390_cpu_do_interrupt(CPUState *cs)
{
    cs->exception_index = -1;
}

int s390_cpu_handle_mmu_fault(CPUState *cs, vaddr address, int size,
                              int rw, int mmu_idx)
{
    S390CPU *cpu = S390_CPU(cs);

    trigger_pgm_exception(&cpu->env, PGM_ADDRESSING, ILEN_AUTO);
    /* On real machines this value is dropped into LowMem.  Since this
       is userland, simply put this someplace that cpu_loop can find it.  */
    cpu->env.__excp_addr = address;
    return 1;
}

#else /* !CONFIG_USER_ONLY */

static inline uint64_t cpu_mmu_idx_to_asc(int mmu_idx)
{
    switch (mmu_idx) {
    case MMU_PRIMARY_IDX:
        return PSW_ASC_PRIMARY;
    case MMU_SECONDARY_IDX:
        return PSW_ASC_SECONDARY;
    case MMU_HOME_IDX:
        return PSW_ASC_HOME;
    default:
        abort();
    }
}

int s390_cpu_handle_mmu_fault(CPUState *cs, vaddr orig_vaddr, int size,
                              int rw, int mmu_idx)
{
    S390CPU *cpu = S390_CPU(cs);
    CPUS390XState *env = &cpu->env;
    target_ulong vaddr, raddr;
    uint64_t asc;
    int prot;

    DPRINTF("%s: address 0x%" VADDR_PRIx " rw %d mmu_idx %d\n",
            __func__, orig_vaddr, rw, mmu_idx);

    vaddr = orig_vaddr;

    if (mmu_idx < MMU_REAL_IDX) {
        asc = cpu_mmu_idx_to_asc(mmu_idx);
        /* 31-Bit mode */
        if (!(env->psw.mask & PSW_MASK_64)) {
            vaddr &= 0x7fffffff;
        }
        if (mmu_translate(env, vaddr, rw, asc, &raddr, &prot, true)) {
            return 1;
        }
    } else if (mmu_idx == MMU_REAL_IDX) {
        /* 31-Bit mode */
        if (!(env->psw.mask & PSW_MASK_64)) {
            vaddr &= 0x7fffffff;
        }
        if (mmu_translate_real(env, vaddr, rw, &raddr, &prot)) {
            return 1;
        }
    } else {
        abort();
    }

    /* check out of RAM access */
    if (!address_space_access_valid(&address_space_memory, raddr,
                                    TARGET_PAGE_SIZE, rw,
                                    MEMTXATTRS_UNSPECIFIED)) {
        DPRINTF("%s: raddr %" PRIx64 " > ram_size %" PRIx64 "\n", __func__,
                (uint64_t)raddr, (uint64_t)ram_size);
        trigger_pgm_exception(env, PGM_ADDRESSING, ILEN_AUTO);
        return 1;
    }

    qemu_log_mask(CPU_LOG_MMU, "%s: set tlb %" PRIx64 " -> %" PRIx64 " (%x)\n",
            __func__, (uint64_t)vaddr, (uint64_t)raddr, prot);

    tlb_set_page(cs, orig_vaddr & TARGET_PAGE_MASK, raddr, prot,
                 mmu_idx, TARGET_PAGE_SIZE);

    return 0;
}

static void do_program_interrupt(CPUS390XState *env)
{
    uint64_t mask, addr;
    LowCore *lowcore;
    int ilen = env->int_pgm_ilen;

    if (ilen == ILEN_AUTO) {
        ilen = get_ilen(cpu_ldub_code(env, env->psw.addr));
    }
    assert(ilen == 2 || ilen == 4 || ilen == 6);

    switch (env->int_pgm_code) {
    case PGM_PER:
        if (env->per_perc_atmid & PER_CODE_EVENT_NULLIFICATION) {
            break;
        }
        /* FALL THROUGH */
    case PGM_OPERATION:
    case PGM_PRIVILEGED:
    case PGM_EXECUTE:
    case PGM_PROTECTION:
    case PGM_ADDRESSING:
    case PGM_SPECIFICATION:
    case PGM_DATA:
    case PGM_FIXPT_OVERFLOW:
    case PGM_FIXPT_DIVIDE:
    case PGM_DEC_OVERFLOW:
    case PGM_DEC_DIVIDE:
    case PGM_HFP_EXP_OVERFLOW:
    case PGM_HFP_EXP_UNDERFLOW:
    case PGM_HFP_SIGNIFICANCE:
    case PGM_HFP_DIVIDE:
    case PGM_TRANS_SPEC:
    case PGM_SPECIAL_OP:
    case PGM_OPERAND:
    case PGM_HFP_SQRT:
    case PGM_PC_TRANS_SPEC:
    case PGM_ALET_SPEC:
    case PGM_MONITOR:
        /* advance the PSW if our exception is not nullifying */
        env->psw.addr += ilen;
        break;
    }

    qemu_log_mask(CPU_LOG_INT, "%s: code=0x%x ilen=%d\n",
                  __func__, env->int_pgm_code, ilen);

    lowcore = cpu_map_lowcore(env);

    /* Signal PER events with the exception.  */
    if (env->per_perc_atmid) {
        env->int_pgm_code |= PGM_PER;
        lowcore->per_address = cpu_to_be64(env->per_address);
        lowcore->per_perc_atmid = cpu_to_be16(env->per_perc_atmid);
        env->per_perc_atmid = 0;
    }

    lowcore->pgm_ilen = cpu_to_be16(ilen);
    lowcore->pgm_code = cpu_to_be16(env->int_pgm_code);
    lowcore->program_old_psw.mask = cpu_to_be64(get_psw_mask(env));
    lowcore->program_old_psw.addr = cpu_to_be64(env->psw.addr);
    mask = be64_to_cpu(lowcore->program_new_psw.mask);
    addr = be64_to_cpu(lowcore->program_new_psw.addr);
    lowcore->per_breaking_event_addr = cpu_to_be64(env->gbea);

    cpu_unmap_lowcore(lowcore);

    DPRINTF("%s: %x %x %" PRIx64 " %" PRIx64 "\n", __func__,
            env->int_pgm_code, ilen, env->psw.mask,
            env->psw.addr);

    load_psw(env, mask, addr);
}

static void do_svc_interrupt(CPUS390XState *env)
{
    uint64_t mask, addr;
    LowCore *lowcore;

    lowcore = cpu_map_lowcore(env);

    lowcore->svc_code = cpu_to_be16(env->int_svc_code);
    lowcore->svc_ilen = cpu_to_be16(env->int_svc_ilen);
    lowcore->svc_old_psw.mask = cpu_to_be64(get_psw_mask(env));
    lowcore->svc_old_psw.addr = cpu_to_be64(env->psw.addr + env->int_svc_ilen);
    mask = be64_to_cpu(lowcore->svc_new_psw.mask);
    addr = be64_to_cpu(lowcore->svc_new_psw.addr);

    cpu_unmap_lowcore(lowcore);

    load_psw(env, mask, addr);

    /* When a PER event is pending, the PER exception has to happen
       immediately after the SERVICE CALL one.  */
    if (env->per_perc_atmid) {
        env->int_pgm_code = PGM_PER;
        env->int_pgm_ilen = env->int_svc_ilen;
        do_program_interrupt(env);
    }
}

#define VIRTIO_SUBCODE_64 0x0D00

static void do_ext_interrupt(CPUS390XState *env)
{
    QEMUS390FLICState *flic = QEMU_S390_FLIC(s390_get_flic());
    S390CPU *cpu = s390_env_get_cpu(env);
    uint64_t mask, addr;
    uint16_t cpu_addr;
    LowCore *lowcore;

    if (!(env->psw.mask & PSW_MASK_EXT)) {
        cpu_abort(CPU(cpu), "Ext int w/o ext mask\n");
    }

    lowcore = cpu_map_lowcore(env);

    if ((env->pending_int & INTERRUPT_EMERGENCY_SIGNAL) &&
        (env->cregs[0] & CR0_EMERGENCY_SIGNAL_SC)) {
        lowcore->ext_int_code = cpu_to_be16(EXT_EMERGENCY);
        cpu_addr = find_first_bit(env->emergency_signals, S390_MAX_CPUS);
        g_assert(cpu_addr < S390_MAX_CPUS);
        lowcore->cpu_addr = cpu_to_be16(cpu_addr);
        clear_bit(cpu_addr, env->emergency_signals);
        if (bitmap_empty(env->emergency_signals, max_cpus)) {
            env->pending_int &= ~INTERRUPT_EMERGENCY_SIGNAL;
        }
    } else if ((env->pending_int & INTERRUPT_EXTERNAL_CALL) &&
               (env->cregs[0] & CR0_EXTERNAL_CALL_SC)) {
        lowcore->ext_int_code = cpu_to_be16(EXT_EXTERNAL_CALL);
        lowcore->cpu_addr = cpu_to_be16(env->external_call_addr);
        env->pending_int &= ~INTERRUPT_EXTERNAL_CALL;
    } else if ((env->pending_int & INTERRUPT_EXT_CLOCK_COMPARATOR) &&
               (env->cregs[0] & CR0_CKC_SC)) {
        lowcore->ext_int_code = cpu_to_be16(EXT_CLOCK_COMP);
        lowcore->cpu_addr = 0;
        env->pending_int &= ~INTERRUPT_EXT_CLOCK_COMPARATOR;
    } else if ((env->pending_int & INTERRUPT_EXT_CPU_TIMER) &&
               (env->cregs[0] & CR0_CPU_TIMER_SC)) {
        lowcore->ext_int_code = cpu_to_be16(EXT_CPU_TIMER);
        lowcore->cpu_addr = 0;
        env->pending_int &= ~INTERRUPT_EXT_CPU_TIMER;
    } else if (qemu_s390_flic_has_service(flic) &&
               (env->cregs[0] & CR0_SERVICE_SC)) {
        uint32_t param;

        param = qemu_s390_flic_dequeue_service(flic);
        lowcore->ext_int_code = cpu_to_be16(EXT_SERVICE);
        lowcore->ext_params = cpu_to_be32(param);
        lowcore->cpu_addr = 0;
    } else {
        g_assert_not_reached();
    }

    mask = be64_to_cpu(lowcore->external_new_psw.mask);
    addr = be64_to_cpu(lowcore->external_new_psw.addr);
    lowcore->external_old_psw.mask = cpu_to_be64(get_psw_mask(env));
    lowcore->external_old_psw.addr = cpu_to_be64(env->psw.addr);

    cpu_unmap_lowcore(lowcore);

    DPRINTF("%s: %" PRIx64 " %" PRIx64 "\n", __func__,
            env->psw.mask, env->psw.addr);

    load_psw(env, mask, addr);
}

static void do_io_interrupt(CPUS390XState *env)
{
    QEMUS390FLICState *flic = QEMU_S390_FLIC(s390_get_flic());
    uint64_t mask, addr;
    QEMUS390FlicIO *io;
    LowCore *lowcore;

    g_assert(env->psw.mask & PSW_MASK_IO);
    io = qemu_s390_flic_dequeue_io(flic, env->cregs[6]);
    g_assert(io);

    lowcore = cpu_map_lowcore(env);

    lowcore->subchannel_id = cpu_to_be16(io->id);
    lowcore->subchannel_nr = cpu_to_be16(io->nr);
    lowcore->io_int_parm = cpu_to_be32(io->parm);
    lowcore->io_int_word = cpu_to_be32(io->word);
    lowcore->io_old_psw.mask = cpu_to_be64(get_psw_mask(env));
    lowcore->io_old_psw.addr = cpu_to_be64(env->psw.addr);
    mask = be64_to_cpu(lowcore->io_new_psw.mask);
    addr = be64_to_cpu(lowcore->io_new_psw.addr);

    cpu_unmap_lowcore(lowcore);
    g_free(io);

    DPRINTF("%s: %" PRIx64 " %" PRIx64 "\n", __func__, env->psw.mask,
            env->psw.addr);
    load_psw(env, mask, addr);
}

static void do_mchk_interrupt(CPUS390XState *env)
{
    QEMUS390FLICState *flic = QEMU_S390_FLIC(s390_get_flic());
    uint64_t mask, addr;
    LowCore *lowcore;
    int i;

    /* for now we only support channel report machine checks (floating) */
    g_assert(env->psw.mask & PSW_MASK_MCHECK);
    g_assert(env->cregs[14] & CR14_CHANNEL_REPORT_SC);

    qemu_s390_flic_dequeue_crw_mchk(flic);

    lowcore = cpu_map_lowcore(env);

    /* we are always in z/Architecture mode */
    lowcore->ar_access_id = 1;

    for (i = 0; i < 16; i++) {
        lowcore->floating_pt_save_area[i] = cpu_to_be64(get_freg(env, i)->ll);
        lowcore->gpregs_save_area[i] = cpu_to_be64(env->regs[i]);
        lowcore->access_regs_save_area[i] = cpu_to_be32(env->aregs[i]);
        lowcore->cregs_save_area[i] = cpu_to_be64(env->cregs[i]);
    }
    lowcore->prefixreg_save_area = cpu_to_be32(env->psa);
    lowcore->fpt_creg_save_area = cpu_to_be32(env->fpc);
    lowcore->tod_progreg_save_area = cpu_to_be32(env->todpr);
    lowcore->cpu_timer_save_area = cpu_to_be64(env->cputm);
    lowcore->clock_comp_save_area = cpu_to_be64(env->ckc >> 8);

    lowcore->mcic = cpu_to_be64(s390_build_validity_mcic() | MCIC_SC_CP);
    lowcore->mcck_old_psw.mask = cpu_to_be64(get_psw_mask(env));
    lowcore->mcck_old_psw.addr = cpu_to_be64(env->psw.addr);
    mask = be64_to_cpu(lowcore->mcck_new_psw.mask);
    addr = be64_to_cpu(lowcore->mcck_new_psw.addr);

    cpu_unmap_lowcore(lowcore);

    DPRINTF("%s: %" PRIx64 " %" PRIx64 "\n", __func__,
            env->psw.mask, env->psw.addr);

    load_psw(env, mask, addr);
}

void s390_cpu_do_interrupt(CPUState *cs)
{
    QEMUS390FLICState *flic = QEMU_S390_FLIC(s390_get_flic());
    S390CPU *cpu = S390_CPU(cs);
    CPUS390XState *env = &cpu->env;
    bool stopped = false;

    qemu_log_mask(CPU_LOG_INT, "%s: %d at pc=%" PRIx64 "\n",
                  __func__, cs->exception_index, env->psw.addr);

try_deliver:
    /* handle machine checks */
    if (cs->exception_index == -1 && s390_cpu_has_mcck_int(cpu)) {
        cs->exception_index = EXCP_MCHK;
    }
    /* handle external interrupts */
    if (cs->exception_index == -1 && s390_cpu_has_ext_int(cpu)) {
        cs->exception_index = EXCP_EXT;
    }
    /* handle I/O interrupts */
    if (cs->exception_index == -1 && s390_cpu_has_io_int(cpu)) {
        cs->exception_index = EXCP_IO;
    }
    /* RESTART interrupt */
    if (cs->exception_index == -1 && s390_cpu_has_restart_int(cpu)) {
        cs->exception_index = EXCP_RESTART;
    }
    /* STOP interrupt has least priority */
    if (cs->exception_index == -1 && s390_cpu_has_stop_int(cpu)) {
        cs->exception_index = EXCP_STOP;
    }

    switch (cs->exception_index) {
    case EXCP_PGM:
        do_program_interrupt(env);
        break;
    case EXCP_SVC:
        do_svc_interrupt(env);
        break;
    case EXCP_EXT:
        do_ext_interrupt(env);
        break;
    case EXCP_IO:
        do_io_interrupt(env);
        break;
    case EXCP_MCHK:
        do_mchk_interrupt(env);
        break;
    case EXCP_RESTART:
        do_restart_interrupt(env);
        break;
    case EXCP_STOP:
        do_stop_interrupt(env);
        stopped = true;
        break;
    }

    if (cs->exception_index != -1 && !stopped) {
        /* check if there are more pending interrupts to deliver */
        cs->exception_index = -1;
        goto try_deliver;
    }
    cs->exception_index = -1;

    /* we might still have pending interrupts, but not deliverable */
    if (!env->pending_int && !qemu_s390_flic_has_any(flic)) {
        cs->interrupt_request &= ~CPU_INTERRUPT_HARD;
    }

    /* WAIT PSW during interrupt injection or STOP interrupt */
    if ((env->psw.mask & PSW_MASK_WAIT) || stopped) {
        /* don't trigger a cpu_loop_exit(), use an interrupt instead */
        cpu_interrupt(CPU(cpu), CPU_INTERRUPT_HALT);
    } else if (cs->halted) {
        /* unhalt if we had a WAIT PSW somehwere in our injection chain */
        s390_cpu_unhalt(cpu);
    }
}

bool s390_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    if (interrupt_request & CPU_INTERRUPT_HARD) {
        S390CPU *cpu = S390_CPU(cs);
        CPUS390XState *env = &cpu->env;

        if (env->ex_value) {
            /* Execution of the target insn is indivisible from
               the parent EXECUTE insn.  */
            return false;
        }
        if (s390_cpu_has_int(cpu)) {
            s390_cpu_do_interrupt(cs);
            return true;
        }
        if (env->psw.mask & PSW_MASK_WAIT) {
            /* Woken up because of a floating interrupt but it has already
             * been delivered. Go back to sleep. */
            cpu_interrupt(CPU(cpu), CPU_INTERRUPT_HALT);
        }
    }
    return false;
}

void s390x_cpu_debug_excp_handler(CPUState *cs)
{
    S390CPU *cpu = S390_CPU(cs);
    CPUS390XState *env = &cpu->env;
    CPUWatchpoint *wp_hit = cs->watchpoint_hit;

    if (wp_hit && wp_hit->flags & BP_CPU) {
        /* FIXME: When the storage-alteration-space control bit is set,
           the exception should only be triggered if the memory access
           is done using an address space with the storage-alteration-event
           bit set.  We have no way to detect that with the current
           watchpoint code.  */
        cs->watchpoint_hit = NULL;

        env->per_address = env->psw.addr;
        env->per_perc_atmid |= PER_CODE_EVENT_STORE | get_per_atmid(env);
        /* FIXME: We currently no way to detect the address space used
           to trigger the watchpoint.  For now just consider it is the
           current default ASC. This turn to be true except when MVCP
           and MVCS instrutions are not used.  */
        env->per_perc_atmid |= env->psw.mask & (PSW_MASK_ASC) >> 46;

        /* Remove all watchpoints to re-execute the code.  A PER exception
           will be triggered, it will call load_psw which will recompute
           the watchpoints.  */
        cpu_watchpoint_remove_all(cs, BP_CPU);
        cpu_loop_exit_noexc(cs);
    }
}

/* Unaligned accesses are only diagnosed with MO_ALIGN.  At the moment,
   this is only for the atomic operations, for which we want to raise a
   specification exception.  */
void s390x_cpu_do_unaligned_access(CPUState *cs, vaddr addr,
                                   MMUAccessType access_type,
                                   int mmu_idx, uintptr_t retaddr)
{
    S390CPU *cpu = S390_CPU(cs);
    CPUS390XState *env = &cpu->env;

    s390_program_interrupt(env, PGM_SPECIFICATION, ILEN_AUTO, retaddr);
}

#endif /* CONFIG_USER_ONLY */
