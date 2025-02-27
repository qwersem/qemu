/*
 * QEMU ARC CPU
 *
 * Copyright (c) 2020 Cupertino Miranda (cmiranda@synopsys.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * http://www.gnu.org/licenses/lgpl-2.1.html
 */

#include "qemu/osdep.h"
#include "target/arc/regs.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "mmu-v6.h"
#include "qemu/error-report.h"
#include "qemu/log.h"

enum mmuv6_version_enum {
  MMUV6_VERSION_INVALID = -1,
  MMUV6_52_64K,
  MMUV6_48_4K,
  MMUV6_48_16K,
  MMUV6_48_64K,
  MMUV6_32_4K,
  MMUV6_VERSION_SIZE
};

struct mmu_version_info {
  enum mmuv6_version_enum id;
  char type;
  const char nbits_per_level[4];
  const char nlevels;
  const char vaddr_size;
};

struct mmu_version_info mmuv6_info[] = {
  [MMUV6_32_4K] = {
      .id = MMUV6_32_4K,
      .type = 0,
      .nbits_per_level = { 2, 9, 9, -1 },
      .nlevels = 3,
      .vaddr_size = 32,
  },
  [MMUV6_48_4K] = {
      .id = MMUV6_48_4K,
      .type = 1,
      .nbits_per_level = { 9, 9, 9, 9 },
      .nlevels = 4,
      .vaddr_size = 48,
  },
  [MMUV6_48_16K] = {
      .id = MMUV6_48_16K,
      .type = 2,
      .nbits_per_level = { 1, 11, 11, 11 },
      .nlevels = 4,
      .vaddr_size = 48,
  },
  [MMUV6_48_64K] = {
      .id = MMUV6_48_16K,
      .type = 3,
      .nbits_per_level = { 6, 13, 13, -1 },
      .nlevels = 3,
      .vaddr_size = 48,
  },
  [MMUV6_52_64K] = {
      .id = MMUV6_52_64K,
      .type = 4,
      .nbits_per_level = { 10, 13, 13, -1 },
      .nlevels = 3,
      .vaddr_size = 52,
  },
};

struct mmu_version_info *mmu_v6_version;

#ifndef CONFIG_USER_ONLY

#define N_BITS_ON_LEVEL(LEVEL) (mmu_v6_version->nbits_per_level[LEVEL])
#define NLEVELS() (mmu_v6_version->nlevels)
#define VADDR_SIZE() (mmu_v6_version->vaddr_size)

#endif

#define LOAD_DATA_IN(ADDR) (address_space_ldq(((CPUState *) cpu)->as, ADDR, MEMTXATTRS_UNSPECIFIED, NULL))

#define SET_MMU_EXCEPTION(EXCP, N, C, P) { \
  (EXCP).number = N; \
  (EXCP).causecode = C; \
  (EXCP).parameter = P; \
}

uint32_t mmu_ctrl;

#define MMU_ENABLED_BIT 0
#define MMU_ENABLED_MASK (1 << MMU_ENABLED_BIT)
#define MMU_ENABLED ((mmu_ctrl & MMU_ENABLED_MASK) != 0)
#define MMU_IN_USER_KERNEL_MODE ((mmu_ctrl >> 1) & 1)

static void disable_mmuv6(void)
{
    mmu_ctrl &= ~MMU_ENABLED_MASK;
}
int mmuv6_enabled(void)
{
    return MMU_ENABLED;
}


uint32_t mmu_ttbcr;

#define MMU_TTBCR_TNSZ(I) ((mmu_ttbcr        >> (I * 16)) & 0x1f)
#define MMU_TTBCR_TNSH(I) (((mmu_ttbcr >> 4) >> (I * 16)) & 0x3)

#define MMU_TTBCR_A1  ((mmu_ttbcr >> 15) & 0x1)

/*
static void init_mmu_ttbcr(void)
{
    // TODO BE DONE
}
*/

uint64_t mmu_rtp0;
uint64_t mmu_rtp1;

uint64_t mmu_fault_status;

//static target_ulong mask_for_root_address(char x) {
//    switch(mmu_v6_version->id) {
//    case MMUV6_52_64K:
//        return (((1 << vaddr_size()) - 1) & (~((1 << x) - 1)));
//        break;
//    case MMUV6_42:
//        return (0x0000ffffffffffff & (~((1 << x) - 1)));
//        break;
//    case MMUV6_32_4K:
//        return (0xffffffff & (~((1 << x) - 1)));
//        break;
//
//    }
//}
#define MASK_FOR_ROOT_ADDRESS(X) \
    (((1ull << VADDR_SIZE()) - 1) & (~((1 << X) - 1)))

