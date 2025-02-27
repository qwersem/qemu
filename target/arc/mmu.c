/*
 * QEMU ARC CPU
 *
 * Copyright (c) 2020 Synppsys Inc.
 * Contributed by Cupertino Miranda <cmiranda@synopsys.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * http://www.gnu.org/licenses/lgpl-2.1.html
 */

#include "qemu/osdep.h"
#include "target/arc/regs.h"
//#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "qemu/log.h"
#include "mmu.h"

unsigned char mmu_v3_page_size = 13;

target_ulong
arc_mmu_aux_get(const struct arc_aux_reg_detail *aux_reg_detail, void *data)
{
    CPUARCState *env = (CPUARCState *) data;
    struct arc_mmu *mmu = &env->mmu.v3;
    uint32_t reg = 0;

    switch (aux_reg_detail->id) {
    case AUX_ID_mmu_build:
        /*
         * For now hardcode the TLB geometry and canonical page sizes
         * MMUv4: 2M Super Page, 8k Page, 4 way set associative,
         *        1K entries (256x4), 4 uITLB, 8 uDTLB
         */
        reg = 0x04e21a4a;
	reg &= ~(0xf << 15); /* PGSZ0 */
	reg |= ((mmu_v3_page_size - 9) << 15); /* 4K page size */
        break;
    case AUX_ID_tlbindex:
        reg = mmu->tlbindex;
        break;
    case AUX_ID_tlbpd0:
        reg = mmu->tlbpd0;
        break;
    case AUX_ID_tlbpd1:
        reg = mmu->tlbpd1;
        break;
    case AUX_ID_tlbpd1_hi:
        reg = mmu->tlbpd1_hi;
        break;
    case AUX_ID_scratch_data0:
        reg = mmu->scratch_data0;
        break;
    case AUX_ID_tlbcommand:
        reg = mmu->tlbcmd;
        break;
    case AUX_ID_pid:
        reg = (mmu->enabled << 31) | mmu->pid_asid;
        break;
    case AUX_ID_sasid0:
        reg = mmu->sasid0;
        break;
    case AUX_ID_sasid1:
        reg = mmu->sasid1;
        break;
    default:
        break;
    }

    return reg;
}

void
arc_mmu_aux_set(const struct arc_aux_reg_detail *aux_reg_detail,
                target_ulong val, void *data)
{
    CPUARCState *env = (CPUARCState *) data;
    CPUState *cs = env_cpu(env);
    struct arc_mmu *mmu = &env->mmu.v3;

    switch (aux_reg_detail->id) {
    /* AUX_ID_tlbcommand is more involved and handled seperately */
    case AUX_ID_tlbindex:
        mmu->tlbindex = val;
        break;
    case AUX_ID_tlbpd0:
        mmu->tlbpd0 = val;
        break;
    case AUX_ID_tlbpd1:
        mmu->tlbpd1 = val;
        break;
    case AUX_ID_tlbpd1_hi:
        mmu->tlbpd1_hi = val;
        break;
    case AUX_ID_scratch_data0:
        mmu->scratch_data0 = val;
        break;
    case AUX_ID_pid:
        qemu_log_mask(CPU_LOG_MMU,
                      "[MMU] Writing PID_ASID with value 0x" TARGET_FMT_lx
                      " at 0x" TARGET_FMT_lx "\n",
                      val, env->pc);
        mmu->enabled = (val >> 31);
        mmu->pid_asid = val & 0xff;
        tlb_flush(cs);
        break;
    case AUX_ID_sasid0:
        mmu->sasid0 = val;
        break;
    case AUX_ID_sasid1:
        mmu->sasid1 = val;
        break;
    default:
        break;
    }
}

/* vaddr can't have top bit */
#define VPN(addr) ((addr) & (MMU_V3_PAGE_MASK & (~0x80000000)))
#define PFN(addr) ((addr) & MMU_V3_PAGE_MASK)