/* TODO: This is for MMU48 only. */

#ifndef CONFIG_USER_ONLY
static char x_for_ttbc(unsigned char i)
{
    const char xs[MMUV6_VERSION_SIZE][2] = {
        [MMUV6_52_64K] = { 13, 16 },
        [MMUV6_48_4K]  = { 12, 12 },
        [MMUV6_48_16K] = {4, 6 },
        [MMUV6_48_64K] = { 9, 13 },
    };
    switch(mmu_v6_version->id) {
    case MMUV6_32_4K:
        if(MMU_TTBCR_TNSZ(i) > 1)
            return (14 - MMU_TTBCR_TNSZ(i));
        else
            return (5 - MMU_TTBCR_TNSZ(i));
        break;
    default:
        return xs[mmu_v6_version->id][i];
        break;
    }
    assert("This should not happen !!!" == 0);
}
#endif

// Grab Root Table Address for RTPN
#ifndef CONFIG_USER_ONLY
static uint64_t root_address(uint64_t rtp)
{
    switch(mmu_v6_version->id) {
    case MMUV6_52_64K:
        return rtp << 4;
    default:
        return rtp;
        break;
    }
    assert("This should not happen !!!" == 0);
}
#endif

#define MMU_RTPN_ROOT_ADDRESS(N) \
    (root_address(mmu_rtp##N) & MASK_FOR_ROOT_ADDRESS(x_for_ttbc(N)))

/* TODO: This is for MMU48/52 only. */
#define MMU_RTPN_ASID(VADDR, N) \
  ((mmu_rtp##N >> 48) & 0xffff)

/* Table descriptors accessor macros */

#ifndef CONFIG_USER_ONLY
static uint64_t pte_tbl_next_level_table_address(unsigned char l, uint64_t pte)
{
    switch(mmu_v6_version->id) {
    case MMUV6_52_64K:
        /* TODO: This expects reserver bits in PTE to be 0 */
        return (((pte & 0xf000ull) << 36) | (pte & 0x0000ffffffff0000));
        break;
    default:
        /* TODO: This expects reserver bits in PTE to be 0 */
        return (pte & 0x0000fffffffff000);
        break;
    }
    assert("This should not happen !!!" == 0);
}
#endif

#define PTE_TBL_ATTRIBUTES(PTE)		      ((PTE & 0xf800000000000000) >> 59)

#define PTE_TBL_KERNEL_EXECUTE_NEVER_NEXT(PTE) (PTE_TBL_ATTRIBUTES(PTE) & 0x1)
#define PTE_TBL_USER_EXECUTE_NEVER_NEXT(PTE) (PTE_TBL_ATTRIBUTES(PTE) & 0x2)

#define PTE_TBL_ACCESS_PERMITIONS_NEXT(PTE) ((PTE_TBL_ATTRIBUTES(PTE) & 0xc) >> 2)
#define PTE_TBL_AP_NO_EFFECT(PTE)     (PTE_TBL_ACCESS_PERMITIONS_NEXT(PTE) == 0)
#define PTE_TBL_AP_NO_USER_MODE(PTE)  (PTE_TBL_ACCESS_PERMITIONS_NEXT(PTE) == 1)
#define PTE_TBL_AP_NO_WRITES(PTE)     (PTE_TBL_ACCESS_PERMITIONS_NEXT(PTE) == 2)
#define PTE_TBL_AP_NO_USER_READS_OR_WRITES(PTE) (PTE_TBL_ACCESS_PERMITIONS_NEXT(PTE) == 3)

/* Block descriptors accessor macros */

#define PTE_BLK_LOWER_ATTRS(PTE) ((PTE >>  2) & ((1 << 10) - 1))
#define PTE_BLK_UPPER_ATTRS(PTE) ((PTE >> 51) & ((1 << 13) - 1))

#define PTE_BLK_IS_READ_ONLY(PTE)   ((PTE_BLK_LOWER_ATTRS(PTE) & 0x20) != 0)   // bit 7 in PTE, 5 in attrs
#define PTE_BLK_IS_KERNEL_ONLY(PTE) ((PTE_BLK_LOWER_ATTRS(PTE) & 0x10) == 0)   // bit 6 in PTE, 4 in attrs
#define PTE_BLK_AF(PTE)		    ((PTE_BLK_LOWER_ATTRS(PTE) & 0x100) != 0)   // AF flag
// We also need to verify MMU_CTRL.KU. Don't know what it means for now. :(

#define PTE_BLK_KERNEL_EXECUTE_NEVER(PTE) ((PTE_BLK_UPPER_ATTRS(PTE) & 0x4) != 0)
#define PTE_BLK_USER_EXECUTE_NEVER(PTE) ((PTE_BLK_UPPER_ATTRS(PTE) & 0x8) != 0)

#define PTE_IS_BLOCK_DESCRIPTOR(PTE, LEVEL) \
     (((PTE & 0x3) == 1) && (LEVEL < (NLEVELS() - 1)))
#define PTE_IS_PAGE_DESCRIPTOR(PTE, LEVEL) \
     ((PTE & 0x3) == 3 && (LEVEL == (NLEVELS() - 1)))
#define PTE_IS_TABLE_DESCRIPTOR(PTE, LEVEL) \
     (!PTE_IS_PAGE_DESCRIPTOR(PTE, LEVEL) && ((PTE & 0x3) == 3))

#ifndef CONFIG_USER_ONLY
static bool pte_is_invalid(char pte, char level)
{
    switch(mmu_v6_version->id) {
/* This versions of MMU do not allow a block entry in the first
   table level. */
    case MMUV6_48_4K:
    case MMUV6_48_16K:
    case MMUV6_48_64K:
    case MMUV6_32_4K:
        return  (((pte & 0x1) == 0)
                 || ((pte & 0x2) == 0 && level == 0)
                 || ((pte & 0x2) == 0 && level == (NLEVELS()-1)));
        break;

/* This version of MMU permits block entry in the first table level. */
    case MMUV6_52_64K:
        return  (((pte & 0x1) == 0)
                 || ((pte & 0x2) == 0 && level == (NLEVELS()-1)));
        break;

    default:
        break;
    }
    assert("This should not happen !!!" == 0);
}
#endif

enum MMUv6_TLBCOMMAND {
    TLBInvalidateAll = 0x1,
    TLBRead,
    TLBInvalidateASID,
    TLBInvalidateAddr,
    TLBInvalidateRegion,
    TLBInvalidateRegionASID
};

static void
mmuv6_tlb_command(CPUARCState *env, enum MMUv6_TLBCOMMAND command)
{
    CPUState *cs = env_cpu(env);

    switch(command) {
    case TLBInvalidateAll:
    case TLBInvalidateASID:
    case TLBInvalidateAddr:
    case TLBInvalidateRegion:
    case TLBInvalidateRegionASID:
        /* For now we flush all entries all the time. */
        qemu_log_mask(CPU_LOG_MMU, "\n[MMUV3] TLB Flush cmd %d\n\n", command);
        tlb_flush(cs);
        break;

    case TLBRead:
        qemu_log_mask(LOG_UNIMP, "TLBRead command is not implemented for MMUv6.");
        break;
    }
}

target_ulong
arc_mmuv6_aux_get(const struct arc_aux_reg_detail *aux_reg_detail, void *data)
{
  target_ulong reg = 0;
  switch(aux_reg_detail->id)
  {
  case AUX_ID_mmuv6_build:
      reg = 0;
      reg |= (0x10 << 24);   /* Version: 0x10 (MMUv6) */
                          /* Type:    1 (MMUv48) */
      reg |= (mmu_v6_version->type << 21);
      reg |= (0 << 9);    /* TC:      0 (no translation cache) */
      reg |= (0 << 6);    /* L2TLB:   0 (256 entries) */
      reg |= (1 << 3);    /* ITLB:    1 (4 entries) */
      reg |= 2;           /* DTLB:    2 (8 entries) */

      qemu_log_mask(CPU_LOG_MMU, "\n[MMUV3] BUILD read " TARGET_FMT_lu " \n\n", reg);
      break;
  case AUX_ID_mmu_rtp0:
      reg = mmu_rtp0;
      qemu_log_mask(CPU_LOG_MMU, "\n[MMUV3] RTP0 read %lx\n\n", mmu_rtp0);
      break;
  case AUX_ID_mmu_rtp0hi:
      reg = (mmu_rtp0 >> 32);
      break;
  case AUX_ID_mmu_rtp1:
      qemu_log_mask(CPU_LOG_MMU, "\n[MMUV3] RTP1 read %lx\n\n", mmu_rtp1);
      reg = mmu_rtp1;
      break;
  case AUX_ID_mmu_rtp1hi:
      reg = (mmu_rtp1 >> 32);
      break;
  case AUX_ID_mmu_ctrl:
      reg = mmu_ctrl;
      break;
  case AUX_ID_mmu_ttbcr:
      reg = mmu_ttbcr;
      break;
  case AUX_ID_mmu_fault_status:
      reg = mmu_fault_status;
      break;
  default:
      break;
  }
  return reg;
}

void
arc_mmuv6_aux_set(const struct arc_aux_reg_detail *aux_reg_detail,
		  target_ulong val, void *data)
{
    CPUARCState *env = (CPUARCState *) data;
    CPUState *cs = env_cpu(env);
    uint64_t u64_val = val;

    switch(aux_reg_detail->id)
    {
    case AUX_ID_mmu_rtp0:
        qemu_log_mask(CPU_LOG_MMU, "\n[MMUV3] RTP0 update %lx"
                      " ==> " TARGET_FMT_lx "\n\n", mmu_rtp0, val);
        if (mmu_rtp0 != u64_val)
            tlb_flush(cs);
        mmu_rtp0 =  u64_val;
        break;
    case AUX_ID_mmu_rtp0hi:
        if ((mmu_rtp0 >> 32) != u64_val)
            tlb_flush(cs);
        mmu_rtp0 &= ~0xffffffff00000000;
        mmu_rtp0 |= (u64_val << 32);
        break;
    case AUX_ID_mmu_rtp1:
        if (mmu_rtp1 != u64_val)
            tlb_flush(cs);
        mmu_rtp1 =  u64_val;
        break;
    case AUX_ID_mmu_rtp1hi:
        if ((mmu_rtp1 >> 32) != u64_val)
            tlb_flush(cs);
        mmu_rtp1 &= ~0xffffffff00000000;
        mmu_rtp1 |= (u64_val << 32);
        break;
    case AUX_ID_mmu_ctrl:
        if (mmu_ctrl != val)
            tlb_flush(cs);
        mmu_ctrl =  val;
        qemu_log_mask(CPU_LOG_MMU, "mmu_ctrl = 0x" TARGET_FMT_lx "\n", val);
        break;
    case AUX_ID_mmu_ttbcr:
        mmu_ttbcr = val;
        break;
    case AUX_ID_mmuv6_tlbcommand:
        mmuv6_tlb_command(env, val);
        break;
    case AUX_ID_mmu_fault_status:
        assert(0);
        break;
    default:
        break;
    }
}

#ifndef CONFIG_USER_ONLY

#define ALL1_64BIT (0xffffffffffffffff)

static uint64_t
root_ptr_for_vaddr(uint64_t vaddr, bool *valid)
{
    /* TODO: This is only for MMUv48 */
    assert(mmu_v6_version->id != MMUV6_48_4K || (
           MMU_TTBCR_TNSZ(0) == MMU_TTBCR_TNSZ(1)
           && (MMU_TTBCR_TNSZ(0) == 16 || MMU_TTBCR_TNSZ(0) == 25)));

    switch(mmu_v6_version->id) {
    case MMUV6_52_64K:
    case MMUV6_48_4K:
    case MMUV6_48_16K:
    case MMUV6_48_64K:
        if ((vaddr >> (64-MMU_TTBCR_TNSZ(0))) == 0)
            return MMU_RTPN_ROOT_ADDRESS(0);

        if ((vaddr >> (64-MMU_TTBCR_TNSZ(1))) == ((1 << MMU_TTBCR_TNSZ(1)) - 1))
            return MMU_RTPN_ROOT_ADDRESS(1);
        break;
    case MMUV6_32_4K:
        if ((vaddr >> (32-MMU_TTBCR_TNSZ(0))) == 0)
            return MMU_RTPN_ROOT_ADDRESS(0);

        if ((vaddr >> (32-MMU_TTBCR_TNSZ(1))) == ((1 << MMU_TTBCR_TNSZ(1)) - 1))
            return MMU_RTPN_ROOT_ADDRESS(1);
        break;
    default:
        break;
    }
    *valid = false;
    return 0;
}


#define V6_PAGE_OFFSET_MASK (((1ull) << remainig_bits) - 1)
#define V6_PTE_PADDR_MASK  ((((1ull) << VADDR_SIZE()) - 1) & (~V6_PAGE_OFFSET_MASK))
#define V6_PADDR(PTE, VADDR) \
  ((PTE & V6_PTE_PADDR_MASK) | (VADDR & V6_PAGE_OFFSET_MASK))

#define RESTRICT_TBL_NO_USER_MODE (1 << 4)
#define RESTRICT_TBL_NO_WRITE_ACCESS (1 << 3)
#define RESTRICT_TBL_NO_USER_READ_WRITE_ACCESS (1 << 2)
#define RESTRICT_TBL_USER_EXECUTE_NEVER (1 << 1)
#define RESTRICT_TBL_KERNEL_EXECUTE_NEVER (1 << 0)

static bool
protv_violation(CPUARCState *env, uint64_t pte, int level, int table_perm_overwride, enum mmu_access_type rwe)
{
    bool in_kernel_mode = !(GET_STATUS_BIT(env->stat, Uf)); /* Read status for user mode. */
    bool trigger_prot_v = false;

    /* FIXME: user write access if Kr needs to trigger priv_v not prot v */
    if(rwe == MMU_MEM_WRITE && PTE_BLK_IS_READ_ONLY(pte)) {
        trigger_prot_v = true;
    }

    /* TODO: Isn't it a little bit tool late for this guard check? */
    if(PTE_IS_BLOCK_DESCRIPTOR(pte, level)
       || PTE_IS_PAGE_DESCRIPTOR(pte, level))
    {
        if (in_kernel_mode == true) {
            if (rwe == MMU_MEM_FETCH &&
                (PTE_BLK_KERNEL_EXECUTE_NEVER(pte) ||
                 (table_perm_overwride &
                  RESTRICT_TBL_KERNEL_EXECUTE_NEVER))) {
                trigger_prot_v = true;
            }
	} else {
	    if((rwe == MMU_MEM_READ || rwe == MMU_MEM_WRITE)
	       && (table_perm_overwride & RESTRICT_TBL_NO_USER_READ_WRITE_ACCESS) != 0) {
	        trigger_prot_v = true;
	    }

        if (rwe == MMU_MEM_FETCH &&
            (PTE_BLK_USER_EXECUTE_NEVER(pte) ||
             (table_perm_overwride & RESTRICT_TBL_USER_EXECUTE_NEVER))) {
            trigger_prot_v = true;
        }

	    if(rwe != MMU_MEM_IRRELEVANT_TYPE
           && (table_perm_overwride & RESTRICT_TBL_NO_USER_MODE) != 0) {
	        trigger_prot_v = true;
	    }

	    if(rwe != MMU_MEM_IRRELEVANT_TYPE
           && PTE_BLK_IS_KERNEL_ONLY(pte)) {
	        trigger_prot_v = true;
	    }
	}
    }

    return trigger_prot_v;
}

static int
get_prot_for_pte(CPUARCState *env, uint64_t pte,
                 int overwrite_permitions)
{
    int ret = PAGE_READ | PAGE_WRITE | PAGE_EXEC;

    bool in_kernel_mode = !(GET_STATUS_BIT(env->stat, Uf)); /* Read status for user mode. */

    if(in_kernel_mode
       && ((overwrite_permitions & RESTRICT_TBL_KERNEL_EXECUTE_NEVER) != 0
           || PTE_BLK_KERNEL_EXECUTE_NEVER(pte))) {
        ret &= ~PAGE_EXEC;
    }

    if(!in_kernel_mode
       && ((overwrite_permitions & RESTRICT_TBL_USER_EXECUTE_NEVER) != 0
           || PTE_BLK_USER_EXECUTE_NEVER(pte))) {
        ret &= ~PAGE_EXEC;
    }

	if(!in_kernel_mode
       && ((overwrite_permitions & RESTRICT_TBL_NO_USER_MODE) != 0
           || PTE_BLK_IS_KERNEL_ONLY(pte))) {
        ret &= ~PAGE_WRITE;
        ret &= ~PAGE_READ;
        ret &= ~PAGE_EXEC;
    }

	if((overwrite_permitions & RESTRICT_TBL_NO_WRITE_ACCESS) != 0) {
        ret &= ~PAGE_WRITE;
    }

	if(!in_kernel_mode
       && (overwrite_permitions & RESTRICT_TBL_NO_USER_READ_WRITE_ACCESS) != 0) {
        ret &= ~PAGE_WRITE;
        ret &= ~PAGE_READ;
    }

    if (PTE_BLK_IS_READ_ONLY(pte)) {
        ret &= ~PAGE_WRITE;
    }

    if ((!in_kernel_mode && PTE_BLK_USER_EXECUTE_NEVER(pte)) ||
        (in_kernel_mode && PTE_BLK_KERNEL_EXECUTE_NEVER(pte))) {
        ret &= ~PAGE_EXEC;
    }

    return ret;
}

static target_ulong
page_table_traverse(CPUARCState *env,
		   target_ulong vaddr, enum mmu_access_type rwe,
           int *prot,
           struct mem_exception *excp)
{
    bool found_block_descriptor = false;
    uint64_t pte, pte_addr;
    int l;
    int overwrite_permitions = 0;
    bool valid_root = true;
    uint64_t root = root_ptr_for_vaddr(vaddr, &valid_root);
    ARCCPU *cpu = env_archcpu (env);
    unsigned char remainig_bits = VADDR_SIZE();

    if(rwe != MMU_MEM_IRRELEVANT_TYPE) {
        qemu_log_mask(CPU_LOG_MMU, "[MMUV3] [PC " TARGET_FMT_lx
                      "] PageWalking for "TARGET_FMT_lx" [%s]\n", env->pc, vaddr, RWE_STRING(rwe));
    }

    if(valid_root == false) {
        if(rwe == MMU_MEM_FETCH || rwe == MMU_MEM_IRRELEVANT_TYPE) {
            SET_MMU_EXCEPTION(*excp, EXCP_IMMU_FAULT, 0x00, 0x00);
            return -1;
        } else if(rwe == MMU_MEM_READ || rwe == MMU_MEM_WRITE) {
            SET_MMU_EXCEPTION(*excp, EXCP_DMMU_FAULT, CAUSE_CODE(rwe), 0x00);
            return -1;
        }
    }

    for(l = 0; l < NLEVELS(); l++) {
        unsigned char bits_to_compare = N_BITS_ON_LEVEL(l);
        remainig_bits = remainig_bits - bits_to_compare;
        unsigned offset = (vaddr >> remainig_bits) & ((1<<bits_to_compare)-1);

        pte_addr = root + (8 * offset);
        pte = LOAD_DATA_IN(pte_addr);

        if(rwe != MMU_MEM_IRRELEVANT_TYPE) {
            qemu_log_mask(CPU_LOG_MMU, "[MMUV3] == Level: %d, offset: %d, pte_addr: %lx ==> %lx\n", l, offset, pte_addr, pte);
        }

        if(pte_is_invalid(pte, l)) {
            if(rwe != MMU_MEM_IRRELEVANT_TYPE) {
                qemu_log_mask(CPU_LOG_MMU, "[MMUV3] PTE seems invalid\n");
            }

            mmu_fault_status = (l & 0x7);
            if(rwe == MMU_MEM_FETCH || rwe == MMU_MEM_IRRELEVANT_TYPE) {
                SET_MMU_EXCEPTION(*excp, EXCP_IMMU_FAULT, 0x00, 0x00);
                return -1;
            } else if(rwe == MMU_MEM_READ || rwe == MMU_MEM_WRITE) {
                SET_MMU_EXCEPTION(*excp, EXCP_DMMU_FAULT, CAUSE_CODE(rwe), 0x00);
                return -1;
            }
        }


        if(PTE_IS_BLOCK_DESCRIPTOR(pte, l) || PTE_IS_PAGE_DESCRIPTOR(pte, l)) {
            if(PTE_BLK_AF(pte) != 0) {
                found_block_descriptor = true;
                break;
            } else {
                qemu_log_mask(CPU_LOG_MMU, "[MMUV3] PTE AF is not set\n");
                mmu_fault_status = (l & 0x7);
                if(rwe == MMU_MEM_FETCH || rwe == MMU_MEM_IRRELEVANT_TYPE) {
                    SET_MMU_EXCEPTION(*excp, EXCP_IMMU_FAULT, 0x10, 0x00);
                    return -1;
                } else if(rwe == MMU_MEM_READ || rwe == MMU_MEM_WRITE) {
                    SET_MMU_EXCEPTION(*excp, EXCP_DMMU_FAULT, 0x10 | CAUSE_CODE(rwe), 0x00);
                    return -1;
                }
            }
        }

        if(PTE_IS_TABLE_DESCRIPTOR(pte, l)) {
            if(PTE_TBL_KERNEL_EXECUTE_NEVER_NEXT(pte)) {
                overwrite_permitions |= RESTRICT_TBL_KERNEL_EXECUTE_NEVER;
            }
            if(PTE_TBL_USER_EXECUTE_NEVER_NEXT(pte)) {
                overwrite_permitions |= RESTRICT_TBL_USER_EXECUTE_NEVER;
            }
            if(PTE_TBL_AP_NO_USER_MODE(pte)) {
                overwrite_permitions |= RESTRICT_TBL_NO_USER_MODE;
            }
            if(PTE_TBL_AP_NO_WRITES(pte)) {
                overwrite_permitions |= RESTRICT_TBL_NO_WRITE_ACCESS;
            }
            if(PTE_TBL_AP_NO_USER_READS_OR_WRITES(pte)) {
                overwrite_permitions |= RESTRICT_TBL_NO_USER_READ_WRITE_ACCESS;
            }
        }

        if(pte_is_invalid(pte, l)) {
            if(rwe == MMU_MEM_FETCH || rwe == MMU_MEM_IRRELEVANT_TYPE) {
                SET_MMU_EXCEPTION(*excp, EXCP_IMMU_FAULT, 0x00, 0x00);
                return -1;
            } else if(rwe == MMU_MEM_READ || rwe == MMU_MEM_WRITE) {
                SET_MMU_EXCEPTION(*excp, EXCP_DMMU_FAULT, CAUSE_CODE(rwe), 0x00);
                return -1;
            }
        }

        root = pte_tbl_next_level_table_address(l, pte);
    }

    if(found_block_descriptor) {

        if(protv_violation(env, pte, l, overwrite_permitions, rwe)) {
            qemu_log_mask(CPU_LOG_MMU, "\n[MMUV3] [PC "TARGET_FMT_lx
                          "] PTE Protection violation: vaddr "TARGET_FMT_lx
                          " pte [addr %lx val %lx]\n", env->pc, vaddr, pte_addr, pte);
            found_block_descriptor = false;
            SET_MMU_EXCEPTION(*excp, EXCP_PROTV, CAUSE_CODE(rwe), 0x08);
            return -1;
        }

        if(prot != NULL) {
            *prot = get_prot_for_pte(env, pte, overwrite_permitions);
        }
        return V6_PADDR(pte, vaddr);
    }
    return -1;
}

#endif /* CONFIG_USER_ONLY */

#undef PAGE_OFFSET_MASK
#undef PTE_PADDR_MASK
#undef PADDR

void arc_mmu_init_v6(CPUARCState *env)
{
    ARCCPU *cpu = env_archcpu(env);

    switch(cpu->family) {
    case ARC_OPCODE_ARC64:
        //mmu_v6_version = &mmuv6_info[MMUV6_48_4K];

        if(cpu->cfg.mmuv6_version == NULL) {
            mmu_v6_version = &mmuv6_info[MMUV6_48_4K];
        } else if(!g_strcmp0(cpu->cfg.mmuv6_version, "48_4k")) {
            mmu_v6_version = &mmuv6_info[MMUV6_48_4K];
        } else if(!g_strcmp0(cpu->cfg.mmuv6_version, "48_16k")) {
            mmu_v6_version = &mmuv6_info[MMUV6_48_16K];
        } else if(!g_strcmp0(cpu->cfg.mmuv6_version, "48_64k")) {
            mmu_v6_version = &mmuv6_info[MMUV6_48_64K];
        } else if(!g_strcmp0(cpu->cfg.mmuv6_version, "52_64k")) {
            mmu_v6_version = &mmuv6_info[MMUV6_52_64K];
        } else {
            assert("MMUV6 mmuv6_version is invalid !!!" == 0);
        }
        break;
    case ARC_OPCODE_ARC32:
        mmu_v6_version = &mmuv6_info[MMUV6_32_4K];
        break;
    default:
        assert("Should not happen!!!" == 0);
        break;
    }

    return;
}

#ifndef CONFIG_USER_ONLY

static target_ulong
arc_mmuv6_translate(CPUARCState *env,
		            target_ulong vaddr, enum mmu_access_type rwe,
                    int *prot, struct mem_exception *excp)
{
    target_ulong paddr;

    /* This is really required. Fail in non singlestep without in_asm. */
    env->mmu.v6.exception.number = EXCP_NO_EXCEPTION;

    if(!MMU_ENABLED) {
      paddr = vaddr;
    }

   paddr = (target_ulong) page_table_traverse(env, vaddr, rwe, prot, excp);

    // TODO: Check if address is valid
    // Still need to know what it means ...

    return paddr;
}
#endif /* CONFIG_USER_ONLY */

#ifndef CONFIG_USER_ONLY

typedef enum {
    DIRECT_ACTION,
    MMU_ACTION,
} ACTION;

static int mmuv6_decide_action(const CPUARCState *env,
                  target_ulong       addr,
                  int                mmu_idx)
{
  if (MMU_ENABLED)
    return MMU_ACTION;
  else
    return DIRECT_ACTION;
}

#endif

static void QEMU_NORETURN raise_mem_exception(
        CPUState *cs, target_ulong addr, uintptr_t host_pc,
        struct mem_exception *excp)
{
    CPUARCState *env = &(ARC_CPU(cs)->env);
    if (excp->number != EXCP_IMMU_FAULT) {
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


bool
arc_get_physical_addr_v6(struct CPUState *cs, hwaddr *paddr, vaddr addr,
                  enum mmu_access_type rwe, bool probe,
                  uintptr_t retaddr)
{
#ifdef CONFIG_USER_ONLY
    *paddr = addr;
    return true;
#else
    CPUARCState *env = &((ARC_CPU(cs))->env);
    uintptr_t mmu_idx = cpu_mmu_index(env, true);
    int action = mmuv6_decide_action(env, addr, mmu_idx);
    struct mem_exception excp;
    int prot;
    excp.number = EXCP_NO_EXCEPTION;

    switch (action) {
    case DIRECT_ACTION:
        *paddr = addr;
        return true;
        break;
    case MMU_ACTION:
	    *paddr = arc_mmuv6_translate(env, addr, rwe, &prot, &excp);
        if ((enum exception_code_list) excp.number != EXCP_NO_EXCEPTION) {
            if (probe) {
                return false;
            }
            raise_mem_exception(cs, addr, retaddr, &excp);
        }
        else {
            return true;
        }
        break;
    default:
        g_assert_not_reached();
    }
#endif
}

/* Softmmu support function for MMU. */
bool arc_cpu_tlb_fill_v6(CPUState *cs, vaddr address, int size,
                      MMUAccessType access_type, int mmu_idx,
                      bool probe, uintptr_t retaddr)
{
    /* TODO: this rwe should go away when the TODO below is done */
    enum mmu_access_type rwe = (char) access_type;
    struct mem_exception excp;
    excp.number = EXCP_NO_EXCEPTION;

#ifndef CONFIG_USER_ONLY
    ARCCPU *cpu = ARC_CPU(cs);
    CPUARCState *env = &(cpu->env);
    int prot;
    int action = mmuv6_decide_action(env, address, mmu_idx);

    switch (action) {
    case DIRECT_ACTION:
        tlb_set_page(cs, address & PAGE_MASK, address & PAGE_MASK,
                     PAGE_READ | PAGE_WRITE | PAGE_EXEC,
                     mmu_idx, TARGET_PAGE_SIZE);
        break;
    case MMU_ACTION: {
        /*
         * TODO: these lines must go inside arc_mmu_translate and it
         * should only signal a failure or success --> generate an
         * exception or not
         */

        target_ulong paddr;
	    paddr = arc_mmuv6_translate(env, address, rwe, &prot, &excp);

        if ((enum exception_code_list) excp.number != EXCP_NO_EXCEPTION) {
            if (probe) {
                return false;
            }
            raise_mem_exception(cs, address, retaddr, &excp);
        }
        else {
		    tlb_set_page(cs, address, paddr & PAGE_MASK, prot,
		                 mmu_idx, TARGET_PAGE_SIZE);
        }
        break;
    }
    default:
        g_assert_not_reached();
    }
#else /* CONFIG_USER_ONLY */
    switch (access_type) {
    case MMU_INST_FETCH:
        SET_MMU_EXCEPTION(excp, EXCP_IMMU_FAULT, 0x00, 0x00);

        break;
    case MMU_DATA_LOAD:
    case MMU_DATA_STORE:
        SET_MMU_EXCEPTION(excp, EXCP_DMMU_FAULT, CAUSE_CODE(rwe), 0x00);
        break;
    default:
        g_assert_not_reached();
    }
    raise_mem_exception(cs, address, retaddr, &excp);
#endif /* CONFIG_USER_ONLY */

    return true;
}


#ifndef CONFIG_USER_ONLY
hwaddr arc_mmu_debug_translate_v6(CPUARCState *env, vaddr addr)
{
    struct mem_exception excp;

    if(mmuv6_enabled()) {
        return arc_mmuv6_translate(env, addr, MMU_MEM_IRRELEVANT_TYPE, NULL, &excp);
    } else {
        return addr;
    }
}
#endif /* CONFIG_USER_ONLY */

void arc_mmu_disable_v6(CPUARCState *env) {
    disable_mmuv6();
}

/*-*-indent-tabs-mode:nil;tab-width:4;indent-line-function:'insert-tab'-*-*/
/* vim: set ts=4 sw=4 et: */