static void
arc_mmu_debug_tlb_for_set(CPUARCState *env, int set)
{
    int j;
    bool set_printed = false;

    for (j = 0; j < N_WAYS; j++) {
        struct arc_tlb_e *tlb = &env->mmu.v3.nTLB[set][j];

        if ((tlb->pd0 & PD0_V) != 0) {
            if (set_printed == false) {
                printf("set %d\n", set);
                set_printed = true;
            }
            if (set_printed == true) {
                printf(" way %d\n", j);
            }
            printf("  tlppd0: %08x: vaddr=\t" TARGET_FMT_lx " %s %s%s asid=%02x\n",
                   tlb->pd0, VPN(tlb->pd0),
                   (char *) ((tlb->pd0 & PD0_SZ) != 0 ? "sz1" : "sz0"),
                   (char *) ((tlb->pd0 & PD0_V) != 0 ? "V" : ""),
                   (char *) ((tlb->pd0 & PD0_G) != 0 ? "g" : ""),
                   tlb->pd0 & PD0_ASID);

            printf("  tlppd1: %08x: paddr=\t%08x k:%s%s%s u:%s%s%s f:%s\n",
                   (unsigned int) tlb->pd1, (unsigned int) PFN(tlb->pd1),
                   (char *) ((tlb->pd1 & PD1_RK) != 0 ? "R" : "r"),
                   (char *) ((tlb->pd1 & PD1_WK) != 0 ? "W" : "w"),
                   (char *) ((tlb->pd1 & PD1_XK) != 0 ? "X" : "x"),
                   (char *) ((tlb->pd1 & PD1_RU) != 0 ? "R" : "r"),
                   (char *) ((tlb->pd1 & PD1_WU) != 0 ? "W" : "w"),
                   (char *) ((tlb->pd1 & PD1_XU) != 0 ? "X" : "x"),
                   (char *) ((tlb->pd1 & PD1_FC) != 0 ? "C" : "c"));
        }
    }
}

void
arc_mmu_debug_tlb(CPUARCState *env)
{
    int i;

    for (i = 0; i < N_SETS; i++) {
        arc_mmu_debug_tlb_for_set(env, i);
    }
}

void
arc_mmu_debug_tlb_for_vaddr(CPUARCState *env, uint32_t vaddr)
{
    uint32_t set = (vaddr >> MMU_V3_PAGE_BITS) & (N_SETS - 1);
    arc_mmu_debug_tlb_for_set(env, set);
}


static struct arc_tlb_e *
arc_mmu_get_tlb_at_index(uint32_t index, struct arc_mmu *mmu)
{
    uint32_t set = index / N_WAYS;
    uint32_t bank = index % N_WAYS;
    return &mmu->nTLB[set][bank];
}

static inline bool
match_sasid(struct arc_tlb_e *tlb, struct arc_mmu *mmu)
{
    /* Match to a shared library. */
    uint8_t position = tlb->pd0 & PD0_ASID_MATCH;
    uint64_t pos = 1ULL << position;
    uint64_t sasid = ((uint64_t) mmu->sasid1 << 32) | mmu->sasid0;
    if ((pos & sasid) == 0) {
        return false;
    }
    return true;
}

static struct arc_tlb_e *
arc_mmu_lookup_tlb(uint32_t vaddr, uint32_t compare_mask, struct arc_mmu *mmu,
                   int *num_finds, uint32_t *index)
{
    struct arc_tlb_e *ret = NULL;
    uint32_t set = (vaddr >> MMU_V3_PAGE_BITS) & (N_SETS - 1);
    struct arc_tlb_e *tlb = &mmu->nTLB[set][0];
    int w;

    if (num_finds != NULL) {
        *num_finds = 0;
    }

    bool general_match = true;
    for (w = 0; w < N_WAYS; w++, tlb++) {
        uint32_t match = vaddr & compare_mask;
        uint32_t final_compare_mask = compare_mask;

        if ((tlb->pd0 & PD0_G) == 0) {
            if ((tlb->pd0 & PD0_S) != 0) {
                /* Match to a shared library. */
                if (match_sasid(tlb, mmu) == false) {
                    general_match = false;
                }
            } else {
                /* Match to a process. */
                match |= mmu->pid_asid & PD0_PID_MATCH;
                final_compare_mask |= PD0_PID_MATCH;
            }
        }

        if (match == (tlb->pd0 & final_compare_mask) && general_match) {
            ret = tlb;
            if (num_finds != NULL) {
                *num_finds += 1;
            }
            if (index != NULL) {
                *index = (set * N_WAYS) + w;
            }
        }
    }

    if (ret == NULL) {
        uint32_t way = mmu->way_sel[set];
        ret = &mmu->nTLB[set][way];

        /* TODO: Replace by something more significant. */
        if (index != NULL) {
            *index = (set * N_WAYS) + way;
        }

        mmu->way_sel[set] = (mmu->way_sel[set] + 1) & (N_WAYS - 1);
    }

    return ret;
}

/*
 * TLB Insert/Delete triggered by writing the cmd to TLBCommand Aux
 *  - Requires PD0 and PD1 be setup apriori
 */
void
arc_mmu_aux_set_tlbcmd(const struct arc_aux_reg_detail *aux_reg_detail,
                       target_ulong val, void *data)
{
    CPUARCState *env = (CPUARCState *) data;
    CPUState *cs = env_cpu(env);
    struct arc_mmu *mmu = &env->mmu.v3;
    uint32_t pd0 = mmu->tlbpd0;
    uint32_t pd1 = mmu->tlbpd1;
    int num_finds = 4;
    uint32_t index;
    struct arc_tlb_e *tlb;

    mmu->tlbcmd = val;
    uint32_t matching_mask = (PD0_VPN | PD0_SZ | PD0_G | PD0_S | PD0_ASID);

    if ((pd0 & PD0_G) != 0) {
        /*
         * When Global do not check for asid match.
         */
        matching_mask &= ~(PD0_S | PD0_ASID);
    }

    /*
     * NOTE: Write and WriteNI commands are the same because we do not model
     * uTLBs in QEMU.
     */
    if (val == TLB_CMD_WRITE || val == TLB_CMD_WRITENI) {
        /*
         * TODO: Include index verification. We are always clearing the index as
         * we assume it is always valid.
         */
        tlb = arc_mmu_get_tlb_at_index(mmu->tlbindex & TLBINDEX_INDEX, mmu);
        tlb->pd0 = mmu->tlbpd0;
        tlb->pd1 = mmu->tlbpd1;

        /*
         * don't try to optimize this: upon ASID rollover the entire TLB is
         * unconditionally flushed for any ASID
         */
        tlb_flush(cs);
    }
    if (val == TLB_CMD_READ) {
        /*
         * TODO: Include index verification. We are always clearing the index as
         * we assume it is always valid.
         */

        tlb = arc_mmu_get_tlb_at_index(mmu->tlbindex & TLBINDEX_INDEX, mmu);
        mmu->tlbpd0 = tlb->pd0;
        mmu->tlbpd1 = tlb->pd1;

        mmu->tlbindex &= ~(TLBINDEX_E | TLBINDEX_RC);
    }
    if (val == TLB_CMD_DELETE || val == TLB_CMD_INSERT) {
        tlb_flush_page_by_mmuidx(cs, VPN(pd0), 3);

        if ((pd0 & PD0_G) != 0) {
            /*
             * When Global do not check for asid match.
             */
            matching_mask &= ~(PD0_S | PD0_ASID);
        }

        matching_mask &= (VPN(PD0_VPN) | (~PD0_VPN)) ;
        tlb = arc_mmu_lookup_tlb(pd0,
                                 matching_mask | PD0_V,
                                 &env->mmu.v3, &num_finds, &index);

        if (num_finds == 0) {
            mmu->tlbindex = 0x80000000; /* No entry to delete */
        } else if (num_finds == 1) {
            mmu->tlbindex = index; /* Entry is deleted set index */
            tlb->pd0 &= ~PD0_V;
            num_finds--;
            qemu_log_mask(CPU_LOG_MMU,
                          "[MMU] Delete at 0x" TARGET_FMT_lx
                          ", pd0 = 0x%08x, pd1 = 0x%08x\n",
                          env->pc, tlb->pd0, tlb->pd1);
        } else {
            while (num_finds > 0) {
                tlb->pd0 &= ~PD0_V;
                qemu_log_mask(CPU_LOG_MMU,
                              "[MMU] Delete at 0x" TARGET_FMT_lx
                              ", pd0 = 0x%08x, pd1 = 0x%08x\n",
                              env->pc, tlb->pd0, tlb->pd1);
                tlb = arc_mmu_lookup_tlb(pd0,
                                         (VPN(PD0_VPN) | PD0_V
                                          | PD0_SZ | PD0_G | PD0_S),
                                         mmu, &num_finds, NULL);
            }
        }
    }

    if (val == TLB_CMD_INSERT) {
        if ((pd0 & PD0_V) == 0) {
            mmu->tlbindex = 0x80000000;
        } else {
            tlb->pd0 = pd0;
            tlb->pd1 = pd1;

            /* Set index for latest inserted element. */
            mmu->tlbindex |= index;

            /* TODO: More verifications needed. */

            qemu_log_mask(CPU_LOG_MMU,
                          "[MMU] Insert at 0x" TARGET_FMT_lx
                          ", PID = %d, VPN = 0x" TARGET_FMT_lx ", "
                          "PFN = 0x" TARGET_FMT_lx ", pd0 = 0x%08x, pd1 = 0x%08x\n",
                          env->pc,
                          pd0 & 0xff,
                          VPN(pd0), PFN(pd1),
                          pd0, pd1);
        }
    }

    /* NOTE: We do not implement IVUTLB as we do not model uTLBs. */
    assert(val == TLB_CMD_INSERT
           || val == TLB_CMD_DELETE
           || val == TLB_CMD_WRITE
           || val == TLB_CMD_READ
           || val == TLB_CMD_WRITENI
           || val == TLB_CMD_IVUTLB
           );
}

/* Function to verify if we have permission to use MMU TLB entry. */
static bool
arc_mmu_have_permission(CPUARCState *env,
                        struct arc_tlb_e *tlb,
                        enum mmu_access_type type)
{
    bool ret = false;
    /* Read status for user mode. */
    bool in_kernel_mode = !GET_STATUS_BIT(env->stat, Uf);
    switch (type) {
    case MMU_MEM_READ:
        ret = in_kernel_mode ? tlb->pd1 & PD1_RK : tlb->pd1 & PD1_RU;
        break;
    case MMU_MEM_WRITE:
        ret = in_kernel_mode ? tlb->pd1 & PD1_WK : tlb->pd1 & PD1_WU;
        break;
    case MMU_MEM_FETCH:
        ret = in_kernel_mode ? tlb->pd1 & PD1_XK : tlb->pd1 & PD1_XU;
        break;
    case MMU_MEM_ATTOMIC:
        ret = in_kernel_mode ? tlb->pd1 & PD1_RK : tlb->pd1 & PD1_RU;
        ret = ret & (in_kernel_mode ? tlb->pd1 & PD1_WK : tlb->pd1 & PD1_WU);
        break;
    case MMU_MEM_IRRELEVANT_TYPE:
        ret = true;
        break;
    }

    return ret;
}

/* Translation function to get physical address from virtual address. */
static bool
arc_mmu_translate(CPUARCState *env,
                  hwaddr *paddr,
                  uint32_t vaddr, enum mmu_access_type rwe,
                  uint32_t *index,
                  struct mem_exception *excp)
{
    struct arc_mmu *mmu = &(env->mmu.v3);
    struct arc_tlb_e *tlb = NULL;
    int num_matching_tlb = 0;

    SET_MEM_EXCEPTION(*excp, EXCP_NO_EXCEPTION, 0, 0);

    if (rwe != MMU_MEM_IRRELEVANT_TYPE
        && GET_STATUS_BIT(env->stat, Uf) != 0 && vaddr >= 0x80000000) {
        goto protv_exception;
    }

    /*
     * Check that we are not addressing an address above 0x80000000.
     * Return the same address in that case.
     */
    if ((vaddr >= 0x80000000) || mmu->enabled == false) {
        *paddr = vaddr;
        return vaddr;
    }

    if (rwe != MMU_MEM_IRRELEVANT_TYPE) {
        qemu_log_mask(CPU_LOG_MMU,
                      "[MMU] Translate at 0x" TARGET_FMT_lx
                      ", vaddr 0x%08x, pid %d, rwe = %s\n",
                      env->pc, vaddr, mmu->pid_asid, RWE_STRING(rwe));
    }

    uint32_t match_pd0 = (VPN(vaddr) | PD0_V);
    tlb = arc_mmu_lookup_tlb(match_pd0, (VPN(PD0_VPN) | PD0_V), mmu,
                              &num_matching_tlb, index);

    /*
     * Check for multiple matches in nTLB, and return machine check
     *  exception.
     */
    if (num_matching_tlb > 1) {
        qemu_log_mask(CPU_LOG_MMU,
                      "[MMU] Machine Check exception. num_matching_tlb = %d\n",
                      num_matching_tlb);
        SET_MEM_EXCEPTION(*excp, EXCP_MACHINE_CHECK, 0x01, 0x00);
        return 0;
    }


    bool match = true;

    if (num_matching_tlb == 0) {
        match = false;
    }

    /* Check if entry if related to this address */
    if (VPN(vaddr) != VPN(tlb->pd0) || (tlb->pd0 & PD0_V) == 0) {
        /* Call the interrupt. */
        match = false;
    }

    if (match == true) {
        if ((tlb->pd0 & PD0_G) == 0) {
            if ((tlb->pd0 & PD0_S) != 0) {
                /* Match to a shared library. */
                if (match_sasid(tlb, mmu) == false) {
                    match = false;
                }
            } else if ((tlb->pd0 & PD0_PID_MATCH) !=
                       (mmu->pid_asid & PD0_PID_MATCH)) {
                /* Match to a process. */
                      match = false;
            }
        }
    }

    if (match == true && !arc_mmu_have_permission(env, tlb, rwe)) {
  protv_exception:
        qemu_log_mask(CPU_LOG_MMU,
                      "[MMU] ProtV exception at 0x" TARGET_FMT_lx
                      " for 0x%08x. rwe = %s, "
                      "tlb->pd0 = %08x, tlb->pd1 = %08x\n",
                      env->pc,
                      vaddr,
                      RWE_STRING(rwe),
                      tlb->pd0, tlb->pd1);

        SET_MEM_EXCEPTION(*excp, EXCP_PROTV, CAUSE_CODE(rwe), 0x08);
        return false;
    }

    if (match == true) {
        if (rwe != MMU_MEM_IRRELEVANT_TYPE) {
            qemu_log_mask(CPU_LOG_MMU,
                          "[MMU] Translated to 0x" TARGET_FMT_lx ", pd0=0x%08x, pd1=0x%08x\n",
                          (tlb->pd1 & MMU_V3_PAGE_MASK) | (vaddr & (~MMU_V3_PAGE_MASK)),
                          tlb->pd0, tlb->pd1);
        }
        *paddr = (tlb->pd1 & MMU_V3_PAGE_MASK) | (vaddr & (~MMU_V3_PAGE_MASK));
        return true;
    } else {
        if (rwe != MMU_MEM_IRRELEVANT_TYPE) {
            /* To remove eventually, just fail safe to check kernel. */
            if (mmu->sasid0 != 0 || mmu->sasid1 != 0) {
                assert(0);
            } else {
                mmu->tlbpd0 = (vaddr & (VPN(PD0_VPN)))
                              | PD0_V | (mmu->pid_asid & PD0_ASID);
            }
            if (rwe == MMU_MEM_FETCH) {
                qemu_log_mask(CPU_LOG_MMU,
                              "[MMU] TLB_MissI exception at 0x"
                              TARGET_FMT_lx ". rwe = %s, "
                              "vaddr = %08x, tlb->pd0 = %08x, tlb->pd1 = %08x\n",
                              env->pc,
                              RWE_STRING(rwe),
                              vaddr, tlb->pd0, tlb->pd1);
                SET_MEM_EXCEPTION(*excp, EXCP_TLB_MISS_I, 0x00, 0x00);
            } else {
                qemu_log_mask(CPU_LOG_MMU,
                              "[MMU] TLB_MissD exception at 0x" TARGET_FMT_lx
                              ". rwe = %s, "
                              "vaddr = %08x, tlb->pd0 = %08x, tlb->pd1 = %08x\n",
                              env->pc,
                              RWE_STRING(rwe),
                              vaddr, tlb->pd0, tlb->pd1);
                SET_MEM_EXCEPTION(*excp, EXCP_TLB_MISS_D, CAUSE_CODE(rwe),
                                  0x00);
            }
        } else if (rwe != MMU_MEM_IRRELEVANT_TYPE) {
            qemu_log_mask(CPU_LOG_MMU,
                          "[MMU] Failed to translate to 0x%08x\n",
                          vaddr);
        }
        return false;
    }
}

#ifndef CONFIG_USER_ONLY
static int
arc_mmu_get_prot_for_index(uint32_t index, CPUARCState *env)
{
    struct arc_tlb_e *tlb;
    int ret = 0;
    /* Read status for user mode. */
    bool in_kernel_mode = !GET_STATUS_BIT(env->stat, Uf);

    tlb = arc_mmu_get_tlb_at_index(
            index,
            &env->mmu.v3);

    if ((in_kernel_mode && (tlb->pd1 & PD1_RK) != 0)
       || (!in_kernel_mode && (tlb->pd1 & PD1_RU) != 0)) {
        ret |= PAGE_READ;
    }

    if ((in_kernel_mode && (tlb->pd1 & PD1_WK) != 0)
       || (!in_kernel_mode && (tlb->pd1 & PD1_WU) != 0)) {
        ret |= PAGE_WRITE;
    }

    if ((in_kernel_mode && (tlb->pd1 & PD1_XK) != 0)
       || (!in_kernel_mode && (tlb->pd1 & PD1_XU) != 0)) {
        ret |= PAGE_EXEC;
    }

    return ret;
}
#endif

static void QEMU_NORETURN raise_mem_exception(
        CPUState *cs, target_ulong addr, uintptr_t host_pc,
        struct mem_exception *excp)
{
    CPUARCState *env = &(ARC_CPU(cs)->env);
    if (excp->number != EXCP_TLB_MISS_I) {
        cpu_restore_state(cs, host_pc, true);
    }

    env->efa = addr;
    env->eret = env->pc;
    env->erbta = env->bta;

    cs->exception_index = excp->number;
    env->causecode = excp->causecode;
    env->param = excp->parameter;
    cpu_loop_exit(cs);
}

#ifndef CONFIG_USER_ONLY

/* MMU range */
static const uint32_t MMU_VA_START = 0x00000000;  /* inclusive */
static const uint32_t MMU_VA_END = 0x80000000;    /* exclusive */

typedef enum {
    DIRECT_ACTION,
    MPU_ACTION,
    MMU_ACTION,
    EXCEPTION_ACTION
} ACTION;

/*
 * Applying the following logic
 * ,-----.-----.-----------.---------.---------------.
 * | MMU | MPU | MMU range | mmu_idx |     action    |
 * |-----+-----+-----------+---------+---------------|
 * | dis | dis |     x     |    x    | phys = virt   |
 * |-----+-----+-----------+---------+---------------|
 * | dis | ena |     x     |    x    | mpu_translate |
 * |-----+-----+-----------+---------+---------------|
 * | ena | dis |   true    |    x    | mmu_translate |
 * |-----+-----+-----------+---------+---------------|
 * | ena | dis |   false   |    0    | phys = virt   |
 * |-----+-----+-----------+---------+---------------|
 * | ena | dis |   false   |    1    | exception     |
 * |-----+-----+-----------+---------+---------------|
 * | ena | ena |   false   |    x    | mpu_translate |
 * |-----+-----+-----------+---------+---------------|
 * | ena | ena |   true    |    x    | mmu_translate |
 * `-----^-----^-----------^---------^---------------'
 */
static int decide_action(const CPUARCState *env,
                         target_ulong       addr,
                         int                mmu_idx)
{
    static ACTION table[2][2][2][2] = { };
    static bool is_initialized;
    const bool is_user = (mmu_idx == 1);
    const bool is_mmu_range = ((addr >= MMU_VA_START) && (addr < MMU_VA_END));

    if (!is_initialized) {
        /* Both MMU and MPU disabled */
#define T true
#define F false

        table[F][F][F][F] = DIRECT_ACTION;
        table[F][F][F][T] = DIRECT_ACTION;
        table[F][F][T][F] = DIRECT_ACTION;
        table[F][F][T][T] = DIRECT_ACTION;

        /* Only MPU */
        table[F][T][F][F] = MPU_ACTION;
        table[F][T][F][T] = MPU_ACTION;
        table[F][T][T][F] = MPU_ACTION;
        table[F][T][T][T] = MPU_ACTION;

        /* Only MMU; non-mmu range; kernel access */
        table[T][F][F][F] = DIRECT_ACTION;
        /* Only MMU; non-mmu range; user access */
        table[T][F][F][T] = EXCEPTION_ACTION;

        /* Only MMU; mmu range; both modes access */
        table[T][F][T][F] = MMU_ACTION;
        table[T][F][T][T] = MMU_ACTION;

        /* Both MMU and MPU enabled; non-mmu range */
        table[T][T][F][F] = MPU_ACTION;
        table[T][T][F][T] = MPU_ACTION;

        /* Both MMU and MPU enabled; mmu range */
        table[T][T][T][F] = MMU_ACTION;
        table[T][T][T][T] = MMU_ACTION;

#undef T
#undef F

        is_initialized = true;
    }

    return table[env->mmu.v3.enabled][env->mpu.enabled][is_mmu_range][is_user];
}

#endif

void arc_mmu_init_v3(CPUARCState *env)
{
    ARCCPU *cpu = env_archcpu(env);

    env->mmu.v3.enabled = 0;
    env->mmu.v3.pid_asid = 0;
    env->mmu.v3.sasid0 = 0;
    env->mmu.v3.sasid1 = 0;

    env->mmu.v3.tlbpd0 = 0;
    env->mmu.v3.tlbpd1 = 0;
    env->mmu.v3.tlbpd1_hi = 0;
    env->mmu.v3.tlbindex = 0;
    env->mmu.v3.tlbcmd = 0;
    env->mmu.v3.scratch_data0 = 0;

    mmu_v3_page_size = cpu->cfg.mmu_page_size_sel0;
    if(mmu_v3_page_size < 12 || mmu_v3_page_size > 24) {
	assert("mmu-pagesize0 should be between 12 and 24." == 0);
    }

    memset(env->mmu.v3.nTLB, 0, sizeof(env->mmu.v3.nTLB));
}

bool
arc_get_physical_addr_v3(struct CPUState *cs, hwaddr *paddr, vaddr addr,
                  enum mmu_access_type rwe, bool probe,
                  uintptr_t retaddr)
{
#ifdef CONFIG_USER_ONLY
    *paddr = addr;
    return true;
#else
    CPUARCState *env = &((ARC_CPU(cs))->env);
    uintptr_t mmu_idx = cpu_mmu_index(env, true);
    int action = decide_action(env, addr, mmu_idx);
    struct mem_exception excp;

    switch (action) {
    case DIRECT_ACTION:
        *paddr = addr;
        return true;
        break;
    case MPU_ACTION:
        /* TODO: This is incomplete. Verify if no address translation happens on MPU */
        *paddr = addr;
        return true;
        break;
    case MMU_ACTION: {
        if(arc_mmu_translate(env, paddr, addr, rwe, NULL, &excp)) {
            return true;
        } else {
            if(probe) {
                return false;
            }
            raise_mem_exception(cs, addr, retaddr, &excp);
        }
        break;
    }
    case EXCEPTION_ACTION:
        if(probe) {
            return false;
        }
        excp.number = EXCP_PROTV;
        excp.causecode = CAUSE_CODE(rwe);
        excp.parameter = 0x08;
        raise_mem_exception(cs, addr, retaddr, &excp);
        break;
    default:
        g_assert_not_reached();
    }
#endif
}

/* Softmmu support function for MMU. */
bool arc_cpu_tlb_fill_v3(CPUState *cs, vaddr address, int size,
                      MMUAccessType access_type, int mmu_idx,
                      bool probe, uintptr_t retaddr)
{
    struct mem_exception excp;
#ifndef CONFIG_USER_ONLY
    enum mmu_access_type rwe = (char) access_type;
    CPUARCState *env = &((ARC_CPU(cs))->env);
    int action = decide_action(env, address, mmu_idx);

    switch (action) {
    case DIRECT_ACTION:
        tlb_set_page(cs, address & PAGE_MASK, address & PAGE_MASK,
                     PAGE_READ | PAGE_WRITE | PAGE_EXEC,
                     mmu_idx, PAGE_SIZE);
        break;
    case MPU_ACTION:
        if (arc_mpu_translate(env, address, access_type, mmu_idx, &excp)) {
            if (probe) {
                return false;
            }
            raise_mem_exception(cs, address, retaddr, &excp);
        }
        break;
    case MMU_ACTION: {
        uint32_t index;
        hwaddr paddr;
        if(arc_mmu_translate(env, &paddr, address, rwe, &index, &excp)) {
            int prot = arc_mmu_get_prot_for_index(index, env);
            //address = arc_mmu_page_address_for(address);
	    address = address & PAGE_MASK;
            tlb_set_page(cs, address, paddr & PAGE_MASK, prot,
                         mmu_idx, PAGE_SIZE);
        } else {
            if (probe) {
                return false;
            }
            raise_mem_exception(cs, address, retaddr, &excp);
        }
        break;
    }
    case EXCEPTION_ACTION:
        if (probe) {
            return false;
        }
        qemu_log_mask(CPU_LOG_MMU, "[MMU_TLB_FILL] ProtV "
                      "exception at 0x" TARGET_FMT_lx ". rwe = %s\n",
                      env->pc, RWE_STRING(rwe));
        excp.number = EXCP_PROTV;
        excp.causecode = CAUSE_CODE(rwe);
        excp.parameter = 0x08;
        raise_mem_exception(cs, address, retaddr, &excp);
        break;
    default:
        g_assert_not_reached();
    }

    return true;
#else /* CONFIG_USER_ONLY */
    switch (access_type) {
    case MMU_INST_FETCH:
        excp.number = EXCP_TLB_MISS_I;
        excp.causecode = 0;
        excp.parameter = 0;

        break;
    case MMU_DATA_LOAD:
    case MMU_DATA_STORE:
        excp.number = EXCP_TLB_MISS_D;
        excp.causecode = 0;
        excp.parameter = 0;
        break;
    default:
        g_assert_not_reached();
    }
    raise_mem_exception(cs, address, retaddr, &excp);
#endif /* CONFIG_USER_ONLY */
}

hwaddr arc_mmu_debug_translate_v3(CPUARCState *env, vaddr addr)
{
    hwaddr paddr = -1;
    struct mem_exception excp;
    arc_mmu_translate(env, &paddr, addr, MMU_MEM_IRRELEVANT_TYPE,
                      NULL, &excp);
    return paddr;
}

void arc_mmu_disable_v3(CPUARCState *env)
{
    env->mmu.v3.enabled = false;
}
