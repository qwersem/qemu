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
#include "translate.h"
#include "target/arc/semfunc.h"
#include "exec/gen-icount.h"
#include "tcg/tcg-op-gvec.h"

/**
 * @brief Generates the code for setting up a 64 bit register from a 32 bit one
 * Either by concatenating a pair or 0 extending it directly
 */
#define ARC_GEN_SRC_PAIR_UNSIGNED(REGISTER) \
    arc_gen_next_register_i32_i64(ctx, r64_##REGISTER, REGISTER);

#define ARC_GEN_SRC_PAIR_SIGNED(REGISTER) \
    arc_gen_next_register_i32_i64(ctx, r64_##REGISTER, REGISTER);

#define ARC_GEN_SRC_NOT_PAIR_SIGNED(REGISTER) \
    tcg_gen_ext_i32_i64(r64_##REGISTER, REGISTER);

#define ARC_GEN_SRC_NOT_PAIR_UNSIGNED(REGISTER) \
    tcg_gen_extu_i32_i64(r64_##REGISTER, REGISTER);

#define ARC_GEN_DST_PAIR(REGISTER) \
    tcg_gen_extr_i64_i32(REGISTER, nextRegWithNull(REGISTER), r64_##REGISTER);

#define ARC_GEN_DST_NOT_PAIR(REGISTER) \
    tcg_gen_extrl_i64_i32(REGISTER, r64_##REGISTER);

/**
 * @brief Generate the function call for signed/unsigned instructions
 */
#define ARC_GEN_BASE32_64_SIGNED(OPERATION) \
OPERATION(ctx, r64_a, r64_b, r64_c, acc, true, \
          tcg_gen_sextract_i64, \
          arc_gen_add_signed_overflow_i64)

#define ARC_GEN_BASE32_64_UNSIGNED(OPERATION) \
OPERATION(ctx, r64_a, r64_b, r64_c , acc, false, \
          tcg_gen_extract_i64, arc_gen_add_unsigned_overflow_i64)

/**
 * @brief Generate a function to be used by 32 bit versions to interface with
 * their 64 bit counterparts.
 * It is assumed the accumulator is always a pair register
 * @param NAME The name of the function
 * @param A_REG_INFO dest is PAIR or NOT_PAIR
 * @param B_REG_INFO first operand is PAIR or NOT_PAIR
 * @param C_REG_INFO second operand is PAIR or NOT_PAIR
 * @param IS_SIGNED OPERATION is signed or unsigned
 * @param OPERATION The operation to perform
 */
#define ARC_GEN_32BIT_INTERFACE(NAME, A_REG_INFO, B_REG_INFO, C_REG_INFO,  \
                                IS_SIGNED, OPERATION)                      \
static inline void                                                         \
arc_autogen_base32_##NAME(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)          \
{                                                                          \
    TCGv_i64 r64_a = tcg_temp_new_i64();                                   \
    TCGv_i64 r64_b = tcg_temp_new_i64();                                   \
    TCGv_i64 r64_c = tcg_temp_new_i64();                                   \
    TCGv_i64 acc = tcg_temp_new_i64();                                     \
    ARC_GEN_SRC_ ## B_REG_INFO ## _ ## IS_SIGNED(b);                       \
    ARC_GEN_SRC_ ## C_REG_INFO ## _ ## IS_SIGNED(c);                       \
    tcg_gen_concat_i32_i64(acc, cpu_acclo, cpu_acchi);                     \
    ARC_GEN_BASE32_64_##IS_SIGNED(OPERATION);                              \
    tcg_gen_extr_i64_i32(cpu_acclo, cpu_acchi, acc);                       \
    ARC_GEN_DST_##A_REG_INFO(a);                                           \
    tcg_temp_free_i64(acc);                                                \
    tcg_temp_free_i64(r64_a);                                              \
    tcg_temp_free_i64(r64_b);                                              \
    tcg_temp_free_i64(r64_c);                                              \
}

/*
 * FLAG
 *    Variables: @src
 *    Functions: getCCFlag, getRegister, getBit, hasInterrupts, Halt, ReplMask,
 *               targetHasOption, setRegister
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       status32 = getRegister (R_STATUS32);
 *       if(((getBit (@src, 0) == 1) && (getBit (status32, 7) == 0)))
 *         {
 *           if((hasInterrupts () > 0))
 *             {
 *               status32 = (status32 | 1);
 *               Halt ();
 *             };
 *         }
 *       else
 *         {
 *           ReplMask (status32, @src, 3840);
 *           if(((getBit (status32, 7) == 0) && (hasInterrupts () > 0)))
 *             {
 *               ReplMask (status32, @src, 30);
 *               if(targetHasOption (DIV_REM_OPTION))
 *                 {
 *                   ReplMask (status32, @src, 8192);
 *                 };
 *               if(targetHasOption (STACK_CHECKING))
 *                 {
 *                   ReplMask (status32, @src, 16384);
 *                 };
 *               if(targetHasOption (LL64_OPTION))
 *                 {
 *                   ReplMask (status32, @src, 524288);
 *                 };
 *               ReplMask (status32, @src, 1048576);
 *             };
 *         };
 *       setRegister (R_STATUS32, status32);
 *     };
 * }
 */

int
arc_gen_FLAG(DisasCtxt *ctx, TCGv src)
{
    int ret = DISAS_NEXT;
    TCGv temp_13 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_14 = tcg_temp_local_new();
    TCGv status32 = tcg_temp_local_new();
    TCGv temp_16 = tcg_temp_local_new();
    TCGv temp_15 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_18 = tcg_temp_local_new();
    TCGv temp_17 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_19 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_20 = tcg_temp_local_new();
    TCGv temp_22 = tcg_temp_local_new();
    TCGv temp_21 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_23 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    TCGv temp_12 = tcg_temp_local_new();
    TCGv temp_24 = tcg_temp_local_new();
    TCGv temp_25 = tcg_temp_local_new();
    TCGv temp_26 = tcg_temp_local_new();
    TCGv temp_27 = tcg_temp_local_new();
    TCGv temp_28 = tcg_temp_local_new();
    getCCFlag(temp_13);
    tcg_gen_mov_tl(cc_flag, temp_13);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    getRegister(temp_14, R_STATUS32);
    tcg_gen_mov_tl(status32, temp_14);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_movi_tl(temp_16, 0);
    getBit(temp_15, src, temp_16);
    tcg_gen_setcondi_tl(TCG_COND_EQ, temp_3, temp_15, 1);
    tcg_gen_movi_tl(temp_18, 7);
    getBit(temp_17, status32, temp_18);
    tcg_gen_setcondi_tl(TCG_COND_EQ, temp_4, temp_17, 0);
    tcg_gen_and_tl(temp_5, temp_3, temp_4);
    tcg_gen_xori_tl(temp_6, temp_5, 1);
    tcg_gen_andi_tl(temp_6, temp_6, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_6, arc_true, else_2);
    TCGLabel *done_3 = gen_new_label();
    hasInterrupts(temp_19);
    tcg_gen_setcondi_tl(TCG_COND_GT, temp_7, temp_19, 0);
    tcg_gen_xori_tl(temp_8, temp_7, 1);
    tcg_gen_andi_tl(temp_8, temp_8, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_8, arc_true, done_3);
    tcg_gen_ori_tl(status32, status32, 1);
    Halt();
    gen_set_label(done_3);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    tcg_gen_movi_tl(temp_20, 3840);
    ReplMask(status32, src, temp_20);
    TCGLabel *done_4 = gen_new_label();
    tcg_gen_movi_tl(temp_22, 7);
    getBit(temp_21, status32, temp_22);
    tcg_gen_setcondi_tl(TCG_COND_EQ, temp_9, temp_21, 0);
    hasInterrupts(temp_23);
    tcg_gen_setcondi_tl(TCG_COND_GT, temp_10, temp_23, 0);
    tcg_gen_and_tl(temp_11, temp_9, temp_10);
    tcg_gen_xori_tl(temp_12, temp_11, 1);
    tcg_gen_andi_tl(temp_12, temp_12, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_12, arc_true, done_4);
    tcg_gen_movi_tl(temp_24, 30);
    ReplMask(status32, src, temp_24);
    if (targetHasOption (DIV_REM_OPTION)) {
        tcg_gen_movi_tl(temp_25, 8192);
        ReplMask(status32, src, temp_25);
    }
    if (targetHasOption (STACK_CHECKING)) {
        tcg_gen_movi_tl(temp_26, 16384);
        ReplMask(status32, src, temp_26);
    }
    if (targetHasOption (LL64_OPTION)) {
        tcg_gen_movi_tl(temp_27, 524288);
        ReplMask(status32, src, temp_27);
    }
    tcg_gen_movi_tl(temp_28, 1048576);
    ReplMask(status32, src, temp_28);
    gen_set_label(done_4);
    gen_set_label(done_2);
    setRegister(R_STATUS32, status32);
    gen_set_label(done_1);
    tcg_temp_free(temp_13);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_14);
    tcg_temp_free(status32);
    tcg_temp_free(temp_16);
    tcg_temp_free(temp_15);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_18);
    tcg_temp_free(temp_17);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_19);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_20);
    tcg_temp_free(temp_22);
    tcg_temp_free(temp_21);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_23);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_11);
    tcg_temp_free(temp_12);
    tcg_temp_free(temp_24);
    tcg_temp_free(temp_25);
    tcg_temp_free(temp_26);
    tcg_temp_free(temp_27);
    tcg_temp_free(temp_28);

    return ret;
}


/*
 * KFLAG
 *    Variables: @src
 *    Functions: getCCFlag, getRegister, getBit, hasInterrupts, Halt, ReplMask,
 *               targetHasOption, setRegister
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       status32 = getRegister (R_STATUS32);
 *       if(((getBit (@src, 0) == 1) && (getBit (status32, 7) == 0)))
 *         {
 *           if((hasInterrupts () > 0))
 *             {
 *               status32 = (status32 | 1);
 *               Halt ();
 *             };
 *         }
 *       else
 *         {
 *           ReplMask (status32, @src, 3840);
 *           if(((getBit (status32, 7) == 0) && (hasInterrupts () > 0)))
 *             {
 *               ReplMask (status32, @src, 62);
 *               if(targetHasOption (DIV_REM_OPTION))
 *                 {
 *                   ReplMask (status32, @src, 8192);
 *                 };
 *               if(targetHasOption (STACK_CHECKING))
 *                 {
 *                   ReplMask (status32, @src, 16384);
 *                 };
 *               ReplMask (status32, @src, 65536);
 *               if(targetHasOption (LL64_OPTION))
 *                 {
 *                   ReplMask (status32, @src, 524288);
 *                 };
 *               ReplMask (status32, @src, 1048576);
 *               ReplMask (status32, @src, 2147483648);
 *             };
 *         };
 *       setRegister (R_STATUS32, status32);
 *     };
 * }
 */

int
arc_gen_KFLAG(DisasCtxt *ctx, TCGv src)
{
    int ret = DISAS_NEXT;
    TCGv temp_13 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_14 = tcg_temp_local_new();
    TCGv status32 = tcg_temp_local_new();
    TCGv temp_16 = tcg_temp_local_new();
    TCGv temp_15 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_18 = tcg_temp_local_new();
    TCGv temp_17 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_19 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_20 = tcg_temp_local_new();
    TCGv temp_22 = tcg_temp_local_new();
    TCGv temp_21 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_23 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    TCGv temp_12 = tcg_temp_local_new();
    TCGv temp_24 = tcg_temp_local_new();
    TCGv temp_25 = tcg_temp_local_new();
    TCGv temp_26 = tcg_temp_local_new();
    TCGv temp_27 = tcg_temp_local_new();
    TCGv temp_28 = tcg_temp_local_new();
    TCGv temp_29 = tcg_temp_local_new();
    TCGv temp_30 = tcg_temp_local_new();
    getCCFlag(temp_13);
    tcg_gen_mov_tl(cc_flag, temp_13);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    getRegister(temp_14, R_STATUS32);
    tcg_gen_mov_tl(status32, temp_14);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_movi_tl(temp_16, 0);
    getBit(temp_15, src, temp_16);
    tcg_gen_setcondi_tl(TCG_COND_EQ, temp_3, temp_15, 1);
    tcg_gen_movi_tl(temp_18, 7);
    getBit(temp_17, status32, temp_18);
    tcg_gen_setcondi_tl(TCG_COND_EQ, temp_4, temp_17, 0);
    tcg_gen_and_tl(temp_5, temp_3, temp_4);
    tcg_gen_xori_tl(temp_6, temp_5, 1);
    tcg_gen_andi_tl(temp_6, temp_6, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_6, arc_true, else_2);
    TCGLabel *done_3 = gen_new_label();
    hasInterrupts(temp_19);
    tcg_gen_setcondi_tl(TCG_COND_GT, temp_7, temp_19, 0);
    tcg_gen_xori_tl(temp_8, temp_7, 1);
    tcg_gen_andi_tl(temp_8, temp_8, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_8, arc_true, done_3);
    tcg_gen_ori_tl(status32, status32, 1);
    Halt();
    gen_set_label(done_3);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    tcg_gen_movi_tl(temp_20, 3840);
    ReplMask(status32, src, temp_20);
    TCGLabel *done_4 = gen_new_label();
    tcg_gen_movi_tl(temp_22, 7);
    getBit(temp_21, status32, temp_22);
    tcg_gen_setcondi_tl(TCG_COND_EQ, temp_9, temp_21, 0);
    hasInterrupts(temp_23);
    tcg_gen_setcondi_tl(TCG_COND_GT, temp_10, temp_23, 0);
    tcg_gen_and_tl(temp_11, temp_9, temp_10);
    tcg_gen_xori_tl(temp_12, temp_11, 1);
    tcg_gen_andi_tl(temp_12, temp_12, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_12, arc_true, done_4);
    tcg_gen_movi_tl(temp_24, 62);
    ReplMask(status32, src, temp_24);
    if (targetHasOption (DIV_REM_OPTION)) {
        tcg_gen_movi_tl(temp_25, 8192);
        ReplMask(status32, src, temp_25);
    }
    if (targetHasOption (STACK_CHECKING)) {
        tcg_gen_movi_tl(temp_26, 16384);
        ReplMask(status32, src, temp_26);
    }
    tcg_gen_movi_tl(temp_27, 65536);
    ReplMask(status32, src, temp_27);
    if (targetHasOption (LL64_OPTION)) {
        tcg_gen_movi_tl(temp_28, 524288);
        ReplMask(status32, src, temp_28);
    }
    tcg_gen_movi_tl(temp_29, 1048576);
    ReplMask(status32, src, temp_29);
    tcg_gen_movi_tl(temp_30, 2147483648);
    ReplMask(status32, src, temp_30);
    gen_set_label(done_4);
    gen_set_label(done_2);
    setRegister(R_STATUS32, status32);
    gen_set_label(done_1);
    tcg_temp_free(temp_13);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_14);
    tcg_temp_free(status32);
    tcg_temp_free(temp_16);
    tcg_temp_free(temp_15);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_18);
    tcg_temp_free(temp_17);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_19);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_20);
    tcg_temp_free(temp_22);
    tcg_temp_free(temp_21);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_23);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_11);
    tcg_temp_free(temp_12);
    tcg_temp_free(temp_24);
    tcg_temp_free(temp_25);
    tcg_temp_free(temp_26);
    tcg_temp_free(temp_27);
    tcg_temp_free(temp_28);
    tcg_temp_free(temp_29);
    tcg_temp_free(temp_30);

    return ret;
}


/*
 * ADD
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag, setCFlag, CarryADD,
 *               setVFlag, OverflowADD
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   lb = @b;
 *   lc = @c;
 *   if((cc_flag == true))
 *     {
 *       lb = @b;
 *       lc = @c;
 *       @a = (@b + @c);
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (@a);
 *           setNFlag (@a);
 *           setCFlag (CarryADD (@a, lb, lc));
 *           setVFlag (OverflowADD (@a, lb, lc));
 *         };
 *     };
 * }
 */

int
arc_gen_ADD(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv lc = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_mov_tl(lc, c);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_mov_tl(lc, c);
    tcg_gen_add_tl(a, b, c);
    if ((getFFlag () == true)) {
        setZFlag(a);
        setNFlag(a);
        CarryADD(temp_5, a, lb, lc);
        tcg_gen_mov_tl(temp_4, temp_5);
        setCFlag(temp_4);
        OverflowADD(temp_7, a, lb, lc);
        tcg_gen_mov_tl(temp_6, temp_7);
        setVFlag(temp_6);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(lb);
    tcg_temp_free(lc);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * ADD1
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag, setCFlag, CarryADD,
 *               setVFlag, OverflowADD
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   lb = @b;
 *   lc = @c << 1;
 *   if((cc_flag == true))
 *     {
 *       @a = (@b + lc);
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (@a);
 *           setNFlag (@a);
 *           setCFlag (CarryADD (@a, lb, lc));
 *           setVFlag (OverflowADD (@a, lb, lc));
 *         };
 *     };
 * }
 */

int
arc_gen_ADD1(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv lc = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_shli_tl(lc, c, 1);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_add_tl(a, b, lc);
    if ((getFFlag () == true)) {
        setZFlag(a);
        setNFlag(a);
        CarryADD(temp_5, a, lb, lc);
        tcg_gen_mov_tl(temp_4, temp_5);
        setCFlag(temp_4);
        OverflowADD(temp_7, a, lb, lc);
        tcg_gen_mov_tl(temp_6, temp_7);
        setVFlag(temp_6);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(lb);
    tcg_temp_free(lc);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * ADD2
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag, setCFlag, CarryADD,
 *               setVFlag, OverflowADD
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   lb = @b;
 *   lc = @c << 2;
 *   if((cc_flag == true))
 *     {
 *       @a = (@b + lc);
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (@a);
 *           setNFlag (@a);
 *           setCFlag (CarryADD (@a, lb, lc));
 *           setVFlag (OverflowADD (@a, lb, lc));
 *         };
 *     };
 * }
 */

int
arc_gen_ADD2(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv lc = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_shli_tl(lc, c, 2);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_add_tl(a, b, lc);
    if ((getFFlag () == true)) {
        setZFlag(a);
        setNFlag(a);
        CarryADD(temp_5, a, lb, lc);
        tcg_gen_mov_tl(temp_4, temp_5);
        setCFlag(temp_4);
        OverflowADD(temp_7, a, lb, lc);
        tcg_gen_mov_tl(temp_6, temp_7);
        setVFlag(temp_6);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(lb);
    tcg_temp_free(lc);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * ADD3
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag, setCFlag, CarryADD,
 *               setVFlag, OverflowADD
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   lb = @b;
 *   lc = @c << 3;
 *   if((cc_flag == true))
 *     {
 *       @a = (@b + lc);
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (@a);
 *           setNFlag (@a);
 *           setCFlag (CarryADD (@a, lb, lc));
 *           setVFlag (OverflowADD (@a, lb, lc));
 *         };
 *     };
 * }
 */

int
arc_gen_ADD3(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv lc = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_shli_tl(lc, c, 3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_add_tl(a, b, lc);
    if ((getFFlag () == true)) {
        setZFlag(a);
        setNFlag(a);
        CarryADD(temp_5, a, lb, lc);
        tcg_gen_mov_tl(temp_4, temp_5);
        setCFlag(temp_4);
        OverflowADD(temp_7, a, lb, lc);
        tcg_gen_mov_tl(temp_6, temp_7);
        setVFlag(temp_6);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(lb);
    tcg_temp_free(lc);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * ADC
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getCFlag, getFFlag, setZFlag, setNFlag, setCFlag,
 *               CarryADD, setVFlag, OverflowADD
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   lb = @b;
 *   lc = @c;
 *   if((cc_flag == true))
 *     {
 *       lb = @b;
 *       lc = @c;
 *       @a = ((@b + @c) + getCFlag ());
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (@a);
 *           setNFlag (@a);
 *           setCFlag (CarryADD (@a, lb, lc));
 *           setVFlag (OverflowADD (@a, lb, lc));
 *         };
 *     };
 * }
 */

int
arc_gen_ADC(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv lc = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_mov_tl(lc, c);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_mov_tl(lc, c);
    tcg_gen_add_tl(temp_4, b, c);
    getCFlag(temp_6);
    tcg_gen_mov_tl(temp_5, temp_6);
    tcg_gen_add_tl(a, temp_4, temp_5);
    if ((getFFlag () == true)) {
        setZFlag(a);
        setNFlag(a);
        CarryADD(temp_8, a, lb, lc);
        tcg_gen_mov_tl(temp_7, temp_8);
        setCFlag(temp_7);
        OverflowADD(temp_10, a, lb, lc);
        tcg_gen_mov_tl(temp_9, temp_10);
        setVFlag(temp_9);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(lb);
    tcg_temp_free(lc);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_9);

    return ret;
}


/*
 * SBC
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getCFlag, getFFlag, setZFlag, setNFlag, setCFlag,
 *               CarrySUB, setVFlag, OverflowSUB
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   lb = @b;
 *   lc = @c;
 *   if((cc_flag == true))
 *     {
 *       lb = @b;
 *       lc = @c;
 *       @a = ((@b - @c) - getCFlag ());
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (@a);
 *           setNFlag (@a);
 *           setCFlag (CarrySUB (@a, lb, lc));
 *           setVFlag (OverflowSUB (@a, lb, lc));
 *         };
 *     };
 * }
 */

int
arc_gen_SBC(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv lc = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_mov_tl(lc, c);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_mov_tl(lc, c);
    tcg_gen_sub_tl(temp_4, b, c);
    getCFlag(temp_6);
    tcg_gen_mov_tl(temp_5, temp_6);
    tcg_gen_sub_tl(a, temp_4, temp_5);
    if ((getFFlag () == true)) {
        setZFlag(a);
        setNFlag(a);
        CarrySUB(temp_8, a, lb, lc);
        tcg_gen_mov_tl(temp_7, temp_8);
        setCFlag(temp_7);
        OverflowSUB(temp_10, a, lb, lc);
        tcg_gen_mov_tl(temp_9, temp_10);
        setVFlag(temp_9);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(lb);
    tcg_temp_free(lc);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_9);

    return ret;
}


/*
 * NEG
 *    Variables: @b, @a
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag, setCFlag, CarrySUB,
 *               setVFlag, OverflowSUB
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   lb = @b;
 *   if((cc_flag == true))
 *     {
 *       lb = @b;
 *       @a = (0 - @b);
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (@a);
 *           setNFlag (@a);
 *           setCFlag (CarrySUB (@a, 0, lb));
 *           setVFlag (OverflowSUB (@a, 0, lb));
 *         };
 *     };
 * }
 */

int
arc_gen_NEG(DisasCtxt *ctx, TCGv b, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    tcg_gen_mov_tl(lb, b);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_subfi_tl(a, 0, b);
    if ((getFFlag () == true)) {
        setZFlag(a);
        setNFlag(a);
        tcg_gen_movi_tl(temp_6, 0);
        CarrySUB(temp_5, a, temp_6, lb);
        tcg_gen_mov_tl(temp_4, temp_5);
        setCFlag(temp_4);
        tcg_gen_movi_tl(temp_9, 0);
        OverflowSUB(temp_8, a, temp_9, lb);
        tcg_gen_mov_tl(temp_7, temp_8);
        setVFlag(temp_7);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(lb);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_7);

    return ret;
}


/*
 * SUB
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag, setCFlag, CarrySUB,
 *               setVFlag, OverflowSUB
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   lb = @b;
 *   if((cc_flag == true))
 *     {
 *       lb = @b;
 *       lc = @c;
 *       @a = (@b - @c);
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (@a);
 *           setNFlag (@a);
 *           setCFlag (CarrySUB (@a, lb, lc));
 *           setVFlag (OverflowSUB (@a, lb, lc));
 *         };
 *     };
 * }
 */

int
arc_gen_SUB(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lc = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    tcg_gen_mov_tl(lb, b);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_mov_tl(lc, c);
    tcg_gen_sub_tl(a, b, c);
    if ((getFFlag () == true)) {
        setZFlag(a);
        setNFlag(a);
        CarrySUB(temp_5, a, lb, lc);
        tcg_gen_mov_tl(temp_4, temp_5);
        setCFlag(temp_4);
        OverflowSUB(temp_7, a, lb, lc);
        tcg_gen_mov_tl(temp_6, temp_7);
        setVFlag(temp_6);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(lb);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lc);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * SUB1
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag, setCFlag, CarrySUB,
 *               setVFlag, OverflowSUB
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   lb = @b;
 *   if((cc_flag == true))
 *     {
 *       lb = @b;
 *       lc = (@c << 1);
 *       @a = (@b - lc);
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (@a);
 *           setNFlag (@a);
 *           setCFlag (CarrySUB (@a, lb, lc));
 *           setVFlag (OverflowSUB (@a, lb, lc));
 *         };
 *     };
 * }
 */

int
arc_gen_SUB1(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lc = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    tcg_gen_mov_tl(lb, b);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_shli_tl(lc, c, 1);
    tcg_gen_sub_tl(a, b, lc);
    if ((getFFlag () == true)) {
        setZFlag(a);
        setNFlag(a);
        CarrySUB(temp_5, a, lb, lc);
        tcg_gen_mov_tl(temp_4, temp_5);
        setCFlag(temp_4);
        OverflowSUB(temp_7, a, lb, lc);
        tcg_gen_mov_tl(temp_6, temp_7);
        setVFlag(temp_6);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(lb);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lc);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * SUB2
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag, setCFlag, CarrySUB,
 *               setVFlag, OverflowSUB
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   lb = @b;
 *   if((cc_flag == true))
 *     {
 *       lb = @b;
 *       lc = (@c << 2);
 *       @a = (@b - lc);
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (@a);
 *           setNFlag (@a);
 *           setCFlag (CarrySUB (@a, lb, lc));
 *           setVFlag (OverflowSUB (@a, lb, lc));
 *         };
 *     };
 * }
 */

int
arc_gen_SUB2(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lc = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    tcg_gen_mov_tl(lb, b);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_shli_tl(lc, c, 2);
    tcg_gen_sub_tl(a, b, lc);
    if ((getFFlag () == true)) {
        setZFlag(a);
        setNFlag(a);
        CarrySUB(temp_5, a, lb, lc);
        tcg_gen_mov_tl(temp_4, temp_5);
        setCFlag(temp_4);
        OverflowSUB(temp_7, a, lb, lc);
        tcg_gen_mov_tl(temp_6, temp_7);
        setVFlag(temp_6);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(lb);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lc);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * SUB3
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag, setCFlag, CarrySUB,
 *               setVFlag, OverflowSUB
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   lb = @b;
 *   if((cc_flag == true))
 *     {
 *       lb = @b;
 *       lc = (@c << 3);
 *       @a = (@b - lc);
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (@a);
 *           setNFlag (@a);
 *           setCFlag (CarrySUB (@a, lb, lc));
 *           setVFlag (OverflowSUB (@a, lb, lc));
 *         };
 *     };
 * }
 */

int
arc_gen_SUB3(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lc = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    tcg_gen_mov_tl(lb, b);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_shli_tl(lc, c, 3);
    tcg_gen_sub_tl(a, b, lc);
    if ((getFFlag () == true)) {
        setZFlag(a);
        setNFlag(a);
        CarrySUB(temp_5, a, lb, lc);
        tcg_gen_mov_tl(temp_4, temp_5);
        setCFlag(temp_4);
        OverflowSUB(temp_7, a, lb, lc);
        tcg_gen_mov_tl(temp_6, temp_7);
        setVFlag(temp_6);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(lb);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lc);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * MAX
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag, setCFlag, CarrySUB,
 *               setVFlag, OverflowSUB
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   lb = @b;
 *   if((cc_flag == true))
 *     {
 *       lb = @b;
 *       lc = @c;
 *       alu = (lb - lc);
 *       if((lc >= lb))
 *         {
 *           @a = lc;
 *         }
 *       else
 *         {
 *           @a = lb;
 *         };
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (alu);
 *           setNFlag (alu);
 *           setCFlag (CarrySUB (@a, lb, lc));
 *           setVFlag (OverflowSUB (@a, lb, lc));
 *         };
 *     };
 * }
 */

int
arc_gen_MAX(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_5 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lc = tcg_temp_local_new();
    TCGv alu = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    getCCFlag(temp_5);
    tcg_gen_mov_tl(cc_flag, temp_5);
    tcg_gen_mov_tl(lb, b);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_mov_tl(lc, c);
    tcg_gen_sub_tl(alu, lb, lc);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_GE, temp_3, lc, lb);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    tcg_gen_mov_tl(a, lc);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    tcg_gen_mov_tl(a, lb);
    gen_set_label(done_2);
    if ((getFFlag () == true)) {
        setZFlag(alu);
        setNFlag(alu);
        CarrySUB(temp_7, a, lb, lc);
        tcg_gen_mov_tl(temp_6, temp_7);
        setCFlag(temp_6);
        OverflowSUB(temp_9, a, lb, lc);
        tcg_gen_mov_tl(temp_8, temp_9);
        setVFlag(temp_8);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_5);
    tcg_temp_free(cc_flag);
    tcg_temp_free(lb);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lc);
    tcg_temp_free(alu);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_8);

    return ret;
}


/*
 * MIN
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag, setCFlag, CarrySUB,
 *               setVFlag, OverflowSUB
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   lb = @b;
 *   if((cc_flag == true))
 *     {
 *       lb = @b;
 *       lc = @c;
 *       alu = (lb - lc);
 *       if((lc <= lb))
 *         {
 *           @a = lc;
 *         }
 *       else
 *         {
 *           @a = lb;
 *         };
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (alu);
 *           setNFlag (alu);
 *           setCFlag (CarrySUB (@a, lb, lc));
 *           setVFlag (OverflowSUB (@a, lb, lc));
 *         };
 *     };
 * }
 */

int
arc_gen_MIN(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_5 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lc = tcg_temp_local_new();
    TCGv alu = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    getCCFlag(temp_5);
    tcg_gen_mov_tl(cc_flag, temp_5);
    tcg_gen_mov_tl(lb, b);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_mov_tl(lc, c);
    tcg_gen_sub_tl(alu, lb, lc);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_LE, temp_3, lc, lb);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    tcg_gen_mov_tl(a, lc);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    tcg_gen_mov_tl(a, lb);
    gen_set_label(done_2);
    if ((getFFlag () == true)) {
        setZFlag(alu);
        setNFlag(alu);
        CarrySUB(temp_7, a, lb, lc);
        tcg_gen_mov_tl(temp_6, temp_7);
        setCFlag(temp_6);
        OverflowSUB(temp_9, a, lb, lc);
        tcg_gen_mov_tl(temp_8, temp_9);
        setVFlag(temp_8);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_5);
    tcg_temp_free(cc_flag);
    tcg_temp_free(lb);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lc);
    tcg_temp_free(alu);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_8);

    return ret;
}


/*
 * CMP
 *    Variables: @b, @c
 *    Functions: getCCFlag, setZFlag, setNFlag, setCFlag, CarrySUB, setVFlag,
 *               OverflowSUB
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       alu = (@b - @c);
 *       setZFlag (alu);
 *       setNFlag (alu);
 *       setCFlag (CarrySUB (alu, @b, @c));
 *       setVFlag (OverflowSUB (alu, @b, @c));
 *     };
 * }
 */

int
arc_gen_CMP(DisasCtxt *ctx, TCGv b, TCGv c)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv alu = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_sub_tl(alu, b, c);
    setZFlag(alu);
    setNFlag(alu);
    CarrySUB(temp_5, alu, b, c);
    tcg_gen_mov_tl(temp_4, temp_5);
    setCFlag(temp_4);
    OverflowSUB(temp_7, alu, b, c);
    tcg_gen_mov_tl(temp_6, temp_7);
    setVFlag(temp_6);
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(alu);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * AND
 *    Variables: @a, @b, @c
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       la = (@b & @c);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *         };
 *     };
 * }
 */

int
arc_gen_AND(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_and_tl(la, b, c);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(la);

    return ret;
}


/*
 * OR
 *    Variables: @a, @b, @c
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       la = (@b | @c);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *         };
 *     };
 * }
 */

int
arc_gen_OR(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_or_tl(la, b, c);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(la);

    return ret;
}


/*
 * XOR
 *    Variables: @a, @b, @c
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       la = (@b ^ @c);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *         };
 *     };
 * }
 */

int
arc_gen_XOR(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_xor_tl(la, b, c);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(la);

    return ret;
}


/*
 * MOV
 *    Variables: @a, @b
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       la = @b;
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *         };
 *     };
 * }
 */

int
arc_gen_MOV(DisasCtxt *ctx, TCGv a, TCGv b)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(la, b);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(la);

    return ret;
}


/*
 * ASL
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag, setCFlag, getBit,
 *               setVFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       lb = @b;
 *       lc = (@c & 31);
 *       la = (lb << lc);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *           if((lc == 0))
 *             {
 *               setCFlag (0);
 *             }
 *           else
 *             {
 *               setCFlag (getBit (lb, (32 - lc)));
 *             };
 *           if((@c == 268435457))
 *             {
 *               t1 = getBit (la, 31);
 *               t2 = getBit (lb, 31);
 *               if((t1 == t2))
 *                 {
 *                   setVFlag (0);
 *                 }
 *               else
 *                 {
 *                   setVFlag (1);
 *                 };
 *             };
 *         };
 *     };
 * }
 */

int
arc_gen_ASL(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_9 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv lc = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_13 = tcg_temp_local_new();
    TCGv temp_12 = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_15 = tcg_temp_local_new();
    TCGv temp_14 = tcg_temp_local_new();
    TCGv t1 = tcg_temp_local_new();
    TCGv temp_17 = tcg_temp_local_new();
    TCGv temp_16 = tcg_temp_local_new();
    TCGv t2 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_18 = tcg_temp_local_new();
    TCGv temp_19 = tcg_temp_local_new();
    getCCFlag(temp_9);
    tcg_gen_mov_tl(cc_flag, temp_9);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_andi_tl(lc, c, 31);
    tcg_gen_shl_tl(la, lb, lc);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
        TCGLabel *else_2 = gen_new_label();
        TCGLabel *done_2 = gen_new_label();
        tcg_gen_setcondi_tl(TCG_COND_EQ, temp_3, lc, 0);
        tcg_gen_xori_tl(temp_4, temp_3, 1);
        tcg_gen_andi_tl(temp_4, temp_4, 1);
        tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
        tcg_gen_movi_tl(temp_10, 0);
        setCFlag(temp_10);
        tcg_gen_br(done_2);
        gen_set_label(else_2);
        tcg_gen_subfi_tl(temp_13, 32, lc);
        getBit(temp_12, lb, temp_13);
        tcg_gen_mov_tl(temp_11, temp_12);
        setCFlag(temp_11);
        gen_set_label(done_2);
        TCGLabel *done_3 = gen_new_label();
        tcg_gen_setcondi_tl(TCG_COND_EQ, temp_5, c, 268435457);
        tcg_gen_xori_tl(temp_6, temp_5, 1);
        tcg_gen_andi_tl(temp_6, temp_6, 1);
        tcg_gen_brcond_tl(TCG_COND_EQ, temp_6, arc_true, done_3);
        tcg_gen_movi_tl(temp_15, 31);
        getBit(temp_14, la, temp_15);
        tcg_gen_mov_tl(t1, temp_14);
        tcg_gen_movi_tl(temp_17, 31);
        getBit(temp_16, lb, temp_17);
        tcg_gen_mov_tl(t2, temp_16);
        TCGLabel *else_4 = gen_new_label();
        TCGLabel *done_4 = gen_new_label();
        tcg_gen_setcond_tl(TCG_COND_EQ, temp_7, t1, t2);
        tcg_gen_xori_tl(temp_8, temp_7, 1);
        tcg_gen_andi_tl(temp_8, temp_8, 1);
        tcg_gen_brcond_tl(TCG_COND_EQ, temp_8, arc_true, else_4);
        tcg_gen_movi_tl(temp_18, 0);
        setVFlag(temp_18);
        tcg_gen_br(done_4);
        gen_set_label(else_4);
        tcg_gen_movi_tl(temp_19, 1);
        setVFlag(temp_19);
        gen_set_label(done_4);
        gen_set_label(done_3);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_9);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lb);
    tcg_temp_free(lc);
    tcg_temp_free(la);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_13);
    tcg_temp_free(temp_12);
    tcg_temp_free(temp_11);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_15);
    tcg_temp_free(temp_14);
    tcg_temp_free(t1);
    tcg_temp_free(temp_17);
    tcg_temp_free(temp_16);
    tcg_temp_free(t2);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_18);
    tcg_temp_free(temp_19);

    return ret;
}


/*
 * ASR
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, arithmeticShiftRight, getFFlag, setZFlag, setNFlag,
 *               setCFlag, getBit
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       lb = @b;
 *       lc = (@c & 31);
 *       la = arithmeticShiftRight (lb, lc);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *           if((lc == 0))
 *             {
 *               setCFlag (0);
 *             }
 *           else
 *             {
 *               setCFlag (getBit (lb, (lc - 1)));
 *             };
 *         };
 *     };
 * }
 */

int
arc_gen_ASR(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_5 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv lc = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    getCCFlag(temp_5);
    tcg_gen_mov_tl(cc_flag, temp_5);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_andi_tl(lc, c, 31);
    arithmeticShiftRight(temp_6, lb, lc);
    tcg_gen_mov_tl(la, temp_6);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
        TCGLabel *else_2 = gen_new_label();
        TCGLabel *done_2 = gen_new_label();
        tcg_gen_setcondi_tl(TCG_COND_EQ, temp_3, lc, 0);
        tcg_gen_xori_tl(temp_4, temp_3, 1);
        tcg_gen_andi_tl(temp_4, temp_4, 1);
        tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
        tcg_gen_movi_tl(temp_7, 0);
        setCFlag(temp_7);
        tcg_gen_br(done_2);
        gen_set_label(else_2);
        tcg_gen_subi_tl(temp_10, lc, 1);
        getBit(temp_9, lb, temp_10);
        tcg_gen_mov_tl(temp_8, temp_9);
        setCFlag(temp_8);
        gen_set_label(done_2);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_5);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lb);
    tcg_temp_free(lc);
    tcg_temp_free(temp_6);
    tcg_temp_free(la);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_8);

    return ret;
}


/*
 * ASR8
 *    Variables: @b, @a
 *    Functions: getCCFlag, arithmeticShiftRight, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       lb = @b;
 *       la = arithmeticShiftRight (lb, 8);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *         };
 *     };
 * }
 */

int
arc_gen_ASR8(DisasCtxt *ctx, TCGv b, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_movi_tl(temp_5, 8);
    arithmeticShiftRight(temp_4, lb, temp_5);
    tcg_gen_mov_tl(la, temp_4);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lb);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(la);

    return ret;
}


/*
 * ASR16
 *    Variables: @b, @a
 *    Functions: getCCFlag, arithmeticShiftRight, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       lb = @b;
 *       la = arithmeticShiftRight (lb, 16);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *         };
 *     };
 * }
 */

int
arc_gen_ASR16(DisasCtxt *ctx, TCGv b, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_movi_tl(temp_5, 16);
    arithmeticShiftRight(temp_4, lb, temp_5);
    tcg_gen_mov_tl(la, temp_4);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lb);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(la);

    return ret;
}


/*
 * LSL16
 *    Variables: @b, @a
 *    Functions: getCCFlag, logicalShiftLeft, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       la = logicalShiftLeft (@b, 16);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *         };
 *     };
 * }
 */

int
arc_gen_LSL16(DisasCtxt *ctx, TCGv b, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_movi_tl(temp_5, 16);
    logicalShiftLeft(temp_4, b, temp_5);
    tcg_gen_mov_tl(la, temp_4);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(la);

    return ret;
}


/*
 * LSL8
 *    Variables: @b, @a
 *    Functions: getCCFlag, logicalShiftLeft, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       la = logicalShiftLeft (@b, 8);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *         };
 *     };
 * }
 */

int
arc_gen_LSL8(DisasCtxt *ctx, TCGv b, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_movi_tl(temp_5, 8);
    logicalShiftLeft(temp_4, b, temp_5);
    tcg_gen_mov_tl(la, temp_4);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(la);

    return ret;
}


/*
 * LSR
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, logicalShiftRight, getFFlag, setZFlag, setNFlag,
 *               setCFlag, getBit
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       lb = @b;
 *       lc = (@c & 31);
 *       la = logicalShiftRight (lb, lc);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *           if((lc == 0))
 *             {
 *               setCFlag (0);
 *             }
 *           else
 *             {
 *               setCFlag (getBit (lb, (lc - 1)));
 *             };
 *         };
 *     };
 * }
 */

int
arc_gen_LSR(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_5 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lb = tcg_temp_local_new();
    TCGv lc = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    getCCFlag(temp_5);
    tcg_gen_mov_tl(cc_flag, temp_5);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lb, b);
    tcg_gen_andi_tl(lc, c, 31);
    logicalShiftRight(temp_6, lb, lc);
    tcg_gen_mov_tl(la, temp_6);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
        TCGLabel *else_2 = gen_new_label();
        TCGLabel *done_2 = gen_new_label();
        tcg_gen_setcondi_tl(TCG_COND_EQ, temp_3, lc, 0);
        tcg_gen_xori_tl(temp_4, temp_3, 1);
        tcg_gen_andi_tl(temp_4, temp_4, 1);
        tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
        tcg_gen_movi_tl(temp_7, 0);
        setCFlag(temp_7);
        tcg_gen_br(done_2);
        gen_set_label(else_2);
        tcg_gen_subi_tl(temp_10, lc, 1);
        getBit(temp_9, lb, temp_10);
        tcg_gen_mov_tl(temp_8, temp_9);
        setCFlag(temp_8);
        gen_set_label(done_2);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_5);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lb);
    tcg_temp_free(lc);
    tcg_temp_free(temp_6);
    tcg_temp_free(la);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_8);

    return ret;
}


/*
 * LSR16
 *    Variables: @b, @a
 *    Functions: getCCFlag, logicalShiftRight, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       la = logicalShiftRight (@b, 16);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *         };
 *     };
 * }
 */

int
arc_gen_LSR16(DisasCtxt *ctx, TCGv b, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_movi_tl(temp_5, 16);
    logicalShiftRight(temp_4, b, temp_5);
    tcg_gen_mov_tl(la, temp_4);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(la);

    return ret;
}


/*
 * LSR8
 *    Variables: @b, @a
 *    Functions: getCCFlag, logicalShiftRight, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       la = logicalShiftRight (@b, 8);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *         };
 *     };
 * }
 */

int
arc_gen_LSR8(DisasCtxt *ctx, TCGv b, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_movi_tl(temp_5, 8);
    logicalShiftRight(temp_4, b, temp_5);
    tcg_gen_mov_tl(la, temp_4);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(la);

    return ret;
}


/*
 * BIC
 *    Variables: @a, @b, @c
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       la = (@b & ~@c);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *         };
 *     };
 * }
 */

int
arc_gen_BIC(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_not_tl(temp_4, c);
    tcg_gen_and_tl(la, b, temp_4);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_4);
    tcg_temp_free(la);

    return ret;
}


/*
 * BCLR
 *    Variables: @c, @a, @b
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       tmp = (1 << (@c & 31));
 *       la = (@b & ~tmp);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *         };
 *     };
 * }
 */

int
arc_gen_BCLR(DisasCtxt *ctx, TCGv c, TCGv a, TCGv b)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv tmp = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_andi_tl(temp_4, c, 31);
    tcg_gen_shlfi_tl(tmp, 1, temp_4);
    tcg_gen_not_tl(temp_5, tmp);
    tcg_gen_and_tl(la, b, temp_5);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_4);
    tcg_temp_free(tmp);
    tcg_temp_free(temp_5);
    tcg_temp_free(la);

    return ret;
}


/*
 * BMSK
 *    Variables: @c, @a, @b
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       tmp1 = ((@c & 31) + 1);
 *       if((tmp1 == 32))
 *         {
 *           tmp2 = 4294967295;
 *         }
 *       else
 *         {
 *           tmp2 = ((1 << tmp1) - 1);
 *         };
 *       la = (@b & tmp2);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *         };
 *     };
 * }
 */

int
arc_gen_BMSK(DisasCtxt *ctx, TCGv c, TCGv a, TCGv b)
{
    int ret = DISAS_NEXT;
    TCGv temp_5 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv tmp1 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv tmp2 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_5);
    tcg_gen_mov_tl(cc_flag, temp_5);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_andi_tl(temp_6, c, 31);
    tcg_gen_addi_tl(tmp1, temp_6, 1);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcondi_tl(TCG_COND_EQ, temp_3, tmp1, 32);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    tcg_gen_movi_tl(tmp2, 4294967295);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    tcg_gen_shlfi_tl(temp_7, 1, tmp1);
    tcg_gen_subi_tl(tmp2, temp_7, 1);
    gen_set_label(done_2);
    tcg_gen_and_tl(la, b, tmp2);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_5);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_6);
    tcg_temp_free(tmp1);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(tmp2);
    tcg_temp_free(temp_7);
    tcg_temp_free(la);

    return ret;
}


/*
 * BMSKN
 *    Variables: @c, @a, @b
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       tmp1 = ((@c & 31) + 1);
 *       if((tmp1 == 32))
 *         {
 *           tmp2 = 4294967295;
 *         }
 *       else
 *         {
 *           tmp2 = ((1 << tmp1) - 1);
 *         };
 *       la = (@b & ~tmp2);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *         };
 *     };
 * }
 */

int
arc_gen_BMSKN(DisasCtxt *ctx, TCGv c, TCGv a, TCGv b)
{
    int ret = DISAS_NEXT;
    TCGv temp_5 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv tmp1 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv tmp2 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_5);
    tcg_gen_mov_tl(cc_flag, temp_5);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_andi_tl(temp_6, c, 31);
    tcg_gen_addi_tl(tmp1, temp_6, 1);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcondi_tl(TCG_COND_EQ, temp_3, tmp1, 32);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    tcg_gen_movi_tl(tmp2, 4294967295);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    tcg_gen_shlfi_tl(temp_7, 1, tmp1);
    tcg_gen_subi_tl(tmp2, temp_7, 1);
    gen_set_label(done_2);
    tcg_gen_not_tl(temp_8, tmp2);
    tcg_gen_and_tl(la, b, temp_8);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_5);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_6);
    tcg_temp_free(tmp1);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(tmp2);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_8);
    tcg_temp_free(la);

    return ret;
}


/*
 * BSET
 *    Variables: @c, @a, @b
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       tmp = (1 << (@c & 31));
 *       la = (@b | tmp);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *         };
 *     };
 * }
 */

int
arc_gen_BSET(DisasCtxt *ctx, TCGv c, TCGv a, TCGv b)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv tmp = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_andi_tl(temp_4, c, 31);
    tcg_gen_shlfi_tl(tmp, 1, temp_4);
    tcg_gen_or_tl(la, b, tmp);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_4);
    tcg_temp_free(tmp);
    tcg_temp_free(la);

    return ret;
}


/*
 * BXOR
 *    Variables: @c, @a, @b
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       tmp = (1 << @c);
 *       la = (@b ^ tmp);
 *       @a = la;
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (la);
 *           setNFlag (la);
 *         };
 *     };
 * }
 */

int
arc_gen_BXOR(DisasCtxt *ctx, TCGv c, TCGv a, TCGv b)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv tmp = tcg_temp_local_new();
    TCGv la = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_shlfi_tl(tmp, 1, c);
    tcg_gen_xor_tl(la, b, tmp);
    tcg_gen_mov_tl(a, la);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(la);
        setNFlag(la);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(tmp);
    tcg_temp_free(la);

    return ret;
}


/*
 * ROL
 *    Variables: @src, @dest, @n
 *    Functions: getCCFlag, rotateLeft, getFFlag, setZFlag, setNFlag, setCFlag,
 *               extractBits
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       lsrc = @src;
 *       @dest = rotateLeft (lsrc, 1);
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (@dest);
 *           setNFlag (@dest);
 *           setCFlag (extractBits (lsrc, 31, 31));
 *         };
 *     };
 * }
 */

int
arc_gen_ROL (DisasCtxt *ctx, TCGv src, TCGv n, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lsrc = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    int f_flag;
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lsrc, src);
    tcg_gen_andi_tl(temp_5, n, 31);
    rotateLeft(temp_4, lsrc, temp_5);
    tcg_gen_mov_tl(dest, temp_4);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(dest);
        setNFlag(dest);
        tcg_gen_movi_tl(temp_9, 31);
        tcg_gen_movi_tl(temp_8, 31);
        extractBits(temp_7, lsrc, temp_8, temp_9);
        tcg_gen_mov_tl(temp_6, temp_7);
        setCFlag(temp_6);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lsrc);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * ROL8
 *    Variables: @src, @dest
 *    Functions: getCCFlag, rotateLeft, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       lsrc = @src;
 *       @dest = rotateLeft (lsrc, 8);
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (@dest);
 *           setNFlag (@dest);
 *         };
 *     };
 * }
 */

int
arc_gen_ROL8(DisasCtxt *ctx, TCGv src, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lsrc = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lsrc, src);
    tcg_gen_movi_tl(temp_5, 8);
    rotateLeft(temp_4, lsrc, temp_5);
    tcg_gen_mov_tl(dest, temp_4);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(dest);
        setNFlag(dest);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lsrc);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);

    return ret;
}


/*
 * ROR
 *    Variables: @src, @n, @dest
 *    Functions: getCCFlag, rotateRight, getFFlag, setZFlag, setNFlag,
 *               setCFlag, extractBits
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       lsrc = @src;
 *       ln = (@n & 31);
 *       @dest = rotateRight (lsrc, ln);
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (@dest);
 *           setNFlag (@dest);
 *           setCFlag (extractBits (lsrc, (ln - 1), (ln - 1)));
 *         };
 *     };
 * }
 */

int
arc_gen_ROR(DisasCtxt *ctx, TCGv src, TCGv n, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lsrc = tcg_temp_local_new();
    TCGv ln = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    int f_flag;
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lsrc, src);
    tcg_gen_andi_tl(ln, n, 31);
    rotateRight(temp_4, lsrc, ln);
    tcg_gen_mov_tl(dest, temp_4);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(dest);
        setNFlag(dest);
        tcg_gen_subi_tl(temp_8, ln, 1);
        tcg_gen_subi_tl(temp_7, ln, 1);
        extractBits(temp_6, lsrc, temp_7, temp_8);
        tcg_gen_mov_tl(temp_5, temp_6);
        setCFlag(temp_5);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lsrc);
    tcg_temp_free(ln);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_5);

    return ret;
}


/*
 * ROR8
 *    Variables: @src, @dest
 *    Functions: getCCFlag, rotateRight, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       lsrc = @src;
 *       @dest = rotateRight (lsrc, 8);
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (@dest);
 *           setNFlag (@dest);
 *         };
 *     };
 * }
 */

int
arc_gen_ROR8(DisasCtxt *ctx, TCGv src, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lsrc = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lsrc, src);
    tcg_gen_movi_tl(temp_5, 8);
    rotateRight(temp_4, lsrc, temp_5);
    tcg_gen_mov_tl(dest, temp_4);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(dest);
        setNFlag(dest);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lsrc);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);

    return ret;
}


/*
 * RLC
 *    Variables: @src, @dest
 *    Functions: getCCFlag, getCFlag, getFFlag, setZFlag, setNFlag, setCFlag,
 *               extractBits
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       lsrc = @src;
 *       @dest = (lsrc << 1);
 *       @dest = (@dest | getCFlag ());
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (@dest);
 *           setNFlag (@dest);
 *           setCFlag (extractBits (lsrc, 31, 31));
 *         };
 *     };
 * }
 */

int
arc_gen_RLC(DisasCtxt *ctx, TCGv src, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lsrc = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    int f_flag;
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lsrc, src);
    tcg_gen_shli_tl(dest, lsrc, 1);
    getCFlag(temp_5);
    tcg_gen_mov_tl(temp_4, temp_5);
    tcg_gen_or_tl(dest, dest, temp_4);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(dest);
        setNFlag(dest);
        tcg_gen_movi_tl(temp_9, 31);
        tcg_gen_movi_tl(temp_8, 31);
        extractBits(temp_7, lsrc, temp_8, temp_9);
        tcg_gen_mov_tl(temp_6, temp_7);
        setCFlag(temp_6);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lsrc);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * RRC
 *    Variables: @src, @dest
 *    Functions: getCCFlag, getCFlag, getFFlag, setZFlag, setNFlag, setCFlag,
 *               extractBits
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       lsrc = @src;
 *       @dest = (lsrc >> 1);
 *       @dest = (@dest | (getCFlag () << 31));
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (@dest);
 *           setNFlag (@dest);
 *           setCFlag (extractBits (lsrc, 0, 0));
 *         };
 *     };
 * }
 */

int
arc_gen_RRC(DisasCtxt *ctx, TCGv src, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv lsrc = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    int f_flag;
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(lsrc, src);
    tcg_gen_shri_tl(dest, lsrc, 1);
    getCFlag(temp_6);
    tcg_gen_mov_tl(temp_5, temp_6);
    tcg_gen_shli_tl(temp_4, temp_5, 31);
    tcg_gen_or_tl(dest, dest, temp_4);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(dest);
        setNFlag(dest);
        tcg_gen_movi_tl(temp_10, 0);
        tcg_gen_movi_tl(temp_9, 0);
        extractBits(temp_8, lsrc, temp_9, temp_10);
        tcg_gen_mov_tl(temp_7, temp_8);
        setCFlag(temp_7);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(lsrc);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_7);

    return ret;
}


/*
 * SEXB
 *    Variables: @dest, @src
 *    Functions: getCCFlag, arithmeticShiftRight, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       @dest = arithmeticShiftRight ((@src << 24), 24);
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (@dest);
 *           setNFlag (@dest);
 *         };
 *     };
 * }
 */

int
arc_gen_SEXB(DisasCtxt *ctx, TCGv dest, TCGv src)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_movi_tl(temp_6, 24);
    tcg_gen_shli_tl(temp_5, src, 24);
    arithmeticShiftRight(temp_4, temp_5, temp_6);
    tcg_gen_mov_tl(dest, temp_4);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(dest);
        setNFlag(dest);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);

    return ret;
}


/*
 * SEXH
 *    Variables: @dest, @src
 *    Functions: getCCFlag, arithmeticShiftRight, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       @dest = arithmeticShiftRight ((@src << 16), 16);
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (@dest);
 *           setNFlag (@dest);
 *         };
 *     };
 * }
 */

int
arc_gen_SEXH(DisasCtxt *ctx, TCGv dest, TCGv src)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_movi_tl(temp_6, 16);
    tcg_gen_shli_tl(temp_5, src, 16);
    arithmeticShiftRight(temp_4, temp_5, temp_6);
    tcg_gen_mov_tl(dest, temp_4);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(dest);
        setNFlag(dest);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);

    return ret;
}


/*
 * EXTB
 *    Variables: @dest, @src
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       @dest = (@src & 255);
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (@dest);
 *           setNFlag (@dest);
 *         };
 *     };
 * }
 */

int
arc_gen_EXTB(DisasCtxt *ctx, TCGv dest, TCGv src)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_andi_tl(dest, src, 255);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(dest);
        setNFlag(dest);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);

    return ret;
}


/*
 * EXTH
 *    Variables: @dest, @src
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       @dest = (@src & 65535);
 *       f_flag = getFFlag ();
 *       if((f_flag == true))
 *         {
 *           setZFlag (@dest);
 *           setNFlag (@dest);
 *         };
 *     };
 * }
 */

int
arc_gen_EXTH(DisasCtxt *ctx, TCGv dest, TCGv src)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    int f_flag;
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_andi_tl(dest, src, 65535);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(dest);
        setNFlag(dest);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);

    return ret;
}


/*
 * BTST
 *    Variables: @c, @b
 *    Functions: getCCFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       tmp = (1 << (@c & 31));
 *       alu = (@b & tmp);
 *       setZFlag (alu);
 *       setNFlag (alu);
 *     };
 * }
 */

int
arc_gen_BTST(DisasCtxt *ctx, TCGv c, TCGv b)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv tmp = tcg_temp_local_new();
    TCGv alu = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_andi_tl(temp_4, c, 31);
    tcg_gen_shlfi_tl(tmp, 1, temp_4);
    tcg_gen_and_tl(alu, b, tmp);
    setZFlag(alu);
    setNFlag(alu);
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_4);
    tcg_temp_free(tmp);
    tcg_temp_free(alu);

    return ret;
}


/*
 * TST
 *    Variables: @b, @c
 *    Functions: getCCFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       alu = (@b & @c);
 *       setZFlag (alu);
 *       setNFlag (alu);
 *     };
 * }
 */

int
arc_gen_TST(DisasCtxt *ctx, TCGv b, TCGv c)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv alu = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_and_tl(alu, b, c);
    setZFlag(alu);
    setNFlag(alu);
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(alu);

    return ret;
}


/*
 * XBFU
 *    Variables: @src2, @src1, @dest
 *    Functions: getCCFlag, extractBits, getFFlag, setZFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       N = extractBits (@src2, 4, 0);
 *       M = (extractBits (@src2, 9, 5) + 1);
 *       tmp1 = (@src1 >> N);
 *       tmp2 = ((1 << M) - 1);
 *       @dest = (tmp1 & tmp2);
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (@dest);
 *         };
 *     };
 * }
 */

int
arc_gen_XBFU(DisasCtxt *ctx, TCGv src2, TCGv src1, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv N = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv M = tcg_temp_local_new();
    TCGv tmp1 = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    TCGv tmp2 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_movi_tl(temp_6, 0);
    tcg_gen_movi_tl(temp_5, 4);
    extractBits(temp_4, src2, temp_5, temp_6);
    tcg_gen_mov_tl(N, temp_4);
    tcg_gen_movi_tl(temp_10, 5);
    tcg_gen_movi_tl(temp_9, 9);
    extractBits(temp_8, src2, temp_9, temp_10);
    tcg_gen_mov_tl(temp_7, temp_8);
    tcg_gen_addi_tl(M, temp_7, 1);
    tcg_gen_shr_tl(tmp1, src1, N);
    tcg_gen_shlfi_tl(temp_11, 1, M);
    tcg_gen_subi_tl(tmp2, temp_11, 1);
    tcg_gen_and_tl(dest, tmp1, tmp2);
    if ((getFFlag () == true)) {
        setZFlag(dest);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(N);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_7);
    tcg_temp_free(M);
    tcg_temp_free(tmp1);
    tcg_temp_free(temp_11);
    tcg_temp_free(tmp2);

    return ret;
}


/*
 * AEX
 *    Variables: @src2, @b
 *    Functions: getCCFlag, readAuxReg, writeAuxReg
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       tmp = readAuxReg (@src2);
 *       writeAuxReg (@src2, @b);
 *       @b = tmp;
 *     };
 * }
 */

int
arc_gen_AEX(DisasCtxt *ctx, TCGv src2, TCGv b)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv tmp = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    readAuxReg(temp_4, src2);
    tcg_gen_mov_tl(tmp, temp_4);
    writeAuxReg(src2, b);
    tcg_gen_mov_tl(b, tmp);
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_4);
    tcg_temp_free(tmp);

    return ret;
}


/*
 * LR
 *    Variables: @dest, @src
 *    Functions: readAuxReg
 * --- code ---
 * {
 *   @dest = readAuxReg (@src);
 * }
 */

int
arc_gen_LR(DisasCtxt *ctx, TCGv dest, TCGv src)
{
    int ret = DISAS_NORETURN;

    if (tb_cflags(ctx->base.tb) & CF_USE_ICOUNT) {
	gen_io_start();
    }

    TCGv temp_1 = tcg_temp_local_new();
    readAuxReg(temp_1, src);
    tcg_gen_mov_tl(dest, temp_1);
    tcg_temp_free(temp_1);

    return ret;
}


/*
 * CLRI
 *    Variables: @c
 *    Functions: getRegister, setRegister
 * --- code ---
 * {
 *   in_kernel_mode = inKernelMode();
 *   if(in_kernel_mode != 1)
 *     {
 *       throwExcpPriviledgeV();
 *     }
 *   status32 = getRegister (R_STATUS32);
 *   ie = (status32 & 2147483648);
 *   ie = (ie >> 27);
 *   e = ((status32 & 30) >> 1);
 *   a = 32;
 *   @c = ((ie | e) | a);
 *   mask = 2147483648;
 *   mask = ~mask;
 *   status32 = (status32 & mask);
 *   setRegister (R_STATUS32, status32);
 * }
 */

int
arc_gen_CLRI(DisasCtxt *ctx, TCGv c)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv in_kernel_mode = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv status32 = tcg_temp_local_new();
    TCGv ie = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv e = tcg_temp_local_new();
    TCGv a = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv mask = tcg_temp_local_new();
    inKernelMode(temp_3);
    tcg_gen_mov_tl(in_kernel_mode, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcondi_tl(TCG_COND_NE, temp_1, in_kernel_mode, 1);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    throwExcpPriviledgeV();
    gen_set_label(done_1);
    getRegister(temp_4, R_STATUS32);
    tcg_gen_mov_tl(status32, temp_4);
    tcg_gen_andi_tl(ie, status32, 2147483648);
    tcg_gen_shri_tl(ie, ie, 27);
    tcg_gen_andi_tl(temp_5, status32, 30);
    tcg_gen_shri_tl(e, temp_5, 1);
    tcg_gen_movi_tl(a, 32);
    tcg_gen_or_tl(temp_6, ie, e);
    tcg_gen_or_tl(c, temp_6, a);
    tcg_gen_movi_tl(mask, 2147483648);
    tcg_gen_not_tl(mask, mask);
    tcg_gen_and_tl(status32, status32, mask);
    setRegister(R_STATUS32, status32);
    tcg_temp_free(temp_3);
    tcg_temp_free(in_kernel_mode);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_4);
    tcg_temp_free(status32);
    tcg_temp_free(ie);
    tcg_temp_free(temp_5);
    tcg_temp_free(e);
    tcg_temp_free(a);
    tcg_temp_free(temp_6);
    tcg_temp_free(mask);

    return ret;
}


/*
 * SETI
 *    Variables: @c
 *    Functions: getRegister, setRegister
 * --- code ---
 * {
 *   in_kernel_mode = inKernelMode();
 *   if(in_kernel_mode != 1)
 *     {
 *       throwExcpPriviledgeV();
 *     }
 *   status32 = getRegister (R_STATUS32);
 *   e_mask = 30;
 *   e_mask = ~e_mask;
 *   e_value = ((@c & 15) << 1);
 *   temp1 = (@c & 32);
 *   if((temp1 != 0))
 *     {
 *       status32 = ((status32 & e_mask) | e_value);
 *       ie_mask = 2147483648;
 *       ie_mask = ~ie_mask;
 *       ie_value = ((@c & 16) << 27);
 *       status32 = ((status32 & ie_mask) | ie_value);
 *     }
 *   else
 *     {
 *       status32 = (status32 | 2147483648);
 *       temp2 = (@c & 16);
 *       if((temp2 != 0))
 *         {
 *           status32 = ((status32 & e_mask) | e_value);
 *         };
 *     };
 *   setRegister (R_STATUS32, status32);
 * }
 */

int
arc_gen_SETI(DisasCtxt *ctx, TCGv c)
{
    int ret = DISAS_NEXT;
    TCGv temp_7 = tcg_temp_local_new();
    TCGv in_kernel_mode = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv status32 = tcg_temp_local_new();
    TCGv e_mask = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv e_value = tcg_temp_local_new();
    TCGv temp1 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv ie_mask = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    TCGv ie_value = tcg_temp_local_new();
    TCGv temp_12 = tcg_temp_local_new();
    TCGv temp2 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_13 = tcg_temp_local_new();
    inKernelMode(temp_7);
    tcg_gen_mov_tl(in_kernel_mode, temp_7);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcondi_tl(TCG_COND_NE, temp_1, in_kernel_mode, 1);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    throwExcpPriviledgeV();
    gen_set_label(done_1);
    getRegister(temp_8, R_STATUS32);
    tcg_gen_mov_tl(status32, temp_8);
    tcg_gen_movi_tl(e_mask, 30);
    tcg_gen_not_tl(e_mask, e_mask);
    tcg_gen_andi_tl(temp_9, c, 15);
    tcg_gen_shli_tl(e_value, temp_9, 1);
    tcg_gen_andi_tl(temp1, c, 32);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcondi_tl(TCG_COND_NE, temp_3, temp1, 0);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    tcg_gen_and_tl(temp_10, status32, e_mask);
    tcg_gen_or_tl(status32, temp_10, e_value);
    tcg_gen_movi_tl(ie_mask, 2147483648);
    tcg_gen_not_tl(ie_mask, ie_mask);
    tcg_gen_andi_tl(temp_11, c, 16);
    tcg_gen_shli_tl(ie_value, temp_11, 27);
    tcg_gen_and_tl(temp_12, status32, ie_mask);
    tcg_gen_or_tl(status32, temp_12, ie_value);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    tcg_gen_ori_tl(status32, status32, 2147483648);
    tcg_gen_andi_tl(temp2, c, 16);
    TCGLabel *done_3 = gen_new_label();
    tcg_gen_setcondi_tl(TCG_COND_NE, temp_5, temp2, 0);
    tcg_gen_xori_tl(temp_6, temp_5, 1);
    tcg_gen_andi_tl(temp_6, temp_6, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_6, arc_true, done_3);
    tcg_gen_and_tl(temp_13, status32, e_mask);
    tcg_gen_or_tl(status32, temp_13, e_value);
    gen_set_label(done_3);
    gen_set_label(done_2);
    setRegister(R_STATUS32, status32);
    tcg_temp_free(temp_7);
    tcg_temp_free(in_kernel_mode);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_8);
    tcg_temp_free(status32);
    tcg_temp_free(e_mask);
    tcg_temp_free(temp_9);
    tcg_temp_free(e_value);
    tcg_temp_free(temp1);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_10);
    tcg_temp_free(ie_mask);
    tcg_temp_free(temp_11);
    tcg_temp_free(ie_value);
    tcg_temp_free(temp_12);
    tcg_temp_free(temp2);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_13);

    return ret;
}


/*
 * NOP
 *    Variables:
 *    Functions: doNothing
 * --- code ---
 * {
 *   doNothing ();
 * }
 */

int
arc_gen_NOP(DisasCtxt *ctx)
{
    int ret = DISAS_NEXT;

    return ret;
}


/*
 * PREALLOC
 *    Variables:
 *    Functions: doNothing
 * --- code ---
 * {
 *   doNothing ();
 * }
 */

int
arc_gen_PREALLOC(DisasCtxt *ctx)
{
    int ret = DISAS_NEXT;

    return ret;
}


/*
 * PREFETCH
 *    Variables: @src1, @src2
 *    Functions: getAAFlag, doNothing
 * --- code ---
 * {
 *   AA = getAAFlag ();
 *   if(((AA == 1) || (AA == 2)))
 *     {
 *       @src1 = (@src1 + @src2);
 *     }
 *   else
 *     {
 *       doNothing ();
 *     };
 * }
 */

int
arc_gen_PREFETCH(DisasCtxt *ctx, TCGv src1, TCGv src2)
{
    int ret = DISAS_NEXT;
    int AA;
    AA = getAAFlag ();
    if (((AA == 1) || (AA == 2))) {
        tcg_gen_add_tl(src1, src1, src2);
    } else {
        doNothing();
    }

    return ret;
}


/*
 * MPY
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getFFlag, HELPER, setZFlag, setNFlag, setVFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       _b = @b;
 *       _c = @c;
 *       @a = ((_b * _c) & 4294967295);
 *       if((getFFlag () == true))
 *         {
 *           high_part = HELPER (mpym, _b, _c);
 *           tmp1 = (high_part & 2147483648);
 *	     tmp2 = @a >> 31;
 *           setZFlag (@a);
 *           setNFlag (high_part);
 *           setVFlag ((tmp1 != tmp2));
 *         };
 *     };
 * }
 */

int
arc_gen_MPY(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv _b = tcg_temp_local_new();
    TCGv _c = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv high_part = tcg_temp_local_new();
    TCGv tmp1 = tcg_temp_local_new();
    TCGv tmp2 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(_b, b);
    tcg_gen_mov_tl(_c, c);
    tcg_gen_mul_tl(temp_4, _b, _c);
    tcg_gen_andi_tl(a, temp_4, 4294967295);
    if ((getFFlag () == true)) {
        ARC_HELPER(mpym, high_part, _b, _c);
        tcg_gen_sari_tl(tmp2, a, 31);
        setZFlag(a);
        setNFlag(high_part);
        tcg_gen_setcond_tl(TCG_COND_NE, temp_5, high_part, tmp2);
        setVFlag(temp_5);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(_b);
    tcg_temp_free(_c);
    tcg_temp_free(temp_4);
    tcg_temp_free(high_part);
    tcg_temp_free(tmp1);
    tcg_temp_free(tmp2);
    tcg_temp_free(temp_5);

    return ret;
}


/*
 * MPYMU
 *    Variables: @a, @b, @c
 *    Functions: getCCFlag, HELPER, getFFlag, setZFlag, setNFlag, setVFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       @a = HELPER (mpymu, @b, @c);
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (@a);
 *           setNFlag (0);
 *           setVFlag (0);
 *         };
 *     };
 * }
 */

int
arc_gen_MPYMU(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    ARC_HELPER(mpymu, a, b, c);
    if ((getFFlag () == true)) {
        setZFlag(a);
        tcg_gen_movi_tl(temp_4, 0);
        setNFlag(temp_4);
        tcg_gen_movi_tl(temp_5, 0);
        setVFlag(temp_5);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);

    return ret;
}


/*
 * MPYM
 *    Variables: @a, @b, @c
 *    Functions: getCCFlag, HELPER, getFFlag, setZFlag, setNFlag, setVFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       @a = HELPER (mpym, @b, @c);
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (@a);
 *           setNFlag (@a);
 *           setVFlag (0);
 *         };
 *     };
 * }
 */

int
arc_gen_MPYM(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    ARC_HELPER(mpym, a, b, c);
    if ((getFFlag () == true)) {
        setZFlag(a);
        setNFlag(a);
        tcg_gen_movi_tl(temp_4, 0);
        setVFlag(temp_4);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_4);

    return ret;
}


/*
 * MPYU
 *    Variables: @a, @b, @c
 *    Functions: getCCFlag, getFFlag, HELPER, setZFlag, setNFlag, setVFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       _b = @b;
 *       _c = @c;
 *       @a = ((_b * _c) & 4294967295);
 *       if((getFFlag () == true))
 *         {
 *           high_part = HELPER (mpymu, _b, _c);
 *           setZFlag (@a);
 *           setNFlag (0);
 *           setVFlag ((high_part != 0));
 *         };
 *     };
 * }
 */

int
arc_gen_MPYU(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv _b = tcg_temp_local_new();
    TCGv _c = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv high_part = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(_b, b);
    tcg_gen_mov_tl(_c, c);
    tcg_gen_mul_tl(temp_4, _b, _c);
    tcg_gen_andi_tl(a, temp_4, 4294967295);
    if ((getFFlag () == true)) {
        ARC_HELPER(mpymu, high_part, _b, _c);
        setZFlag(a);
        tcg_gen_movi_tl(temp_5, 0);
        setNFlag(temp_5);
        tcg_gen_setcondi_tl(TCG_COND_NE, temp_6, high_part, 0);
        setVFlag(temp_6);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(_b);
    tcg_temp_free(_c);
    tcg_temp_free(temp_4);
    tcg_temp_free(high_part);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * MPYUW
 *    Variables: @a, @b, @c
 *    Functions: getCCFlag, getFFlag, setZFlag, setNFlag, setVFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       @a = ((@b & 65535) * (@c & 65535));
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (@a);
 *           setNFlag (0);
 *           setVFlag (0);
 *         };
 *     };
 * }
 */

int
arc_gen_MPYUW(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_andi_tl(temp_5, c, 65535);
    tcg_gen_andi_tl(temp_4, b, 65535);
    tcg_gen_mul_tl(a, temp_4, temp_5);
    if ((getFFlag () == true)) {
        setZFlag(a);
        tcg_gen_movi_tl(temp_6, 0);
        setNFlag(temp_6);
        tcg_gen_movi_tl(temp_7, 0);
        setVFlag(temp_7);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_7);

    return ret;
}


/*
 * MPYW
 *    Variables: @a, @b, @c
 *    Functions: getCCFlag, arithmeticShiftRight, getFFlag, setZFlag, setNFlag,
 *               setVFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       @a = (arithmeticShiftRight ((@b << 16), 16)
 *            * arithmeticShiftRight ((@c << 16), 16));
 *       if((getFFlag () == true))
 *         {
 *           setZFlag (@a);
 *           setNFlag (@a);
 *           setVFlag (0);
 *         };
 *     };
 * }
 */

int
arc_gen_MPYW(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_12 = tcg_temp_local_new();
    getCCFlag(temp_3);
    tcg_gen_mov_tl(cc_flag, temp_3);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_movi_tl(temp_11, 16);
    tcg_gen_shli_tl(temp_10, c, 16);
    tcg_gen_movi_tl(temp_7, 16);
    tcg_gen_shli_tl(temp_6, b, 16);
    arithmeticShiftRight(temp_5, temp_6, temp_7);
    tcg_gen_mov_tl(temp_4, temp_5);
    arithmeticShiftRight(temp_9, temp_10, temp_11);
    tcg_gen_mov_tl(temp_8, temp_9);
    tcg_gen_mul_tl(a, temp_4, temp_8);
    if ((getFFlag () == true)) {
        setZFlag(a);
        setNFlag(a);
        tcg_gen_movi_tl(temp_12, 0);
        setVFlag(temp_12);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_11);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_12);

    return ret;
}


/*
 * DIV
 *    Variables: @src2, @src1, @dest
 *    Functions: getCCFlag, divSigned, getFFlag, setZFlag, setNFlag, setVFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       if(((@src2 != 0) && ((@src1 != 2147483648) || (@src2 != 4294967295))))
 *         {
 *           @dest = divSigned (@src1, @src2);
 *           if((getFFlag () == true))
 *             {
 *               setZFlag (@dest);
 *               setNFlag (@dest);
 *               setVFlag (0);
 *             };
 *         }
 *       else
 *         {
 *         };
 *     };
 * }
 */

int
arc_gen_DIV(DisasCtxt *ctx, TCGv src2, TCGv src1, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv temp_9 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    getCCFlag(temp_9);
    tcg_gen_mov_tl(cc_flag, temp_9);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcondi_tl(TCG_COND_NE, temp_3, src2, 0);
    tcg_gen_setcondi_tl(TCG_COND_NE, temp_4, src1, 2147483648);
    tcg_gen_setcondi_tl(TCG_COND_NE, temp_5, src2, 4294967295);
    tcg_gen_or_tl(temp_6, temp_4, temp_5);
    tcg_gen_and_tl(temp_7, temp_3, temp_6);
    tcg_gen_xori_tl(temp_8, temp_7, 1);
    tcg_gen_andi_tl(temp_8, temp_8, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_8, arc_true, else_2);
    divSigned(temp_10, src1, src2);
    tcg_gen_mov_tl(dest, temp_10);
    if ((getFFlag () == true)) {
        setZFlag(dest);
        setNFlag(dest);
        tcg_gen_movi_tl(temp_11, 0);
        setVFlag(temp_11);
    }
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    gen_set_label(done_2);
    gen_set_label(done_1);
    tcg_temp_free(temp_9);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_11);

    return ret;
}


/*
 * DIVU
 *    Variables: @src2, @dest, @src1
 *    Functions: getCCFlag, divUnsigned, getFFlag, setZFlag, setNFlag,
 *               setVFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       if((@src2 != 0))
 *         {
 *           @dest = divUnsigned (@src1, @src2);
 *           if((getFFlag () == true))
 *             {
 *               setZFlag (@dest);
 *               setNFlag (0);
 *               setVFlag (0);
 *             };
 *         }
 *       else
 *         {
 *         };
 *     };
 * }
 */

int
arc_gen_DIVU(DisasCtxt *ctx, TCGv src2, TCGv dest, TCGv src1)
{
    int ret = DISAS_NEXT;
    TCGv temp_5 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    getCCFlag(temp_5);
    tcg_gen_mov_tl(cc_flag, temp_5);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcondi_tl(TCG_COND_NE, temp_3, src2, 0);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    divUnsigned(temp_6, src1, src2);
    tcg_gen_mov_tl(dest, temp_6);
    if ((getFFlag () == true)) {
        setZFlag(dest);
        tcg_gen_movi_tl(temp_7, 0);
        setNFlag(temp_7);
        tcg_gen_movi_tl(temp_8, 0);
        setVFlag(temp_8);
    }
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    gen_set_label(done_2);
    gen_set_label(done_1);
    tcg_temp_free(temp_5);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_8);

    return ret;
}


/*
 * REM
 *    Variables: @src2, @src1, @dest
 *    Functions: getCCFlag, divRemainingSigned, getFFlag, setZFlag, setNFlag,
 *               setVFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       if(((@src2 != 0) && ((@src1 != 2147483648) || (@src2 != 4294967295))))
 *         {
 *           @dest = divRemainingSigned (@src1, @src2);
 *           if((getFFlag () == true))
 *             {
 *               setZFlag (@dest);
 *               setNFlag (@dest);
 *               setVFlag (0);
 *             };
 *         }
 *       else
 *         {
 *         };
 *     };
 * }
 */

int
arc_gen_REM(DisasCtxt *ctx, TCGv src2, TCGv src1, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv temp_9 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    getCCFlag(temp_9);
    tcg_gen_mov_tl(cc_flag, temp_9);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcondi_tl(TCG_COND_NE, temp_3, src2, 0);
    tcg_gen_setcondi_tl(TCG_COND_NE, temp_4, src1, 2147483648);
    tcg_gen_setcondi_tl(TCG_COND_NE, temp_5, src2, 4294967295);
    tcg_gen_or_tl(temp_6, temp_4, temp_5);
    tcg_gen_and_tl(temp_7, temp_3, temp_6);
    tcg_gen_xori_tl(temp_8, temp_7, 1);
    tcg_gen_andi_tl(temp_8, temp_8, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_8, arc_true, else_2);
    divRemainingSigned(temp_10, src1, src2);
    tcg_gen_mov_tl(dest, temp_10);
    if ((getFFlag () == true)) {
        setZFlag(dest);
        setNFlag(dest);
        tcg_gen_movi_tl(temp_11, 0);
        setVFlag(temp_11);
    }
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    gen_set_label(done_2);
    gen_set_label(done_1);
    tcg_temp_free(temp_9);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_11);

    return ret;
}


/*
 * REMU
 *    Variables: @src2, @dest, @src1
 *    Functions: getCCFlag, divRemainingUnsigned, getFFlag, setZFlag, setNFlag,
 *               setVFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       if((@src2 != 0))
 *         {
 *           @dest = divRemainingUnsigned (@src1, @src2);
 *           if((getFFlag () == true))
 *             {
 *               setZFlag (@dest);
 *               setNFlag (0);
 *               setVFlag (0);
 *             };
 *         }
 *       else
 *         {
 *         };
 *     };
 * }
 */

int
arc_gen_REMU(DisasCtxt *ctx, TCGv src2, TCGv dest, TCGv src1)
{
    int ret = DISAS_NEXT;
    TCGv temp_5 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    getCCFlag(temp_5);
    tcg_gen_mov_tl(cc_flag, temp_5);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcondi_tl(TCG_COND_NE, temp_3, src2, 0);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    divRemainingUnsigned(temp_6, src1, src2);
    tcg_gen_mov_tl(dest, temp_6);
    if ((getFFlag () == true)) {
        setZFlag(dest);
        tcg_gen_movi_tl(temp_7, 0);
        setNFlag(temp_7);
        tcg_gen_movi_tl(temp_8, 0);
        setVFlag(temp_8);
    }
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    gen_set_label(done_2);
    gen_set_label(done_1);
    tcg_temp_free(temp_5);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_8);

    return ret;
}


/*
 * MAC
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getRegister, MAC, getFFlag, setNFlag, OverflowADD,
 *               setVFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       old_acchi = getRegister (R_ACCHI);
 *       high_mul = MAC (@b, @c);
 *       @a = getRegister (R_ACCLO);
 *       if((getFFlag () == true))
 *         {
 *           new_acchi = getRegister (R_ACCHI);
 *           setNFlag (new_acchi);
 *           if((OverflowADD (new_acchi, old_acchi, high_mul) == true))
 *             {
 *               setVFlag (1);
 *             };
 *         };
 *     };
 * }
 */

int
arc_gen_MAC(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_5 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv old_acchi = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv high_mul = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv new_acchi = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    getCCFlag(temp_5);
    tcg_gen_mov_tl(cc_flag, temp_5);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    getRegister(temp_6, R_ACCHI);
    tcg_gen_mov_tl(old_acchi, temp_6);
    MAC(temp_7, b, c);
    tcg_gen_mov_tl(high_mul, temp_7);
    getRegister(temp_8, R_ACCLO);
    tcg_gen_mov_tl(a, temp_8);
    if ((getFFlag () == true)) {
        getRegister(temp_9, R_ACCHI);
        tcg_gen_mov_tl(new_acchi, temp_9);
        setNFlag(new_acchi);
        TCGLabel *done_2 = gen_new_label();
        OverflowADD(temp_10, new_acchi, old_acchi, high_mul);
        tcg_gen_setcond_tl(TCG_COND_EQ, temp_3, temp_10, arc_true);
        tcg_gen_xori_tl(temp_4, temp_3, 1);
        tcg_gen_andi_tl(temp_4, temp_4, 1);
        tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, done_2);
        tcg_gen_movi_tl(temp_11, 1);
        setVFlag(temp_11);
        gen_set_label(done_2);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_5);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_6);
    tcg_temp_free(old_acchi);
    tcg_temp_free(temp_7);
    tcg_temp_free(high_mul);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_9);
    tcg_temp_free(new_acchi);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_11);

    return ret;
}


/*
 * MACU
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getRegister, MACU, getFFlag, CarryADD, setVFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       old_acchi = getRegister (R_ACCHI);
 *       high_mul = MACU (@b, @c);
 *       @a = getRegister (R_ACCLO);
 *       if((getFFlag () == true))
 *         {
 *           new_acchi = getRegister (R_ACCHI);
 *           if((CarryADD (new_acchi, old_acchi, high_mul) == true))
 *             {
 *               setVFlag (1);
 *             };
 *         };
 *     };
 * }
 */

int
arc_gen_MACU(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_5 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv old_acchi = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv high_mul = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv new_acchi = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    getCCFlag(temp_5);
    tcg_gen_mov_tl(cc_flag, temp_5);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    getRegister(temp_6, R_ACCHI);
    tcg_gen_mov_tl(old_acchi, temp_6);
    MACU(temp_7, b, c);
    tcg_gen_mov_tl(high_mul, temp_7);
    getRegister(temp_8, R_ACCLO);
    tcg_gen_mov_tl(a, temp_8);
    if ((getFFlag () == true)) {
        getRegister(temp_9, R_ACCHI);
        tcg_gen_mov_tl(new_acchi, temp_9);
        TCGLabel *done_2 = gen_new_label();
        CarryADD(temp_10, new_acchi, old_acchi, high_mul);
        tcg_gen_setcond_tl(TCG_COND_EQ, temp_3, temp_10, arc_true);
        tcg_gen_xori_tl(temp_4, temp_3, 1);
        tcg_gen_andi_tl(temp_4, temp_4, 1);
        tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, done_2);
        tcg_gen_movi_tl(temp_11, 1);
        setVFlag(temp_11);
        gen_set_label(done_2);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_5);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_6);
    tcg_temp_free(old_acchi);
    tcg_temp_free(temp_7);
    tcg_temp_free(high_mul);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_9);
    tcg_temp_free(new_acchi);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_11);

    return ret;
}


/*
 * MACD
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getRegister, MAC, nextReg, getFFlag, setNFlag,
 *               OverflowADD, setVFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       old_acchi = getRegister (R_ACCHI);
 *       high_mul = MAC (@b, @c);
 *       @a = getRegister (R_ACCLO);
 *       pair = nextReg (a);
 *       pair = getRegister (R_ACCHI);
 *       if((getFFlag () == true))
 *         {
 *           new_acchi = getRegister (R_ACCHI);
 *           setNFlag (new_acchi);
 *           if((OverflowADD (new_acchi, old_acchi, high_mul) == true))
 *             {
 *               setVFlag (1);
 *             };
 *         };
 *     };
 * }
 */

int
arc_gen_MACD(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_5 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv old_acchi = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv high_mul = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv pair = NULL;
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv new_acchi = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_12 = tcg_temp_local_new();
    getCCFlag(temp_5);
    tcg_gen_mov_tl(cc_flag, temp_5);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    getRegister(temp_6, R_ACCHI);
    tcg_gen_mov_tl(old_acchi, temp_6);
    MAC(temp_7, b, c);
    tcg_gen_mov_tl(high_mul, temp_7);
    getRegister(temp_8, R_ACCLO);
    tcg_gen_mov_tl(a, temp_8);
    pair = nextReg (a);
    getRegister(temp_9, R_ACCHI);
    tcg_gen_mov_tl(pair, temp_9);
    if ((getFFlag () == true)) {
        getRegister(temp_10, R_ACCHI);
        tcg_gen_mov_tl(new_acchi, temp_10);
        setNFlag(new_acchi);
        TCGLabel *done_2 = gen_new_label();
        OverflowADD(temp_11, new_acchi, old_acchi, high_mul);
        tcg_gen_setcond_tl(TCG_COND_EQ, temp_3, temp_11, arc_true);
        tcg_gen_xori_tl(temp_4, temp_3, 1);
        tcg_gen_andi_tl(temp_4, temp_4, 1);
        tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, done_2);
        tcg_gen_movi_tl(temp_12, 1);
        setVFlag(temp_12);
        gen_set_label(done_2);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_5);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_6);
    tcg_temp_free(old_acchi);
    tcg_temp_free(temp_7);
    tcg_temp_free(high_mul);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_10);
    tcg_temp_free(new_acchi);
    tcg_temp_free(temp_11);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_12);

    return ret;
}


/*
 * MACDU
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag, getRegister, MACU, nextReg, getFFlag, CarryADD,
 *               setVFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       old_acchi = getRegister (R_ACCHI);
 *       high_mul = MACU (@b, @c);
 *       @a = getRegister (R_ACCLO);
 *       pair = nextReg (a);
 *       pair = getRegister (R_ACCHI);
 *       if((getFFlag () == true))
 *         {
 *           new_acchi = getRegister (R_ACCHI);
 *           if((CarryADD (new_acchi, old_acchi, high_mul) == true))
 *             {
 *               setVFlag (1);
 *             };
 *         };
 *     };
 * }
 */

int
arc_gen_MACDU(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_5 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv old_acchi = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv high_mul = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv pair = NULL;
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv new_acchi = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_12 = tcg_temp_local_new();
    getCCFlag(temp_5);
    tcg_gen_mov_tl(cc_flag, temp_5);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    getRegister(temp_6, R_ACCHI);
    tcg_gen_mov_tl(old_acchi, temp_6);
    MACU(temp_7, b, c);
    tcg_gen_mov_tl(high_mul, temp_7);
    getRegister(temp_8, R_ACCLO);
    tcg_gen_mov_tl(a, temp_8);
    pair = nextReg (a);
    getRegister(temp_9, R_ACCHI);
    tcg_gen_mov_tl(pair, temp_9);
    if ((getFFlag () == true)) {
        getRegister(temp_10, R_ACCHI);
        tcg_gen_mov_tl(new_acchi, temp_10);
        TCGLabel *done_2 = gen_new_label();
        CarryADD(temp_11, new_acchi, old_acchi, high_mul);
        tcg_gen_setcond_tl(TCG_COND_EQ, temp_3, temp_11, arc_true);
        tcg_gen_xori_tl(temp_4, temp_3, 1);
        tcg_gen_andi_tl(temp_4, temp_4, 1);
        tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, done_2);
        tcg_gen_movi_tl(temp_12, 1);
        setVFlag(temp_12);
        gen_set_label(done_2);
    }
    gen_set_label(done_1);
    tcg_temp_free(temp_5);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_6);
    tcg_temp_free(old_acchi);
    tcg_temp_free(temp_7);
    tcg_temp_free(high_mul);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_10);
    tcg_temp_free(new_acchi);
    tcg_temp_free(temp_11);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_12);

    return ret;
}


/*
 * ABS
 *    Variables: @src, @dest
 *    Functions: Carry, getFFlag, setZFlag, setNFlag, setCFlag, Zero, setVFlag,
 *               getNFlag
 * --- code ---
 * {
 *   lsrc = @src;
 *   alu = (0 - lsrc);
 *   if((Carry (lsrc) == 1))
 *     {
 *       @dest = alu;
 *     }
 *   else
 *     {
 *       @dest = lsrc;
 *     };
 *   if((getFFlag () == true))
 *     {
 *       setZFlag (@dest);
 *       setNFlag (@dest);
 *       setCFlag (Zero ());
 *       setVFlag (getNFlag ());
 *     };
 * }
 */

int
arc_gen_ABS(DisasCtxt *ctx, TCGv src, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv lsrc = tcg_temp_local_new();
    TCGv alu = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    tcg_gen_mov_tl(lsrc, src);
    tcg_gen_subfi_tl(alu, 0, lsrc);
    TCGLabel *else_1 = gen_new_label();
    TCGLabel *done_1 = gen_new_label();
    Carry(temp_3, lsrc);
    tcg_gen_setcondi_tl(TCG_COND_EQ, temp_1, temp_3, 1);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, else_1);
    tcg_gen_mov_tl(dest, alu);
    tcg_gen_br(done_1);
    gen_set_label(else_1);
    tcg_gen_mov_tl(dest, lsrc);
    gen_set_label(done_1);
    if ((getFFlag () == true)) {
        setZFlag(dest);
        setNFlag(dest);
        tcg_gen_mov_tl(temp_4, Zero());
        setCFlag(temp_4);
        tcg_gen_mov_tl(temp_5, getNFlag());
        setVFlag(temp_5);
    }
    tcg_temp_free(lsrc);
    tcg_temp_free(alu);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);

    return ret;
}


/*
 * SWAP
 *    Variables: @src, @dest
 *    Functions: getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   tmp1 = (@src << 16);
 *   tmp2 = ((@src >> 16) & 65535);
 *   @dest = (tmp1 | tmp2);
 *   f_flag = getFFlag ();
 *   if((f_flag == true))
 *     {
 *       setZFlag (@dest);
 *       setNFlag (@dest);
 *     };
 * }
 */

int
arc_gen_SWAP(DisasCtxt *ctx, TCGv src, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv tmp1 = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv tmp2 = tcg_temp_local_new();
    int f_flag;
    tcg_gen_shli_tl(tmp1, src, 16);
    tcg_gen_shri_tl(temp_1, src, 16);
    tcg_gen_andi_tl(tmp2, temp_1, 65535);
    tcg_gen_or_tl(dest, tmp1, tmp2);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(dest);
        setNFlag(dest);
    }
    tcg_temp_free(tmp1);
    tcg_temp_free(temp_1);
    tcg_temp_free(tmp2);

    return ret;
}


/*
 * SWAPE
 *    Variables: @src, @dest
 *    Functions: getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   tmp1 = ((@src << 24) & 4278190080);
 *   tmp2 = ((@src << 8) & 16711680);
 *   tmp3 = ((@src >> 8) & 65280);
 *   tmp4 = ((@src >> 24) & 255);
 *   @dest = (((tmp1 | tmp2) | tmp3) | tmp4);
 *   f_flag = getFFlag ();
 *   if((f_flag == true))
 *     {
 *       setZFlag (@dest);
 *       setNFlag (@dest);
 *     };
 * }
 */

int
arc_gen_SWAPE(DisasCtxt *ctx, TCGv src, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv temp_1 = tcg_temp_local_new();
    TCGv tmp1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv tmp2 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv tmp3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv tmp4 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    int f_flag;
    tcg_gen_shli_tl(temp_1, src, 24);
    tcg_gen_andi_tl(tmp1, temp_1, 4278190080);
    tcg_gen_shli_tl(temp_2, src, 8);
    tcg_gen_andi_tl(tmp2, temp_2, 16711680);
    tcg_gen_shri_tl(temp_3, src, 8);
    tcg_gen_andi_tl(tmp3, temp_3, 65280);
    tcg_gen_shri_tl(temp_4, src, 24);
    tcg_gen_andi_tl(tmp4, temp_4, 255);
    tcg_gen_or_tl(temp_6, tmp1, tmp2);
    tcg_gen_or_tl(temp_5, temp_6, tmp3);
    tcg_gen_or_tl(dest, temp_5, tmp4);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(dest);
        setNFlag(dest);
    }
    tcg_temp_free(temp_1);
    tcg_temp_free(tmp1);
    tcg_temp_free(temp_2);
    tcg_temp_free(tmp2);
    tcg_temp_free(temp_3);
    tcg_temp_free(tmp3);
    tcg_temp_free(temp_4);
    tcg_temp_free(tmp4);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_5);

    return ret;
}


/*
 * NOT
 *    Variables: @dest, @src
 *    Functions: getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   @dest = ~@src;
 *   f_flag = getFFlag ();
 *   if((f_flag == true))
 *     {
 *       setZFlag (@dest);
 *       setNFlag (@dest);
 *     };
 * }
 */

int
arc_gen_NOT(DisasCtxt *ctx, TCGv dest, TCGv src)
{
    int ret = DISAS_NEXT;
    int f_flag;
    tcg_gen_not_tl(dest, src);
    f_flag = getFFlag ();
    if ((f_flag == true)) {
        setZFlag(dest);
        setNFlag(dest);
    }
    return ret;
}


/*
 * BI
 *    Variables: @c
 *    Functions: setPC, getPCL
 * --- code ---
 * {
 *   setPC ((nextInsnAddress () + (@c << 2)));
 * }
 */

int
arc_gen_BI(DisasCtxt *ctx, TCGv c)
{
    int ret = DISAS_NEXT;
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    tcg_gen_shli_tl(temp_4, c, 2);
    nextInsnAddress(temp_3);
    tcg_gen_mov_tl(temp_2, temp_3);
    tcg_gen_add_tl(temp_1, temp_2, temp_4);
    setPC(temp_1);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_1);

    return ret;
}


/*
 * BIH
 *    Variables: @c
 *    Functions: setPC, getPCL
 * --- code ---
 * {
 *   setPC ((nextInsnAddress () + (@c << 1)));
 * }
 */

int
arc_gen_BIH(DisasCtxt *ctx, TCGv c)
{
    int ret = DISAS_NEXT;
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    tcg_gen_shli_tl(temp_4, c, 1);
    nextInsnAddress(temp_3);
    tcg_gen_mov_tl(temp_2, temp_3);
    tcg_gen_add_tl(temp_1, temp_2, temp_4);
    setPC(temp_1);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_1);

    return ret;
}


/*
 * B
 *    Variables: @rd
 *    Functions: getCCFlag, getPCL, shouldExecuteDelaySlot, executeDelaySlot,
 *               setPC
 * --- code ---
 * {
 *   take_branch = false;
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       take_branch = true;
 *     };
 *   bta = (getPCL () + @rd);
 *   if((shouldExecuteDelaySlot () == true))
 *     {
 *       executeDelaySlot (bta, take_branch);
 *     };
 *   if((cc_flag == true))
 *     {
 *       setPC (bta);
 *     };
 * }
 */

int
arc_gen_B(DisasCtxt *ctx, TCGv rd)
{
    int ret = DISAS_NEXT;
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv bta = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    tcg_gen_mov_tl(take_branch, arc_false);
    getCCFlag(temp_5);
    tcg_gen_mov_tl(cc_flag, temp_5);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(take_branch, arc_true);
    gen_set_label(done_1);
    getPCL(temp_7);
    tcg_gen_mov_tl(temp_6, temp_7);
    tcg_gen_add_tl(bta, temp_6, rd);
    if ((shouldExecuteDelaySlot () == true)) {
        executeDelaySlot(bta, take_branch);
    }
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_3, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, done_2);
    setPC(bta);
    gen_set_label(done_2);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_5);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);
    tcg_temp_free(bta);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);

    return ret;
}


/* DBNZ
 *    Variables: @a, @offset
 *    Functions: getPCL, setPC
--- code ---
{
  bta = getPCL() + @offset;
  @a = @a - 1
  if (shouldExecuteDelaySlot() == 1)
  {
      take_branch = true;
      if (@a == 0)
      {
          take_branch = false;
      };
      executeDelaySlot (bta, take_branch);
  };
  if(@a != 0) {
    setPC(getPCL () + @offset)
  }
}
 */

int
arc_gen_DBNZ (DisasCtxt *ctx, TCGv a, TCGv offset)
{
    int ret = DISAS_NEXT;
    TCGLabel *do_not_branch = gen_new_label();
    TCGLabel *keep_take_branch_1 = gen_new_label();
    TCGv bta = tcg_temp_local_new();

    getPCL(bta);
    tcg_gen_add_tl(bta, bta, offset);
    tcg_gen_subi_tl(a, a, 1);

    if (shouldExecuteDelaySlot() == 1) {
        TCGv take_branch = tcg_const_local_tl(1);
        tcg_gen_brcondi_tl(TCG_COND_NE, a, 0, keep_take_branch_1);
        tcg_temp_free(take_branch);
        tcg_gen_mov_tl(take_branch, tcg_const_local_tl(0));
        gen_set_label(keep_take_branch_1);
        executeDelaySlot(bta, take_branch);
        tcg_temp_free(take_branch);
    }

    tcg_gen_brcondi_tl(TCG_COND_EQ, a, 0, do_not_branch);
        setPC(bta);
    gen_set_label(do_not_branch);
    tcg_temp_free(bta);

  return ret;
}

/*
 * BBIT0
 *    Variables: @b, @c, @rd
 *    Functions: getCCFlag, getPCL, shouldExecuteDelaySlot, executeDelaySlot,
 *               setPC
 * --- code ---
 * {
 *   take_branch = false;
 *   cc_flag = getCCFlag ();
 *   p_b = @b;
 *   p_c = (@c & 31);
 *   tmp = (1 << p_c);
 *   if((cc_flag == true))
 *     {
 *       if(((p_b && tmp) == 0))
 *         {
 *           take_branch = true;
 *         };
 *     };
 *   bta = (getPCL () + @rd);
 *   if((shouldExecuteDelaySlot () == true))
 *     {
 *       executeDelaySlot (bta, take_branch);
 *     };
 *   if((cc_flag == true))
 *     {
 *       if(((p_b && tmp) == 0))
 *         {
 *           setPC (bta);
 *         };
 *     };
 * }
 */

int
arc_gen_BBIT0(DisasCtxt *ctx, TCGv b, TCGv c, TCGv rd)
{
    int ret = DISAS_NEXT;
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv p_b = tcg_temp_local_new();
    TCGv p_c = tcg_temp_local_new();
    TCGv tmp = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_13 = tcg_temp_local_new();
    TCGv temp_12 = tcg_temp_local_new();
    TCGv bta = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    tcg_gen_mov_tl(take_branch, arc_false);
    getCCFlag(temp_11);
    tcg_gen_mov_tl(cc_flag, temp_11);
    tcg_gen_mov_tl(p_b, b);
    tcg_gen_andi_tl(p_c, c, 31);
    tcg_gen_shlfi_tl(tmp, 1, p_c);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_and_tl(temp_3, p_b, tmp);
    tcg_gen_setcondi_tl(TCG_COND_EQ, temp_4, temp_3, 0);
    tcg_gen_xori_tl(temp_5, temp_4, 1);
    tcg_gen_andi_tl(temp_5, temp_5, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_5, arc_true, done_2);
    tcg_gen_mov_tl(take_branch, arc_true);
    gen_set_label(done_2);
    gen_set_label(done_1);
    getPCL(temp_13);
    tcg_gen_mov_tl(temp_12, temp_13);
    tcg_gen_add_tl(bta, temp_12, rd);
    if ((shouldExecuteDelaySlot () == true)) {
        executeDelaySlot(bta, take_branch);
    }
    TCGLabel *done_3 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_6, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_7, temp_6, 1);
    tcg_gen_andi_tl(temp_7, temp_7, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_7, arc_true, done_3);
    TCGLabel *done_4 = gen_new_label();
    tcg_gen_and_tl(temp_8, p_b, tmp);
    tcg_gen_setcondi_tl(TCG_COND_EQ, temp_9, temp_8, 0);
    tcg_gen_xori_tl(temp_10, temp_9, 1);
    tcg_gen_andi_tl(temp_10, temp_10, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_10, arc_true, done_4);
    setPC(bta);
    gen_set_label(done_4);
    gen_set_label(done_3);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_11);
    tcg_temp_free(cc_flag);
    tcg_temp_free(p_b);
    tcg_temp_free(p_c);
    tcg_temp_free(tmp);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_13);
    tcg_temp_free(temp_12);
    tcg_temp_free(bta);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_10);

    return ret;
}


/*
 * BBIT1
 *    Variables: @b, @c, @rd
 *    Functions: getCCFlag, getPCL, shouldExecuteDelaySlot, executeDelaySlot,
 *               setPC
 * --- code ---
 * {
 *   take_branch = false;
 *   cc_flag = getCCFlag ();
 *   p_b = @b;
 *   p_c = (@c & 31);
 *   tmp = (1 << p_c);
 *   if((cc_flag == true))
 *     {
 *       if(((p_b && tmp) != 0))
 *         {
 *           take_branch = true;
 *         };
 *     };
 *   bta = (getPCL () + @rd);
 *   if((shouldExecuteDelaySlot () == true))
 *     {
 *       executeDelaySlot (bta, take_branch);
 *     };
 *   if((cc_flag == true))
 *     {
 *       if(((p_b && tmp) != 0))
 *         {
 *           setPC (bta);
 *         };
 *     };
 * }
 */

int
arc_gen_BBIT1(DisasCtxt *ctx, TCGv b, TCGv c, TCGv rd)
{
    int ret = DISAS_NEXT;
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv p_b = tcg_temp_local_new();
    TCGv p_c = tcg_temp_local_new();
    TCGv tmp = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_13 = tcg_temp_local_new();
    TCGv temp_12 = tcg_temp_local_new();
    TCGv bta = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    tcg_gen_mov_tl(take_branch, arc_false);
    getCCFlag(temp_11);
    tcg_gen_mov_tl(cc_flag, temp_11);
    tcg_gen_mov_tl(p_b, b);
    tcg_gen_andi_tl(p_c, c, 31);
    tcg_gen_shlfi_tl(tmp, 1, p_c);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_and_tl(temp_3, p_b, tmp);
    tcg_gen_setcondi_tl(TCG_COND_NE, temp_4, temp_3, 0);
    tcg_gen_xori_tl(temp_5, temp_4, 1);
    tcg_gen_andi_tl(temp_5, temp_5, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_5, arc_true, done_2);
    tcg_gen_mov_tl(take_branch, arc_true);
    gen_set_label(done_2);
    gen_set_label(done_1);
    getPCL(temp_13);
    tcg_gen_mov_tl(temp_12, temp_13);
    tcg_gen_add_tl(bta, temp_12, rd);
    if ((shouldExecuteDelaySlot () == true)) {
        executeDelaySlot(bta, take_branch);
    }
    TCGLabel *done_3 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_6, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_7, temp_6, 1);
    tcg_gen_andi_tl(temp_7, temp_7, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_7, arc_true, done_3);
    TCGLabel *done_4 = gen_new_label();
    tcg_gen_and_tl(temp_8, p_b, tmp);
    tcg_gen_setcondi_tl(TCG_COND_NE, temp_9, temp_8, 0);
    tcg_gen_xori_tl(temp_10, temp_9, 1);
    tcg_gen_andi_tl(temp_10, temp_10, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_10, arc_true, done_4);
    setPC(bta);
    gen_set_label(done_4);
    gen_set_label(done_3);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_11);
    tcg_temp_free(cc_flag);
    tcg_temp_free(p_b);
    tcg_temp_free(p_c);
    tcg_temp_free(tmp);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_13);
    tcg_temp_free(temp_12);
    tcg_temp_free(bta);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_10);

    return ret;
}


/*
 * BL
 *    Variables: @rd
 *    Functions: getCCFlag, getPCL, shouldExecuteDelaySlot, setBLINK,
 *               nextInsnAddressAfterDelaySlot, executeDelaySlot,
 *               nextInsnAddress, setPC
 * --- code ---
 * {
 *   take_branch = false;
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       take_branch = true;
 *     };
 *   bta = (getPCL () + @rd);
 *   if((shouldExecuteDelaySlot () == 1))
 *     {
 *       if(take_branch)
 *         {
 *           setBLINK (nextInsnAddressAfterDelaySlot ());
 *         };
 *       executeDelaySlot (bta, take_branch);
 *     }
 *   else
 *     {
 *       if(take_branch)
 *         {
 *           setBLINK (nextInsnAddress ());
 *         };
 *     };
 *   if((cc_flag == true))
 *     {
 *       setPC (bta);
 *     };
 * }
 */

int
arc_gen_BL(DisasCtxt *ctx, TCGv rd)
{
    int ret = DISAS_NEXT;
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv bta = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_13 = tcg_temp_local_new();
    TCGv temp_12 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    tcg_gen_mov_tl(take_branch, arc_false);
    getCCFlag(temp_7);
    tcg_gen_mov_tl(cc_flag, temp_7);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(take_branch, arc_true);
    gen_set_label(done_1);
    getPCL(temp_9);
    tcg_gen_mov_tl(temp_8, temp_9);
    tcg_gen_add_tl(bta, temp_8, rd);
    if ((shouldExecuteDelaySlot () == 1)) {
        TCGLabel *done_2 = gen_new_label();
        tcg_gen_xori_tl(temp_3, take_branch, 1);
        tcg_gen_andi_tl(temp_3, temp_3, 1);
        tcg_gen_brcond_tl(TCG_COND_EQ, temp_3, arc_true, done_2);
        nextInsnAddressAfterDelaySlot(temp_11);
        tcg_gen_mov_tl(temp_10, temp_11);
        setBLINK(temp_10);
        gen_set_label(done_2);
        executeDelaySlot(bta, take_branch);
    } else {
        TCGLabel *done_3 = gen_new_label();
        tcg_gen_xori_tl(temp_4, take_branch, 1);
        tcg_gen_andi_tl(temp_4, temp_4, 1);
        tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, done_3);
        nextInsnAddress(temp_13);
        tcg_gen_mov_tl(temp_12, temp_13);
        setBLINK(temp_12);
        gen_set_label(done_3);
    }
    TCGLabel *done_4 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_5, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_6, temp_5, 1);
    tcg_gen_andi_tl(temp_6, temp_6, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_6, arc_true, done_4);
    setPC(bta);
    gen_set_label(done_4);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_7);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_8);
    tcg_temp_free(bta);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_11);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_13);
    tcg_temp_free(temp_12);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * J
 *    Variables: @src
 *    Functions: getCCFlag, shouldExecuteDelaySlot, executeDelaySlot, setPC
 * --- code ---
 * {
 *   take_branch = false;
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       take_branch = true;
 *     };
 *   bta = @src;
 *   if((shouldExecuteDelaySlot () == 1))
 *     {
 *       executeDelaySlot (bta, take_branch);
 *     };
 *   if((cc_flag == true))
 *     {
 *       setPC (bta);
 *     };
 * }
 */

int
arc_gen_J(DisasCtxt *ctx, TCGv src)
{
    int ret = DISAS_NEXT;
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv bta = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    tcg_gen_mov_tl(take_branch, arc_false);
    getCCFlag(temp_5);
    tcg_gen_mov_tl(cc_flag, temp_5);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(take_branch, arc_true);
    gen_set_label(done_1);
    tcg_gen_mov_tl(bta, src);
    if ((shouldExecuteDelaySlot () == 1)) {
        executeDelaySlot(bta, take_branch);
    }
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_3, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, done_2);
    setPC(bta);
    gen_set_label(done_2);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_5);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(bta);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);

    return ret;
}


/*
 * JL
 *    Variables: @src
 *    Functions: getCCFlag, shouldExecuteDelaySlot, setBLINK,
 *               nextInsnAddressAfterDelaySlot, executeDelaySlot,
 *               nextInsnAddress, setPC
 * --- code ---
 * {
 *   take_branch = false;
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       take_branch = true;
 *     };
 *   bta = @src;
 *   if((shouldExecuteDelaySlot () == 1))
 *     {
 *       if(take_branch)
 *         {
 *           setBLINK (nextInsnAddressAfterDelaySlot ());
 *         };
 *       executeDelaySlot (bta, take_branch);
 *     }
 *   else
 *     {
 *       if(take_branch)
 *         {
 *           setBLINK (nextInsnAddress ());
 *         };
 *     };
 *   if((cc_flag == true))
 *     {
 *       setPC (bta);
 *     };
 * }
 */

int
arc_gen_JL(DisasCtxt *ctx, TCGv src)
{
    int ret = DISAS_NEXT;
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv bta = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    tcg_gen_mov_tl(take_branch, arc_false);
    getCCFlag(temp_7);
    tcg_gen_mov_tl(cc_flag, temp_7);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(take_branch, arc_true);
    gen_set_label(done_1);
    tcg_gen_mov_tl(bta, src);
    if ((shouldExecuteDelaySlot () == 1)) {
        TCGLabel *done_2 = gen_new_label();
        tcg_gen_xori_tl(temp_3, take_branch, 1);
        tcg_gen_andi_tl(temp_3, temp_3, 1);
        tcg_gen_brcond_tl(TCG_COND_EQ, temp_3, arc_true, done_2);
        nextInsnAddressAfterDelaySlot(temp_9);
        tcg_gen_mov_tl(temp_8, temp_9);
        setBLINK(temp_8);
        gen_set_label(done_2);
        executeDelaySlot(bta, take_branch);
    } else {
        TCGLabel *done_3 = gen_new_label();
        tcg_gen_xori_tl(temp_4, take_branch, 1);
        tcg_gen_andi_tl(temp_4, temp_4, 1);
        tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, done_3);
        nextInsnAddress(temp_11);
        tcg_gen_mov_tl(temp_10, temp_11);
        setBLINK(temp_10);
        gen_set_label(done_3);
    }
    TCGLabel *done_4 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_5, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_6, temp_5, 1);
    tcg_gen_andi_tl(temp_6, temp_6, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_6, arc_true, done_4);
    setPC(bta);
    gen_set_label(done_4);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_7);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(bta);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_11);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * SETEQ
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       p_b = @b;
 *       p_c = @c;
 *       take_branch = false;
 *       if((p_b == p_c))
 *         {
 *         }
 *       else
 *         {
 *         };
 *       if((p_b == p_c))
 *         {
 *           @a = true;
 *         }
 *       else
 *         {
 *           @a = false;
 *         };
 *     };
 * }
 */

int
arc_gen_SETEQ(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_7 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv p_b = tcg_temp_local_new();
    TCGv p_c = tcg_temp_local_new();
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_7);
    tcg_gen_mov_tl(cc_flag, temp_7);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(p_b, b);
    tcg_gen_mov_tl(p_c, c);
    tcg_gen_mov_tl(take_branch, arc_false);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_3, p_b, p_c);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    gen_set_label(done_2);
    TCGLabel *else_3 = gen_new_label();
    TCGLabel *done_3 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_5, p_b, p_c);
    tcg_gen_xori_tl(temp_6, temp_5, 1);
    tcg_gen_andi_tl(temp_6, temp_6, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_6, arc_true, else_3);
    tcg_gen_mov_tl(a, arc_true);
    tcg_gen_br(done_3);
    gen_set_label(else_3);
    tcg_gen_mov_tl(a, arc_false);
    gen_set_label(done_3);
    gen_set_label(done_1);
    tcg_temp_free(temp_7);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(p_b);
    tcg_temp_free(p_c);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * BREQ
 *    Variables: @b, @c, @offset
 *    Functions: getPCL, shouldExecuteDelaySlot, executeDelaySlot, setPC
 * --- code ---
 * {
 *   p_b = @b;
 *   p_c = @c;
 *   take_branch = false;
 *   if((p_b == p_c))
 *     {
 *       take_branch = true;
 *     }
 *   else
 *     {
 *     };
 *   bta = (getPCL () + @offset);
 *   if((shouldExecuteDelaySlot () == 1))
 *     {
 *       executeDelaySlot (bta, take_branch);
 *     };
 *   if((p_b == p_c))
 *     {
 *       setPC (bta);
 *     }
 *   else
 *     {
 *     };
 * }
 */

int
arc_gen_BREQ(DisasCtxt *ctx, TCGv b, TCGv c, TCGv offset)
{
    int ret = DISAS_NEXT;
    TCGv p_b = tcg_temp_local_new();
    TCGv p_c = tcg_temp_local_new();
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv bta = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    tcg_gen_mov_tl(p_b, b);
    tcg_gen_mov_tl(p_c, c);
    tcg_gen_mov_tl(take_branch, arc_false);
    TCGLabel *else_1 = gen_new_label();
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, p_b, p_c);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, else_1);
    tcg_gen_mov_tl(take_branch, arc_true);
    tcg_gen_br(done_1);
    gen_set_label(else_1);
    gen_set_label(done_1);
    getPCL(temp_6);
    tcg_gen_mov_tl(temp_5, temp_6);
    tcg_gen_add_tl(bta, temp_5, offset);
    if ((shouldExecuteDelaySlot () == 1)) {
        executeDelaySlot(bta, take_branch);
    }
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_3, p_b, p_c);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    setPC(bta);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    gen_set_label(done_2);
    tcg_temp_free(p_b);
    tcg_temp_free(p_c);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_5);
    tcg_temp_free(bta);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);

    return ret;
}


/*
 * SETNE
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       p_b = @b;
 *       p_c = @c;
 *       take_branch = false;
 *       if((p_b != p_c))
 *         {
 *         }
 *       else
 *         {
 *         };
 *       if((p_b != p_c))
 *         {
 *           @a = true;
 *         }
 *       else
 *         {
 *           @a = false;
 *         };
 *     };
 * }
 */

int
arc_gen_SETNE(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_7 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv p_b = tcg_temp_local_new();
    TCGv p_c = tcg_temp_local_new();
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_7);
    tcg_gen_mov_tl(cc_flag, temp_7);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(p_b, b);
    tcg_gen_mov_tl(p_c, c);
    tcg_gen_mov_tl(take_branch, arc_false);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_NE, temp_3, p_b, p_c);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    gen_set_label(done_2);
    TCGLabel *else_3 = gen_new_label();
    TCGLabel *done_3 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_NE, temp_5, p_b, p_c);
    tcg_gen_xori_tl(temp_6, temp_5, 1);
    tcg_gen_andi_tl(temp_6, temp_6, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_6, arc_true, else_3);
    tcg_gen_mov_tl(a, arc_true);
    tcg_gen_br(done_3);
    gen_set_label(else_3);
    tcg_gen_mov_tl(a, arc_false);
    gen_set_label(done_3);
    gen_set_label(done_1);
    tcg_temp_free(temp_7);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(p_b);
    tcg_temp_free(p_c);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * BRNE
 *    Variables: @b, @c, @offset
 *    Functions: getPCL, shouldExecuteDelaySlot, executeDelaySlot, setPC
 * --- code ---
 * {
 *   p_b = @b;
 *   p_c = @c;
 *   take_branch = false;
 *   if((p_b != p_c))
 *     {
 *       take_branch = true;
 *     }
 *   else
 *     {
 *     };
 *   bta = (getPCL () + @offset);
 *   if((shouldExecuteDelaySlot () == 1))
 *     {
 *       executeDelaySlot (bta, take_branch);
 *     };
 *   if((p_b != p_c))
 *     {
 *       setPC (bta);
 *     }
 *   else
 *     {
 *     };
 * }
 */

int
arc_gen_BRNE(DisasCtxt *ctx, TCGv b, TCGv c, TCGv offset)
{
    int ret = DISAS_NEXT;
    TCGv p_b = tcg_temp_local_new();
    TCGv p_c = tcg_temp_local_new();
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv bta = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    tcg_gen_mov_tl(p_b, b);
    tcg_gen_mov_tl(p_c, c);
    tcg_gen_mov_tl(take_branch, arc_false);
    TCGLabel *else_1 = gen_new_label();
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_NE, temp_1, p_b, p_c);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, else_1);
    tcg_gen_mov_tl(take_branch, arc_true);
    tcg_gen_br(done_1);
    gen_set_label(else_1);
    gen_set_label(done_1);
    getPCL(temp_6);
    tcg_gen_mov_tl(temp_5, temp_6);
    tcg_gen_add_tl(bta, temp_5, offset);
    if ((shouldExecuteDelaySlot () == 1)) {
        executeDelaySlot(bta, take_branch);
    }
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_NE, temp_3, p_b, p_c);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    setPC(bta);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    gen_set_label(done_2);
    tcg_temp_free(p_b);
    tcg_temp_free(p_c);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_5);
    tcg_temp_free(bta);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);

    return ret;
}


/*
 * SETLT
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       p_b = @b;
 *       p_c = @c;
 *       take_branch = false;
 *       if((p_b < p_c))
 *         {
 *         }
 *       else
 *         {
 *         };
 *       if((p_b < p_c))
 *         {
 *           @a = true;
 *         }
 *       else
 *         {
 *           @a = false;
 *         };
 *     };
 * }
 */

int
arc_gen_SETLT(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_7 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv p_b = tcg_temp_local_new();
    TCGv p_c = tcg_temp_local_new();
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_7);
    tcg_gen_mov_tl(cc_flag, temp_7);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(p_b, b);
    tcg_gen_mov_tl(p_c, c);
    tcg_gen_mov_tl(take_branch, arc_false);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_LT, temp_3, p_b, p_c);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    gen_set_label(done_2);
    TCGLabel *else_3 = gen_new_label();
    TCGLabel *done_3 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_LT, temp_5, p_b, p_c);
    tcg_gen_xori_tl(temp_6, temp_5, 1);
    tcg_gen_andi_tl(temp_6, temp_6, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_6, arc_true, else_3);
    tcg_gen_mov_tl(a, arc_true);
    tcg_gen_br(done_3);
    gen_set_label(else_3);
    tcg_gen_mov_tl(a, arc_false);
    gen_set_label(done_3);
    gen_set_label(done_1);
    tcg_temp_free(temp_7);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(p_b);
    tcg_temp_free(p_c);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * BRLT
 *    Variables: @b, @c, @offset
 *    Functions: getPCL, shouldExecuteDelaySlot, executeDelaySlot, setPC
 * --- code ---
 * {
 *   p_b = @b;
 *   p_c = @c;
 *   take_branch = false;
 *   if((p_b < p_c))
 *     {
 *       take_branch = true;
 *     }
 *   else
 *     {
 *     };
 *   bta = (getPCL () + @offset);
 *   if((shouldExecuteDelaySlot () == 1))
 *     {
 *       executeDelaySlot (bta, take_branch);
 *     };
 *   if((p_b < p_c))
 *     {
 *       setPC (bta);
 *     }
 *   else
 *     {
 *     };
 * }
 */

int
arc_gen_BRLT(DisasCtxt *ctx, TCGv b, TCGv c, TCGv offset)
{
    int ret = DISAS_NEXT;
    TCGv p_b = tcg_temp_local_new();
    TCGv p_c = tcg_temp_local_new();
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv bta = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    tcg_gen_mov_tl(p_b, b);
    tcg_gen_mov_tl(p_c, c);
    tcg_gen_mov_tl(take_branch, arc_false);
    TCGLabel *else_1 = gen_new_label();
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_LT, temp_1, p_b, p_c);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, else_1);
    tcg_gen_mov_tl(take_branch, arc_true);
    tcg_gen_br(done_1);
    gen_set_label(else_1);
    gen_set_label(done_1);
    getPCL(temp_6);
    tcg_gen_mov_tl(temp_5, temp_6);
    tcg_gen_add_tl(bta, temp_5, offset);
    if ((shouldExecuteDelaySlot () == 1)) {
        executeDelaySlot(bta, take_branch);
    }
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_LT, temp_3, p_b, p_c);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    setPC(bta);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    gen_set_label(done_2);
    tcg_temp_free(p_b);
    tcg_temp_free(p_c);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_5);
    tcg_temp_free(bta);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);

    return ret;
}


/*
 * SETGE
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       p_b = @b;
 *       p_c = @c;
 *       take_branch = false;
 *       if((p_b >= p_c))
 *         {
 *         }
 *       else
 *         {
 *         };
 *       if((p_b >= p_c))
 *         {
 *           @a = true;
 *         }
 *       else
 *         {
 *           @a = false;
 *         };
 *     };
 * }
 */

int
arc_gen_SETGE(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_7 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv p_b = tcg_temp_local_new();
    TCGv p_c = tcg_temp_local_new();
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_7);
    tcg_gen_mov_tl(cc_flag, temp_7);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(p_b, b);
    tcg_gen_mov_tl(p_c, c);
    tcg_gen_mov_tl(take_branch, arc_false);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_GE, temp_3, p_b, p_c);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    gen_set_label(done_2);
    TCGLabel *else_3 = gen_new_label();
    TCGLabel *done_3 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_GE, temp_5, p_b, p_c);
    tcg_gen_xori_tl(temp_6, temp_5, 1);
    tcg_gen_andi_tl(temp_6, temp_6, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_6, arc_true, else_3);
    tcg_gen_mov_tl(a, arc_true);
    tcg_gen_br(done_3);
    gen_set_label(else_3);
    tcg_gen_mov_tl(a, arc_false);
    gen_set_label(done_3);
    gen_set_label(done_1);
    tcg_temp_free(temp_7);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(p_b);
    tcg_temp_free(p_c);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * BRGE
 *    Variables: @b, @c, @offset
 *    Functions: getPCL, shouldExecuteDelaySlot, executeDelaySlot, setPC
 * --- code ---
 * {
 *   p_b = @b;
 *   p_c = @c;
 *   take_branch = false;
 *   if((p_b >= p_c))
 *     {
 *       take_branch = true;
 *     }
 *   else
 *     {
 *     };
 *   bta = (getPCL () + @offset);
 *   if((shouldExecuteDelaySlot () == 1))
 *     {
 *       executeDelaySlot (bta, take_branch);
 *     };
 *   if((p_b >= p_c))
 *     {
 *       setPC (bta);
 *     }
 *   else
 *     {
 *     };
 * }
 */

int
arc_gen_BRGE(DisasCtxt *ctx, TCGv b, TCGv c, TCGv offset)
{
    int ret = DISAS_NEXT;
    TCGv p_b = tcg_temp_local_new();
    TCGv p_c = tcg_temp_local_new();
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv bta = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    tcg_gen_mov_tl(p_b, b);
    tcg_gen_mov_tl(p_c, c);
    tcg_gen_mov_tl(take_branch, arc_false);
    TCGLabel *else_1 = gen_new_label();
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_GE, temp_1, p_b, p_c);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, else_1);
    tcg_gen_mov_tl(take_branch, arc_true);
    tcg_gen_br(done_1);
    gen_set_label(else_1);
    gen_set_label(done_1);
    getPCL(temp_6);
    tcg_gen_mov_tl(temp_5, temp_6);
    tcg_gen_add_tl(bta, temp_5, offset);
    if ((shouldExecuteDelaySlot () == 1)) {
        executeDelaySlot(bta, take_branch);
    }
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_GE, temp_3, p_b, p_c);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    setPC(bta);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    gen_set_label(done_2);
    tcg_temp_free(p_b);
    tcg_temp_free(p_c);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_5);
    tcg_temp_free(bta);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);

    return ret;
}


/*
 * SETLE
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       p_b = @b;
 *       p_c = @c;
 *       take_branch = false;
 *       if((p_b <= p_c))
 *         {
 *         }
 *       else
 *         {
 *         };
 *       if((p_b <= p_c))
 *         {
 *           @a = true;
 *         }
 *       else
 *         {
 *           @a = false;
 *         };
 *     };
 * }
 */

int
arc_gen_SETLE(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_7 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv p_b = tcg_temp_local_new();
    TCGv p_c = tcg_temp_local_new();
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_7);
    tcg_gen_mov_tl(cc_flag, temp_7);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(p_b, b);
    tcg_gen_mov_tl(p_c, c);
    tcg_gen_mov_tl(take_branch, arc_false);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_LE, temp_3, p_b, p_c);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    gen_set_label(done_2);
    TCGLabel *else_3 = gen_new_label();
    TCGLabel *done_3 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_LE, temp_5, p_b, p_c);
    tcg_gen_xori_tl(temp_6, temp_5, 1);
    tcg_gen_andi_tl(temp_6, temp_6, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_6, arc_true, else_3);
    tcg_gen_mov_tl(a, arc_true);
    tcg_gen_br(done_3);
    gen_set_label(else_3);
    tcg_gen_mov_tl(a, arc_false);
    gen_set_label(done_3);
    gen_set_label(done_1);
    tcg_temp_free(temp_7);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(p_b);
    tcg_temp_free(p_c);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * SETGT
 *    Variables: @b, @c, @a
 *    Functions: getCCFlag
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       p_b = @b;
 *       p_c = @c;
 *       take_branch = false;
 *       if((p_b > p_c))
 *         {
 *         }
 *       else
 *         {
 *         };
 *       if((p_b > p_c))
 *         {
 *           @a = true;
 *         }
 *       else
 *         {
 *           @a = false;
 *         };
 *     };
 * }
 */

int
arc_gen_SETGT(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv temp_7 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv p_b = tcg_temp_local_new();
    TCGv p_c = tcg_temp_local_new();
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    getCCFlag(temp_7);
    tcg_gen_mov_tl(cc_flag, temp_7);
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
    tcg_gen_mov_tl(p_b, b);
    tcg_gen_mov_tl(p_c, c);
    tcg_gen_mov_tl(take_branch, arc_false);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_GT, temp_3, p_b, p_c);
    tcg_gen_xori_tl(temp_4, temp_3, 1);
    tcg_gen_andi_tl(temp_4, temp_4, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_4, arc_true, else_2);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    gen_set_label(done_2);
    TCGLabel *else_3 = gen_new_label();
    TCGLabel *done_3 = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_GT, temp_5, p_b, p_c);
    tcg_gen_xori_tl(temp_6, temp_5, 1);
    tcg_gen_andi_tl(temp_6, temp_6, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_6, arc_true, else_3);
    tcg_gen_mov_tl(a, arc_true);
    tcg_gen_br(done_3);
    gen_set_label(else_3);
    tcg_gen_mov_tl(a, arc_false);
    gen_set_label(done_3);
    gen_set_label(done_1);
    tcg_temp_free(temp_7);
    tcg_temp_free(cc_flag);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(p_b);
    tcg_temp_free(p_c);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_6);

    return ret;
}


/*
 * BRLO
 *    Variables: @b, @c, @offset
 *    Functions: unsignedLT, getPCL, shouldExecuteDelaySlot, executeDelaySlot,
 *               setPC
 * --- code ---
 * {
 *   p_b = @b;
 *   p_c = @c;
 *   take_branch = false;
 *   if(unsignedLT (p_b, p_c))
 *     {
 *       take_branch = true;
 *     }
 *   else
 *     {
 *     };
 *   bta = (getPCL () + @offset);
 *   if((shouldExecuteDelaySlot () == 1))
 *     {
 *       executeDelaySlot (bta, take_branch);
 *     };
 *   if(unsignedLT (p_b, p_c))
 *     {
 *       setPC (bta);
 *     }
 *   else
 *     {
 *     };
 * }
 */

int
arc_gen_BRLO(DisasCtxt *ctx, TCGv b, TCGv c, TCGv offset)
{
    int ret = DISAS_NEXT;
    TCGv p_b = tcg_temp_local_new();
    TCGv p_c = tcg_temp_local_new();
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv bta = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    tcg_gen_mov_tl(p_b, b);
    tcg_gen_mov_tl(p_c, c);
    tcg_gen_mov_tl(take_branch, arc_false);
    TCGLabel *else_1 = gen_new_label();
    TCGLabel *done_1 = gen_new_label();
    unsignedLT(temp_3, p_b, p_c);
    tcg_gen_xori_tl(temp_1, temp_3, 1);
    tcg_gen_andi_tl(temp_1, temp_1, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_1, arc_true, else_1);
    tcg_gen_mov_tl(take_branch, arc_true);
    tcg_gen_br(done_1);
    gen_set_label(else_1);
    gen_set_label(done_1);
    getPCL(temp_5);
    tcg_gen_mov_tl(temp_4, temp_5);
    tcg_gen_add_tl(bta, temp_4, offset);
    if ((shouldExecuteDelaySlot () == 1)) {
        executeDelaySlot(bta, take_branch);
    }
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    unsignedLT(temp_6, p_b, p_c);
    tcg_gen_xori_tl(temp_2, temp_6, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, else_2);
    setPC(bta);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    gen_set_label(done_2);
    tcg_temp_free(p_b);
    tcg_temp_free(p_c);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(bta);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_2);

    return ret;
}


/*
 * SETLO
 *    Variables: @b, @c, @a
 *    Functions: unsignedLT
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       p_b = @b;
 *       p_c = @c;
 *       take_branch = false;
 *       if(unsignedLT (p_b, p_c))
 *         {
 *         }
 *       else
 *         {
 *         };
 *       if(unsignedLT (p_b, p_c))
 *         {
 *           @a = true;
 *         }
 *       else
 *         {
 *           @a = false;
 *         };
 *     }
 * }
 */

int
arc_gen_SETLO(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv p_b = tcg_temp_local_new();
    TCGv p_c = tcg_temp_local_new();
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv cc_temp_1 = tcg_temp_local_new();
    getCCFlag(cc_flag);
    TCGLabel *done_cc = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, cc_temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(cc_temp_1, cc_temp_1, 1); tcg_gen_andi_tl(cc_temp_1, cc_temp_1, 1);;
    tcg_gen_brcond_tl(TCG_COND_EQ, cc_temp_1, arc_true, done_cc);;
    tcg_gen_mov_tl(p_b, b);
    tcg_gen_mov_tl(p_c, c);
    tcg_gen_mov_tl(take_branch, arc_false);
    TCGLabel *else_1 = gen_new_label();
    TCGLabel *done_1 = gen_new_label();
    unsignedLT(temp_3, p_b, p_c);
    tcg_gen_xori_tl(temp_1, temp_3, 1);
    tcg_gen_andi_tl(temp_1, temp_1, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_1, arc_true, else_1);
    tcg_gen_br(done_1);
    gen_set_label(else_1);
    gen_set_label(done_1);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    unsignedLT(temp_4, p_b, p_c);
    tcg_gen_xori_tl(temp_2, temp_4, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, else_2);
    tcg_gen_mov_tl(a, arc_true);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    tcg_gen_mov_tl(a, arc_false);
    gen_set_label(done_2);
    gen_set_label(done_cc);
    tcg_temp_free(p_b);
    tcg_temp_free(p_c);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_2);
    tcg_temp_free(cc_temp_1);
    tcg_temp_free(cc_flag);

    return ret;
}


/*
 * BRHS
 *    Variables: @b, @c, @offset
 *    Functions: unsignedGE, getPCL, shouldExecuteDelaySlot, executeDelaySlot,
 *               setPC
 * --- code ---
 * {
 *   p_b = @b;
 *   p_c = @c;
 *   take_branch = false;
 *   if(unsignedGE (p_b, p_c))
 *     {
 *       take_branch = true;
 *     }
 *   else
 *     {
 *     };
 *   bta = (getPCL () + @offset);
 *   if((shouldExecuteDelaySlot () == 1))
 *     {
 *       executeDelaySlot (bta, take_branch);
 *     };
 *   if(unsignedGE (p_b, p_c))
 *     {
 *       setPC (bta);
 *     }
 *   else
 *     {
 *     };
 * }
 */

int
arc_gen_BRHS(DisasCtxt *ctx, TCGv b, TCGv c, TCGv offset)
{
    int ret = DISAS_NEXT;
    TCGv p_b = tcg_temp_local_new();
    TCGv p_c = tcg_temp_local_new();
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv bta = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    tcg_gen_mov_tl(p_b, b);
    tcg_gen_mov_tl(p_c, c);
    tcg_gen_mov_tl(take_branch, arc_false);
    TCGLabel *else_1 = gen_new_label();
    TCGLabel *done_1 = gen_new_label();
    unsignedGE(temp_3, p_b, p_c);
    tcg_gen_xori_tl(temp_1, temp_3, 1);
    tcg_gen_andi_tl(temp_1, temp_1, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_1, arc_true, else_1);
    tcg_gen_mov_tl(take_branch, arc_true);
    tcg_gen_br(done_1);
    gen_set_label(else_1);
    gen_set_label(done_1);
    getPCL(temp_5);
    tcg_gen_mov_tl(temp_4, temp_5);
    tcg_gen_add_tl(bta, temp_4, offset);
    if ((shouldExecuteDelaySlot () == 1)) {
        executeDelaySlot(bta, take_branch);
    }
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    unsignedGE(temp_6, p_b, p_c);
    tcg_gen_xori_tl(temp_2, temp_6, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, else_2);
    setPC(bta);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    gen_set_label(done_2);
    tcg_temp_free(p_b);
    tcg_temp_free(p_c);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(bta);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_2);

    return ret;
}


/*
 * SETHS
 *    Variables: @b, @c, @a
 *    Functions: unsignedGE
 * --- code ---
 * {
 *   cc_flag = getCCFlag ();
 *   if((cc_flag == true))
 *     {
 *       p_b = @b;
 *       p_c = @c;
 *       take_branch = false;
 *       if(unsignedGE (p_b, p_c))
 *         {
 *         }
 *       else
 *         {
 *         };
 *       if(unsignedGE (p_b, p_c))
 *         {
 *           @a = true;
 *         }
 *       else
 *         {
 *           @a = false;
 *         };
 *     }
 * }
 */

int
arc_gen_SETHS(DisasCtxt *ctx, TCGv b, TCGv c, TCGv a)
{
    int ret = DISAS_NEXT;
    TCGv p_b = tcg_temp_local_new();
    TCGv p_c = tcg_temp_local_new();
    TCGv take_branch = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv cc_flag = tcg_temp_local_new();
    TCGv cc_temp_1 = tcg_temp_local_new();
    getCCFlag(cc_flag);
    TCGLabel *done_cc = gen_new_label();
    tcg_gen_setcond_tl(TCG_COND_EQ, cc_temp_1, cc_flag, arc_true);
    tcg_gen_xori_tl(cc_temp_1, cc_temp_1, 1); tcg_gen_andi_tl(cc_temp_1, cc_temp_1, 1);;
    tcg_gen_brcond_tl(TCG_COND_EQ, cc_temp_1, arc_true, done_cc);;
    tcg_gen_mov_tl(p_b, b);
    tcg_gen_mov_tl(p_c, c);
    tcg_gen_mov_tl(take_branch, arc_false);
    TCGLabel *else_1 = gen_new_label();
    TCGLabel *done_1 = gen_new_label();
    unsignedGE(temp_3, p_b, p_c);
    tcg_gen_xori_tl(temp_1, temp_3, 1);
    tcg_gen_andi_tl(temp_1, temp_1, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_1, arc_true, else_1);
    tcg_gen_br(done_1);
    gen_set_label(else_1);
    gen_set_label(done_1);
    TCGLabel *else_2 = gen_new_label();
    TCGLabel *done_2 = gen_new_label();
    unsignedGE(temp_4, p_b, p_c);
    tcg_gen_xori_tl(temp_2, temp_4, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, else_2);
    tcg_gen_mov_tl(a, arc_true);
    tcg_gen_br(done_2);
    gen_set_label(else_2);
    tcg_gen_mov_tl(a, arc_false);
    gen_set_label(done_2);
    gen_set_label(done_cc);
    tcg_temp_free(p_b);
    tcg_temp_free(p_c);
    tcg_temp_free(take_branch);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_2);
    tcg_temp_free(cc_temp_1);
    tcg_temp_free(cc_flag);

    return ret;
}


/*
 * EX - CODED BY HAND
 */

int
arc_gen_EX (DisasCtxt *ctx, TCGv b, TCGv c)
{
  int ret = DISAS_NEXT;
  TCGv temp = tcg_temp_local_new();
  tcg_gen_mov_tl(temp, b);
  tcg_gen_atomic_xchg_tl(b, c, temp, ctx->mem_idx, MO_UL);
  tcg_temp_free(temp);

  return ret;
}


//#define ARM_LIKE_LLOCK_SCOND

extern TCGv cpu_exclusive_addr;
extern TCGv cpu_exclusive_val;
extern TCGv cpu_exclusive_val_hi;

/*
 * LLOCK -- CODED BY HAND
 */

int
arc_gen_LLOCK(DisasCtxt *ctx, TCGv dest, TCGv src)
{
    int ret = DISAS_NEXT;
#ifndef ARM_LIKE_LLOCK_SCOND
    gen_helper_llock(dest, cpu_env, src);
#else
    tcg_gen_qemu_ld_tl(cpu_exclusive_val, src, ctx->mem_idx, MO_UL);
    tcg_gen_mov_tl(dest, cpu_exclusive_val);
    tcg_gen_mov_tl(cpu_exclusive_addr, src);
#endif

    return ret;
}


/*
 * LLOCKD -- CODED BY HAND
 */

int
arc_gen_LLOCKD(DisasCtxt *ctx, TCGv dest, TCGv src)
{
    int ret = DISAS_NEXT;
    TCGv pair = nextReg (dest);

    TCGv_i64 temp_1 = tcg_temp_local_new_i64();
    TCGv_i64 temp_2 = tcg_temp_local_new_i64();

#ifndef ARM_LIKE_LLOCK_SCOND
    gen_helper_llockd(temp_1, cpu_env, src);
#else
    tcg_gen_qemu_ld_i64(temp_1, src, ctx->mem_idx, MO_UQ);
    tcg_gen_mov_tl(cpu_exclusive_addr, src);

    tcg_gen_shri_i64(temp_2, temp_1, 32);
    tcg_gen_trunc_i64_tl(cpu_exclusive_val_hi, temp_2);
    tcg_gen_trunc_i64_tl(cpu_exclusive_val, temp_1);
#endif

    tcg_gen_shri_i64(temp_2, temp_1, 32);
    tcg_gen_trunc_i64_tl(pair, temp_2);
    tcg_gen_trunc_i64_tl(dest, temp_1);

    tcg_temp_free_i64(temp_1);
    tcg_temp_free_i64(temp_2);


    return ret;
}


/*
 * SCOND -- CODED BY HAND
 */

int
arc_gen_SCOND(DisasCtxt *ctx, TCGv addr, TCGv value)
{
    int ret = DISAS_NEXT;
#ifndef ARM_LIKE_LLOCK_SCOND
    TCGv temp_4 = tcg_temp_local_new();
    gen_helper_scond(temp_4, cpu_env, addr, value);
    setZFlag(temp_4);
    tcg_temp_free(temp_4);
#else
    TCGLabel *fail_label = gen_new_label();
    TCGLabel *done_label = gen_new_label();
    TCGv tmp;

    tcg_gen_brcond_tl(TCG_COND_NE, addr, cpu_exclusive_addr, fail_label);
    tmp = tcg_temp_new();

    tcg_gen_atomic_cmpxchg_tl(tmp, cpu_exclusive_addr, cpu_exclusive_val,
                               value, ctx->mem_idx,
                               MO_UL | MO_ALIGN);
    tcg_gen_setcond_tl(TCG_COND_NE, tmp, tmp, cpu_exclusive_val);

    setZFlag(tmp);

    tcg_temp_free(tmp);
    tcg_gen_br(done_label);

    gen_set_label(fail_label);
    tcg_gen_movi_tl(cpu_Zf, 1);
    gen_set_label(done_label);
    tcg_gen_movi_tl(cpu_exclusive_addr, -1);
#endif

    return ret;
}


/*
 * SCONDD -- CODED BY HAND
 */

int
arc_gen_SCONDD(DisasCtxt *ctx, TCGv addr, TCGv value)
{
    int ret = DISAS_NEXT;
    TCGv pair = NULL;
    pair = nextReg (value);

    TCGv_i64 temp_1 = tcg_temp_local_new_i64();
    TCGv_i64 temp_2 = tcg_temp_local_new_i64();

    TCGv_i64 temp_3 = tcg_temp_local_new_i64();
    TCGv_i64 temp_4 = tcg_temp_local_new_i64();
    TCGv_i64 exclusive_val = tcg_temp_local_new_i64();

    tcg_gen_ext_i32_i64(temp_1, pair);
    tcg_gen_ext_i32_i64(temp_2, value);
    tcg_gen_shli_i64(temp_1, temp_1, 32);
    tcg_gen_or_i64(temp_1, temp_1, temp_2);

#ifndef ARM_LIKE_LLOCK_SCOND
    TCGv temp_5 = tcg_temp_local_new();
    gen_helper_scondd(temp_5, cpu_env, addr, temp_1);
    setZFlag(temp_5);
    tcg_temp_free(temp_5);
#else
    TCGLabel *fail_label = gen_new_label();
    TCGLabel *done_label = gen_new_label();

    tcg_gen_ext_i32_i64(temp_3, cpu_exclusive_val_hi);
    tcg_gen_ext_i32_i64(temp_4, cpu_exclusive_val);
    tcg_gen_shli_i64(temp_3, temp_3, 32);

    tcg_gen_brcond_tl(TCG_COND_NE, addr, cpu_exclusive_addr, fail_label);

    TCGv_i64 tmp = tcg_temp_new_i64();
    TCGv tmp1 = tcg_temp_new();

    tcg_gen_or_i64(exclusive_val, temp_3, temp_4);

    tcg_gen_atomic_cmpxchg_i64(tmp, cpu_exclusive_addr, exclusive_val,
                               temp_1, ctx->mem_idx,
                               MO_UL | MO_ALIGN);
    tcg_gen_setcond_i64(TCG_COND_NE, tmp, tmp, exclusive_val);
    tcg_gen_trunc_i64_tl(tmp1, tmp);
    setZFlag(tmp1);

    tcg_temp_free_i64(tmp);
    tcg_temp_free(tmp1);
    tcg_gen_br(done_label);

    gen_set_label(fail_label);
    tcg_gen_movi_tl(cpu_Zf, 1);
    gen_set_label(done_label);
    tcg_gen_movi_tl(cpu_exclusive_addr, -1);
#endif

    tcg_temp_free_i64(temp_1);
    tcg_temp_free_i64(temp_2);
    tcg_temp_free_i64(temp_3);
    tcg_temp_free_i64(temp_4);
    tcg_temp_free_i64(exclusive_val);

    return ret;
}


/* DMB - HAND MADE
 */

int
arc_gen_DMB (DisasCtxt *ctx, TCGv a)
{
  int ret = DISAS_NEXT;

  TCGBar bar = 0;
  switch(ctx->insn.operands[0].value & 7) {
    case 1:
      bar |= TCG_BAR_SC | TCG_MO_LD_LD | TCG_MO_LD_ST;
      break;
    case 2:
      bar |= TCG_BAR_SC | TCG_MO_ST_ST;
      break;
    default:
      bar |= TCG_BAR_SC | TCG_MO_ALL;
      break;
  }
  tcg_gen_mb(bar);

  return ret;
}


/*
 * LD
 *    Variables: @src1, @src2, @dest
 *    Functions: getAAFlag, getZZFlag, setDebugLD, getMemory, getFlagX,
 *               SignExtend, NoFurtherLoadsPending
 * --- code ---
 * {
 *   AA = getAAFlag ();
 *   ZZ = getZZFlag ();
 *   address = 0;
 *   if(((AA == 0) || (AA == 1)))
 *     {
 *       address = (@src1 + @src2);
 *     };
 *   if((AA == 2))
 *     {
 *       address = @src1;
 *     };
 *   if(((AA == 3) && ((ZZ == 0) || (ZZ == 3))))
 *     {
 *       address = (@src1 + (@src2 << 2));
 *     };
 *   if(((AA == 3) && (ZZ == 2)))
 *     {
 *       address = (@src1 + (@src2 << 1));
 *     };
 *   l_src1 = @src1;
 *   l_src2 = @src2;
 *   setDebugLD (1);
 *   new_dest = getMemory (address, ZZ);
 *   if(((AA == 1) || (AA == 2)))
 *     {
 *       @src1 = (l_src1 + l_src2);
 *     };
 *   if((getFlagX () == 1))
 *     {
 *       new_dest = SignExtend (new_dest, ZZ);
 *     };
 *   if(NoFurtherLoadsPending ())
 *     {
 *       setDebugLD (0);
 *     };
 *   @dest = new_dest;
 * }
 */

int
arc_gen_LD(DisasCtxt *ctx, TCGv src1, TCGv src2, TCGv dest)
{
    int ret = DISAS_NEXT;
    int AA;
    int ZZ;
    TCGv address = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv l_src1 = tcg_temp_local_new();
    TCGv l_src2 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv new_dest = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    AA = getAAFlag ();
    ZZ = getZZFlag ();
    tcg_gen_movi_tl(address, 0);
    if (((AA == 0) || (AA == 1))) {
        tcg_gen_add_tl(address, src1, src2);
    }
    if ((AA == 2)) {
        tcg_gen_mov_tl(address, src1);
    }
    if (((AA == 3) && ((ZZ == 0) || (ZZ == 3)))) {
        tcg_gen_shli_tl(temp_2, src2, 2);
        tcg_gen_add_tl(address, src1, temp_2);
    }
    if (((AA == 3) && (ZZ == 2))) {
        tcg_gen_shli_tl(temp_3, src2, 1);
        tcg_gen_add_tl(address, src1, temp_3);
    }
    tcg_gen_mov_tl(l_src1, src1);
    tcg_gen_mov_tl(l_src2, src2);
    tcg_gen_movi_tl(temp_4, 1);
    setDebugLD(temp_4);
    getMemory(temp_5, address, ZZ);
    tcg_gen_mov_tl(new_dest, temp_5);
    if (((AA == 1) || (AA == 2))) {
        tcg_gen_add_tl(src1, l_src1, l_src2);
    }
    if ((getFlagX () == 1)) {
        new_dest = SignExtend (new_dest, ZZ);
    }
    TCGLabel *done_1 = gen_new_label();
    NoFurtherLoadsPending(temp_6);
    tcg_gen_xori_tl(temp_1, temp_6, 1);
    tcg_gen_andi_tl(temp_1, temp_1, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_1, arc_true, done_1);
    tcg_gen_movi_tl(temp_7, 0);
    setDebugLD(temp_7);
    gen_set_label(done_1);
    tcg_gen_mov_tl(dest, new_dest);
    tcg_temp_free(address);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_3);
    tcg_temp_free(l_src1);
    tcg_temp_free(l_src2);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);
    tcg_temp_free(new_dest);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_7);

    return ret;
}


/*
 * LDD
 *    Variables: @src1, @src2, @dest
 *    Functions: getAAFlag, getZZFlag, setDebugLD, getMemory, nextReg,
 *               NoFurtherLoadsPending
 * --- code ---
 * {
 *   AA = getAAFlag ();
 *   ZZ = getZZFlag ();
 *   address = 0;
 *   if(((AA == 0) || (AA == 1)))
 *     {
 *       address = (@src1 + @src2);
 *     };
 *   if((AA == 2))
 *     {
 *       address = @src1;
 *     };
 *   if(((AA == 3) && ((ZZ == 0) || (ZZ == 3))))
 *     {
 *       address = (@src1 + (@src2 << 2));
 *     };
 *   if(((AA == 3) && (ZZ == 2)))
 *     {
 *       address = (@src1 + (@src2 << 1));
 *     };
 *   l_src1 = @src1;
 *   l_src2 = @src2;
 *   setDebugLD (1);
 *   new_dest = getMemory (address, LONG);
 *   pair = nextReg (dest);
 *   pair = getMemory ((address + 4), LONG);
 *   if(((AA == 1) || (AA == 2)))
 *     {
 *       @src1 = (l_src1 + l_src2);
 *     };
 *   if(NoFurtherLoadsPending ())
 *     {
 *       setDebugLD (0);
 *     };
 *   @dest = new_dest;
 * }
 */

int
arc_gen_LDD(DisasCtxt *ctx, TCGv src1, TCGv src2, TCGv dest)
{
    int ret = DISAS_NEXT;
    int AA;
    int ZZ;
    TCGv address = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv l_src1 = tcg_temp_local_new();
    TCGv l_src2 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv new_dest = tcg_temp_local_new();
    TCGv pair = NULL;
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    AA = getAAFlag ();
    ZZ = getZZFlag ();
    tcg_gen_movi_tl(address, 0);
    if (((AA == 0) || (AA == 1))) {
        tcg_gen_add_tl(address, src1, src2);
    }
    if ((AA == 2)) {
        tcg_gen_mov_tl(address, src1);
    }
    if (((AA == 3) && ((ZZ == 0) || (ZZ == 3)))) {
        tcg_gen_shli_tl(temp_2, src2, 2);
        tcg_gen_add_tl(address, src1, temp_2);
    }
    if (((AA == 3) && (ZZ == 2))) {
        tcg_gen_shli_tl(temp_3, src2, 1);
        tcg_gen_add_tl(address, src1, temp_3);
    }
    tcg_gen_mov_tl(l_src1, src1);
    tcg_gen_mov_tl(l_src2, src2);
    tcg_gen_movi_tl(temp_4, 1);
    setDebugLD(temp_4);
    getMemory(temp_5, address, LONG);
    tcg_gen_mov_tl(new_dest, temp_5);
    pair = nextReg (dest);
    tcg_gen_addi_tl(temp_7, address, 4);
    getMemory(temp_6, temp_7, LONG);
    tcg_gen_mov_tl(pair, temp_6);
    if (((AA == 1) || (AA == 2))) {
        tcg_gen_add_tl(src1, l_src1, l_src2);
    }
    TCGLabel *done_1 = gen_new_label();
    NoFurtherLoadsPending(temp_8);
    tcg_gen_xori_tl(temp_1, temp_8, 1);
    tcg_gen_andi_tl(temp_1, temp_1, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_1, arc_true, done_1);
    tcg_gen_movi_tl(temp_9, 0);
    setDebugLD(temp_9);
    gen_set_label(done_1);
    tcg_gen_mov_tl(dest, new_dest);
    tcg_temp_free(address);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_3);
    tcg_temp_free(l_src1);
    tcg_temp_free(l_src2);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);
    tcg_temp_free(new_dest);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_9);

    return ret;
}


/*
 * ST
 *    Variables: @src1, @src2, @dest
 *    Functions: getAAFlag, getZZFlag, setMemory
 * --- code ---
 * {
 *   AA = getAAFlag ();
 *   ZZ = getZZFlag ();
 *   address = 0;
 *   if(((AA == 0) || (AA == 1)))
 *     {
 *       address = (@src1 + @src2);
 *     };
 *   if((AA == 2))
 *     {
 *       address = @src1;
 *     };
 *   if(((AA == 3) && ((ZZ == 0) || (ZZ == 3))))
 *     {
 *       address = (@src1 + (@src2 << 2));
 *     };
 *   if(((AA == 3) && (ZZ == 2)))
 *     {
 *       address = (@src1 + (@src2 << 1));
 *     };
 *   setMemory (address, ZZ, @dest);
 *   if(((AA == 1) || (AA == 2)))
 *     {
 *       @src1 = (@src1 + @src2);
 *     };
 * }
 */

int
arc_gen_ST(DisasCtxt *ctx, TCGv src1, TCGv src2, TCGv dest)
{
    int ret = DISAS_NEXT;
    int AA;
    int ZZ;
    TCGv address = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    AA = getAAFlag ();
    ZZ = getZZFlag ();
    tcg_gen_movi_tl(address, 0);
    if (((AA == 0) || (AA == 1))) {
        tcg_gen_add_tl(address, src1, src2);
    }
    if ((AA == 2)) {
        tcg_gen_mov_tl(address, src1);
    }
    if (((AA == 3) && ((ZZ == 0) || (ZZ == 3)))) {
        tcg_gen_shli_tl(temp_1, src2, 2);
        tcg_gen_add_tl(address, src1, temp_1);
    }
    if (((AA == 3) && (ZZ == 2))) {
        tcg_gen_shli_tl(temp_2, src2, 1);
        tcg_gen_add_tl(address, src1, temp_2);
    }
    setMemory(address, ZZ, dest);
    if (((AA == 1) || (AA == 2))) {
        tcg_gen_add_tl(src1, src1, src2);
    }
    tcg_temp_free(address);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);

    return ret;
}


/*
 * STD
 *    Variables: @src1, @src2, @dest
 *    Functions: getAAFlag, getZZFlag, setMemory,
 *               instructionHasRegisterOperandIn, nextReg, getBit
 * --- code ---
 * {
 *   AA = getAAFlag ();
 *   ZZ = getZZFlag ();
 *   address = 0;
 *   if(((AA == 0) || (AA == 1)))
 *     {
 *       address = (@src1 + @src2);
 *     };
 *   if((AA == 2))
 *     {
 *       address = @src1;
 *     };
 *   if(((AA == 3) && ((ZZ == 0) || (ZZ == 3))))
 *     {
 *       address = (@src1 + (@src2 << 2));
 *     };
 *   if(((AA == 3) && (ZZ == 2)))
 *     {
 *       address = (@src1 + (@src2 << 1));
 *     };
 *   setMemory (address, LONG, @dest);
 *   if(instructionHasRegisterOperandIn (0))
 *     {
 *       pair = nextReg (dest);
 *       setMemory ((address + 4), LONG, pair);
 *     }
 *   else
 *     {
 *       tmp = 0;
 *       if(getBit (@dest, 31) == 1)
 *         {
 *           tmp = 4294967295;
 *         }
 *       setMemory ((address + 4), LONG, tmp);
 *     };
 *   if(((AA == 1) || (AA == 2)))
 *     {
 *       @src1 = (@src1 + @src2);
 *     };
 * }
 */

int
arc_gen_STD(DisasCtxt *ctx, TCGv src1, TCGv src2, TCGv dest)
{
    int ret = DISAS_NEXT;
    int AA;
    int ZZ;
    TCGv address = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv pair = NULL;
    TCGv temp_5 = tcg_temp_local_new();
    TCGv tmp = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    AA = getAAFlag ();
    ZZ = getZZFlag ();
    tcg_gen_movi_tl(address, 0);
    if (((AA == 0) || (AA == 1))) {
        tcg_gen_add_tl(address, src1, src2);
    }
    if ((AA == 2)) {
        tcg_gen_mov_tl(address, src1);
    }
    if (((AA == 3) && ((ZZ == 0) || (ZZ == 3)))) {
        tcg_gen_shli_tl(temp_3, src2, 2);
        tcg_gen_add_tl(address, src1, temp_3);
    }
    if (((AA == 3) && (ZZ == 2))) {
        tcg_gen_shli_tl(temp_4, src2, 1);
        tcg_gen_add_tl(address, src1, temp_4);
    }
    setMemory(address, LONG, dest);
    if (instructionHasRegisterOperandIn (0)) {
        pair = nextReg (dest);
        tcg_gen_addi_tl(temp_5, address, 4);
        setMemory(temp_5, LONG, pair);
    } else {
        tcg_gen_movi_tl(tmp, 0);
        TCGLabel *done_1 = gen_new_label();
        tcg_gen_movi_tl(temp_7, 31);
        getBit(temp_6, dest, temp_7);
        tcg_gen_setcondi_tl(TCG_COND_EQ, temp_1, temp_6, 1);
        tcg_gen_xori_tl(temp_2, temp_1, 1);
        tcg_gen_andi_tl(temp_2, temp_2, 1);
        tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, done_1);
        tcg_gen_movi_tl(tmp, 4294967295);
        gen_set_label(done_1);
        tcg_gen_addi_tl(temp_8, address, 4);
        setMemory(temp_8, LONG, tmp);
    }
    if (((AA == 1) || (AA == 2))) {
        tcg_gen_add_tl(src1, src1, src2);
    }
    tcg_temp_free(address);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_5);
    tcg_temp_free(tmp);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_8);

    return ret;
}


/*
 * POP
 *    Variables: @dest
 *    Functions: getMemory, getRegister, setRegister
 * --- code ---
 * {
 *   new_dest = getMemory (getRegister (R_SP), LONG);
 *   setRegister (R_SP, (getRegister (R_SP) + 4));
 *   @dest = new_dest;
 * }
 */

int
arc_gen_POP(DisasCtxt *ctx, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv new_dest = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    getRegister(temp_3, R_SP);
    tcg_gen_mov_tl(temp_2, temp_3);
    getMemory(temp_1, temp_2, LONG);
    tcg_gen_mov_tl(new_dest, temp_1);
    getRegister(temp_6, R_SP);
    tcg_gen_mov_tl(temp_5, temp_6);
    tcg_gen_addi_tl(temp_4, temp_5, 4);
    setRegister(R_SP, temp_4);
    tcg_gen_mov_tl(dest, new_dest);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_1);
    tcg_temp_free(new_dest);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);

    return ret;
}


/*
 * PUSH
 *    Variables: @src
 *    Functions: setMemory, getRegister, setRegister
 * --- code ---
 * {
 *   local_src = @src;
 *   setMemory ((getRegister (R_SP) - 4), LONG, local_src);
 *   setRegister (R_SP, (getRegister (R_SP) - 4));
 * }
 */

int
arc_gen_PUSH(DisasCtxt *ctx, TCGv src)
{
    int ret = DISAS_NEXT;
    TCGv local_src = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    tcg_gen_mov_tl(local_src, src);
    getRegister(temp_3, R_SP);
    tcg_gen_mov_tl(temp_2, temp_3);
    tcg_gen_subi_tl(temp_1, temp_2, 4);
    setMemory(temp_1, LONG, local_src);
    getRegister(temp_6, R_SP);
    tcg_gen_mov_tl(temp_5, temp_6);
    tcg_gen_subi_tl(temp_4, temp_5, 4);
    setRegister(R_SP, temp_4);
    tcg_temp_free(local_src);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);

    return ret;
}


/*
 * LP
 *    Variables: @rd
 *    Functions: getCCFlag, getRegIndex, writeAuxReg, nextInsnAddress, getPCL,
 *               setPC
 * --- code ---
 * {
 *   if((getCCFlag () == true))
 *     {
 *       lp_start_index = getRegIndex (LP_START);
 *       lp_end_index = getRegIndex (LP_END);
 *       writeAuxReg (lp_start_index, nextInsnAddress ());
 *       writeAuxReg (lp_end_index, (getPCL () + @rd));
 *     }
 *   else
 *     {
 *       setPC ((getPCL () + @rd));
 *     };
 * }
 */

int
arc_gen_LP(DisasCtxt *ctx, TCGv rd)
{
    int ret = DISAS_NORETURN;
    TCGv temp_3 = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv lp_start_index = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv lp_end_index = tcg_temp_local_new();
    TCGv temp_7 = tcg_temp_local_new();
    TCGv temp_6 = tcg_temp_local_new();
    TCGv temp_10 = tcg_temp_local_new();
    TCGv temp_9 = tcg_temp_local_new();
    TCGv temp_8 = tcg_temp_local_new();
    TCGv temp_13 = tcg_temp_local_new();
    TCGv temp_12 = tcg_temp_local_new();
    TCGv temp_11 = tcg_temp_local_new();
    TCGLabel *else_1 = gen_new_label();
    TCGLabel *done_1 = gen_new_label();
    getCCFlag(temp_3);
    tcg_gen_setcond_tl(TCG_COND_EQ, temp_1, temp_3, arc_true);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, else_1);
    getRegIndex(temp_4, LP_START);
    tcg_gen_mov_tl(lp_start_index, temp_4);
    getRegIndex(temp_5, LP_END);
    tcg_gen_mov_tl(lp_end_index, temp_5);
    nextInsnAddress(temp_7);
    tcg_gen_mov_tl(temp_6, temp_7);
    writeAuxReg(lp_start_index, temp_6);
    getPCL(temp_10);
    tcg_gen_mov_tl(temp_9, temp_10);
    tcg_gen_add_tl(temp_8, temp_9, rd);
    writeAuxReg(lp_end_index, temp_8);
    tcg_gen_br(done_1);
    gen_set_label(else_1);
    getPCL(temp_13);
    tcg_gen_mov_tl(temp_12, temp_13);
    tcg_gen_add_tl(temp_11, temp_12, rd);
    setPC(temp_11);
    gen_set_label(done_1);
    tcg_temp_free(temp_3);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_4);
    tcg_temp_free(lp_start_index);
    tcg_temp_free(temp_5);
    tcg_temp_free(lp_end_index);
    tcg_temp_free(temp_7);
    tcg_temp_free(temp_6);
    tcg_temp_free(temp_10);
    tcg_temp_free(temp_9);
    tcg_temp_free(temp_8);
    tcg_temp_free(temp_13);
    tcg_temp_free(temp_12);
    tcg_temp_free(temp_11);

    return ret;
}


/*
 * NORM
 *    Variables: @src, @dest
 *    Functions: CRLSB, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   psrc = @src;
 *   @dest = CRLSB (psrc);
 *   if((getFFlag () == true))
 *     {
 *       setZFlag (psrc);
 *       setNFlag (psrc);
 *     };
 * }
 */

int
arc_gen_NORM(DisasCtxt *ctx, TCGv src, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv psrc = tcg_temp_local_new();
    tcg_gen_mov_tl(psrc, src);
    tcg_gen_clrsb_tl(dest, psrc);
    if ((getFFlag () == true)) {
        setZFlag(psrc);
        setNFlag(psrc);
    }
    tcg_temp_free(psrc);

    return ret;
}


/*
 * NORMH
 *    Variables: @src, @dest
 *    Functions: SignExtend16to32, CRLSB, getFFlag, setZFlagByNum, setNFlagByNum
 * --- code ---
 * {
 *   psrc = (@src & 65535);
 *   psrc = SignExtend16to32 (psrc);
 *   @dest = CRLSB (psrc);
 *   @dest = (@dest - 16);
 *   if((getFFlag () == true))
 *     {
 *       setZFlagByNum (psrc, 16);
 *       setNFlagByNum (psrc, 16);
 *     };
 * }
 */

int
arc_gen_NORMH(DisasCtxt *ctx, TCGv src, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv psrc = tcg_temp_local_new();
    tcg_gen_andi_tl(psrc, src, 65535);
    tcg_gen_ext16s_tl(psrc, psrc);
    tcg_gen_clrsb_tl(dest, psrc);
    tcg_gen_subi_tl(dest, dest, 16);
    if ((getFFlag () == true)) {
        setZFlagByNum(psrc, 16);
        setNFlagByNum(psrc, 16);
    }
    tcg_temp_free(psrc);

    return ret;
}


/*
 * FLS
 *    Variables: @src, @dest
 *    Functions: CLZ, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   psrc = @src;
 *   if((psrc == 0))
 *     {
 *       @dest = 0;
 *     }
 *   else
 *     {
 *       @dest = 31 - CLZ (psrc, 32);
 *     };
 *   if((getFFlag () == true))
 *     {
 *       setZFlag (psrc);
 *       setNFlag (psrc);
 *     };
 * }
 */

int
arc_gen_FLS(DisasCtxt *ctx, TCGv src, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv psrc = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_5 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    tcg_gen_mov_tl(psrc, src);
    TCGLabel *else_1 = gen_new_label();
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcondi_tl(TCG_COND_EQ, temp_1, psrc, 0);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, else_1);
    tcg_gen_movi_tl(dest, 0);
    tcg_gen_br(done_1);
    gen_set_label(else_1);
    tcg_gen_movi_tl(temp_5, 32);
    tcg_gen_clz_tl(temp_4, psrc, temp_5);
    tcg_gen_mov_tl(temp_3, temp_4);
    tcg_gen_subfi_tl(dest, 31, temp_3);
    gen_set_label(done_1);
    if ((getFFlag () == true)) {
        setZFlag(psrc);
        setNFlag(psrc);
    }
    tcg_temp_free(psrc);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_5);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_3);

    return ret;
}


/*
 * FFS
 *    Variables: @src, @dest
 *    Functions: CTZ, getFFlag, setZFlag, setNFlag
 * --- code ---
 * {
 *   psrc = @src;
 *   if((psrc == 0))
 *     {
 *       @dest = 31;
 *     }
 *   else
 *     {
 *       @dest = CTZ (psrc, 32);
 *     };
 *   if((getFFlag () == true))
 *     {
 *       setZFlag (psrc);
 *       setNFlag (psrc);
 *     };
 * }
 */

int
arc_gen_FFS(DisasCtxt *ctx, TCGv src, TCGv dest)
{
    int ret = DISAS_NEXT;
    TCGv psrc = tcg_temp_local_new();
    TCGv temp_1 = tcg_temp_local_new();
    TCGv temp_2 = tcg_temp_local_new();
    TCGv temp_4 = tcg_temp_local_new();
    TCGv temp_3 = tcg_temp_local_new();
    tcg_gen_mov_tl(psrc, src);
    TCGLabel *else_1 = gen_new_label();
    TCGLabel *done_1 = gen_new_label();
    tcg_gen_setcondi_tl(TCG_COND_EQ, temp_1, psrc, 0);
    tcg_gen_xori_tl(temp_2, temp_1, 1);
    tcg_gen_andi_tl(temp_2, temp_2, 1);
    tcg_gen_brcond_tl(TCG_COND_EQ, temp_2, arc_true, else_1);
    tcg_gen_movi_tl(dest, 31);
    tcg_gen_br(done_1);
    gen_set_label(else_1);
    tcg_gen_movi_tl(temp_4, 32);
    tcg_gen_ctz_tl(temp_3, psrc, temp_4);
    tcg_gen_mov_tl(dest, temp_3);
    gen_set_label(done_1);
    if ((getFFlag () == true)) {
        setZFlag(psrc);
        setNFlag(psrc);
    }
    tcg_temp_free(psrc);
    tcg_temp_free(temp_1);
    tcg_temp_free(temp_2);
    tcg_temp_free(temp_4);
    tcg_temp_free(temp_3);

    return ret;
}

static void
arc_check_dest_reg_is_even_or_null(DisasCtxt *ctx, TCGv reg)
{
  ptrdiff_t n = tcgv_i32_temp(reg) - tcgv_i32_temp(cpu_r[0]);
  if (n >= 0 && n < 64) {
    /* REG is an odd register. */
    if (n % 2 != 0)
      arc_gen_excp(ctx, EXCP_INST_ERROR, 0, 0);
  }
}

static void
arc_gen_next_register_i32_i64(DisasCtxt *ctx,
                              TCGv_i64 dest, TCGv_i32 reg)
{
  ptrdiff_t n = tcgv_i32_temp(reg) - tcgv_i32_temp(cpu_r[0]);
  if (n >= 0 && n < 64) {
    /* Check if REG is an even register. */
    if (n % 2 == 0) {
      if (n == 62) { /* limm */
        tcg_gen_concat_i32_i64(dest, reg, reg);
        tcg_gen_andi_i64(dest, dest, 0xffffffff);
      } else { /* normal register */
        tcg_gen_concat_i32_i64(dest, reg, cpu_r[n + 1]);
      }
    } else { /* if REG is an odd register, thows an exception */
      arc_gen_excp(ctx, EXCP_INST_ERROR, 0, 0);
    }
  } else { /* u6 or s12 */
    tcg_gen_concat_i32_i64(dest, reg, reg);
  }
}

static void
arc_gen_vec_pair_i32(DisasCtxt *ctx,
                     TCGv_i32 dest, TCGv_i32 b, TCGv_i32 c,
                     void (*OP)(TCGv_i64, TCGv_i64, TCGv_i64))
{
  TCGv_i64 t1 = tcg_temp_new_i64();
  TCGv_i64 t2 = tcg_temp_new_i64();
  TCGv_i64 t3 = tcg_temp_new_i64();

  /* check if dest is an even or a null register */
  arc_check_dest_reg_is_even_or_null(ctx, dest);

  /* t2 = [next(b):b] */
  arc_gen_next_register_i32_i64(ctx, t2, b);
  /* t3 = [next(c):c] */
  arc_gen_next_register_i32_i64(ctx, t3, c);

  /* execute the instruction operation */
  OP(t1, t2, t3);

  /* save the result on [next(dest):dest] */
  tcg_gen_extrl_i64_i32(dest, t1);
  tcg_gen_extrh_i64_i32(nextRegWithNull(dest), t1);

  tcg_temp_free_i64(t3);
  tcg_temp_free_i64(t2);
  tcg_temp_free_i64(t1);
}

/*
 * VMAC2H and VMAC2HU
 */

static void
arc_gen_vmac2h_i32(DisasCtxt *ctx, TCGv dest, TCGv b, TCGv c,
                   void (*OP)(TCGv, TCGv, unsigned int, unsigned int))
{
  TCGv b_h0, b_h1, c_h0, c_h1;

  arc_check_dest_reg_is_even_or_null(ctx, dest);

  b_h0 = tcg_temp_new();
  b_h1 = tcg_temp_new();
  c_h0 = tcg_temp_new();
  c_h1 = tcg_temp_new();

  OP(b_h0, b, 0, 16);
  OP(c_h0, c, 0, 16);
  OP(b_h1, b, 16, 16);
  OP(c_h1, c, 16, 16);

  tcg_gen_mul_tl(b_h0, b_h0, c_h0);
  tcg_gen_mul_tl(b_h1, b_h1, c_h1);

  tcg_gen_add_tl(cpu_acclo, cpu_acclo, b_h0);
  tcg_gen_add_tl(cpu_acchi, cpu_acchi, b_h1);
  tcg_gen_mov_tl(dest, cpu_acclo);
  tcg_gen_mov_tl(nextRegWithNull(dest), cpu_acchi);

  tcg_temp_free(c_h1);
  tcg_temp_free(c_h0);
  tcg_temp_free(b_h1);
  tcg_temp_free(b_h0);
}

int
arc_gen_VMAC2H(DisasCtxt *ctx, TCGv dest, TCGv b, TCGv c)
{
  TCGv cc_temp = tcg_temp_local_new();
  TCGLabel *cc_done = gen_new_label();

  getCCFlag(cc_temp);
  tcg_gen_brcondi_tl(TCG_COND_EQ, cc_temp, 0, cc_done);

  arc_gen_vmac2h_i32(ctx, dest, b, c, tcg_gen_sextract_i32);

  gen_set_label(cc_done);
  tcg_temp_free(cc_temp);

  return DISAS_NEXT;
}

int
arc_gen_VMAC2HU(DisasCtxt *ctx, TCGv dest, TCGv b, TCGv c)
{
  TCGv cc_temp = tcg_temp_local_new();
  TCGLabel *cc_done = gen_new_label();

  getCCFlag(cc_temp);
  tcg_gen_brcondi_tl(TCG_COND_EQ, cc_temp, 0, cc_done);

  arc_gen_vmac2h_i32(ctx, dest, b, c, tcg_gen_extract_i32);

  gen_set_label(cc_done);
  tcg_temp_free(cc_temp);

  return DISAS_NEXT;
}

/*
 * VADD: VADD2, VADD2H, VADD4H
 */

int
arc_gen_VADD2(DisasCtxt *ctx, TCGv dest, TCGv b, TCGv c)
{
  TCGv cc_temp = tcg_temp_local_new();
  TCGLabel *cc_done = gen_new_label();

  getCCFlag(cc_temp);
  tcg_gen_brcondi_tl(TCG_COND_EQ, cc_temp, 0, cc_done);

  arc_gen_vec_pair_i32(ctx, dest, b, c,
                       tcg_gen_vec_add32_i64);

  gen_set_label(cc_done);
  tcg_temp_free(cc_temp);

  return DISAS_NEXT;
}

int
arc_gen_VADD2H(DisasCtxt *ctx, TCGv dest, TCGv b, TCGv c)
{
  TCGv cc_temp = tcg_temp_local_new();
  TCGLabel *cc_done = gen_new_label();

  getCCFlag(cc_temp);
  tcg_gen_brcondi_tl(TCG_COND_EQ, cc_temp, 0, cc_done);

  tcg_gen_vec_add16_i32(dest, b, c);

  gen_set_label(cc_done);
  tcg_temp_free(cc_temp);

  return DISAS_NEXT;
}

int
arc_gen_VADD4H(DisasCtxt *ctx, TCGv dest, TCGv b, TCGv c)
{
  TCGv cc_temp = tcg_temp_local_new();
  TCGLabel *cc_done = gen_new_label();

  getCCFlag(cc_temp);
  tcg_gen_brcondi_tl(TCG_COND_EQ, cc_temp, 0, cc_done);

  arc_gen_vec_pair_i32(ctx, dest, b, c,
                       tcg_gen_vec_add16_i64);

  gen_set_label(cc_done);
  tcg_temp_free(cc_temp);

  return DISAS_NEXT;
}

/*
 * VSUB: VSUB2, VSUB2H, VSUB4H
 */

int
arc_gen_VSUB2(DisasCtxt *ctx, TCGv dest, TCGv b, TCGv c)
{
  TCGv cc_temp = tcg_temp_local_new();
  TCGLabel *cc_done = gen_new_label();

  getCCFlag(cc_temp);
  tcg_gen_brcondi_tl(TCG_COND_EQ, cc_temp, 0, cc_done);

  arc_gen_vec_pair_i32(ctx, dest, b, c,
                       tcg_gen_vec_sub32_i64);

  gen_set_label(cc_done);
  tcg_temp_free(cc_temp);

  return DISAS_NEXT;
}

int
arc_gen_VSUB2H(DisasCtxt *ctx, TCGv dest, TCGv b, TCGv c)
{
  TCGv cc_temp = tcg_temp_local_new();
  TCGLabel *cc_done = gen_new_label();

  getCCFlag(cc_temp);
  tcg_gen_brcondi_tl(TCG_COND_EQ, cc_temp, 0, cc_done);

  tcg_gen_vec_sub16_i32(dest, b, c);

  gen_set_label(cc_done);
  tcg_temp_free(cc_temp);

  return DISAS_NEXT;
}

int
arc_gen_VSUB4H(DisasCtxt *ctx, TCGv dest, TCGv b, TCGv c)
{
  TCGv cc_temp = tcg_temp_local_new();
  TCGLabel *cc_done = gen_new_label();

  getCCFlag(cc_temp);
  tcg_gen_brcondi_tl(TCG_COND_EQ, cc_temp, 0, cc_done);

  arc_gen_vec_pair_i32(ctx, dest, b, c,
                       tcg_gen_vec_sub16_i64);

  gen_set_label(cc_done);
  tcg_temp_free(cc_temp);

  return DISAS_NEXT;
}

/*
 * VADDSUB and VSUBADD operations
 */

static void
arc_gen_cmpl2_i32(TCGv_i32 ret, TCGv_i32 arg1,
                  unsigned int ofs, unsigned int len)
{
    TCGv_i32 t1 = tcg_temp_new_i32();
    TCGv_i32 t2 = tcg_temp_new_i32();

    tcg_gen_mov_i32(t1, arg1);
    tcg_gen_extract_i32(t2, t1, ofs, len);
    tcg_gen_not_i32(t2, t2);
    tcg_gen_addi_i32(t2, t2, 1);
    tcg_gen_deposit_i32(t1, t1, t2, ofs, len);
    tcg_gen_mov_i32(ret, t1);

    tcg_temp_free_i32(t2);
	tcg_temp_free_i32(t1);
}

#define ARC_GEN_CMPL2_H0_I32(RET, ARG1)     arc_gen_cmpl2_i32(RET, ARG1, 0, 16)
#define ARC_GEN_CMPL2_H1_I32(RET, ARG1)     arc_gen_cmpl2_i32(RET, ARG1, 16, 16)

#define VEC_VADDSUB_VSUBADD_OP(NAME, FIELD, OP, TL)             \
static void                                                     \
arc_gen_##NAME##_op(TCGv_##TL dest, TCGv_##TL b, TCGv_##TL c)   \
{                                                               \
    TCGv_##TL t1 = tcg_temp_new_##TL();                         \
                                                                \
    ARC_GEN_CMPL2_##FIELD(t1, c);                               \
    tcg_gen_vec_##OP##_##TL(dest, b, t1);                       \
                                                                \
    tcg_temp_free_##TL(t1);                                     \
}

VEC_VADDSUB_VSUBADD_OP(vaddsub, W1_I64, add32, i64)
VEC_VADDSUB_VSUBADD_OP(vaddsub2h, H1_I32, add16, i32)
VEC_VADDSUB_VSUBADD_OP(vaddsub4h, H1_H3_I64, add16, i64)
VEC_VADDSUB_VSUBADD_OP(vsubadd, W0_I64, add32, i64)
VEC_VADDSUB_VSUBADD_OP(vsubadd2h, H0_I32, add16, i32)
VEC_VADDSUB_VSUBADD_OP(vsubadd4h, H0_H2_I64, add16, i64)

/*
 * VADDSUB: VADDSUB, VADDSUB2H, VADDSUB4H
 */

int
arc_gen_VADDSUB(DisasCtxt *ctx, TCGv dest, TCGv b, TCGv c)
{
  TCGv cc_temp = tcg_temp_local_new();
  TCGLabel *cc_done = gen_new_label();

  getCCFlag(cc_temp);
  tcg_gen_brcondi_tl(TCG_COND_EQ, cc_temp, 0, cc_done);

  arc_gen_vec_pair_i32(ctx, dest, b, c,
                       arc_gen_vaddsub_op);

  gen_set_label(cc_done);
  tcg_temp_free(cc_temp);

  return DISAS_NEXT;
}

int
arc_gen_VADDSUB2H(DisasCtxt *ctx, TCGv dest, TCGv b, TCGv c)
{
  TCGv cc_temp = tcg_temp_local_new();
  TCGLabel *cc_done = gen_new_label();

  getCCFlag(cc_temp);
  tcg_gen_brcondi_tl(TCG_COND_EQ, cc_temp, 0, cc_done);

  arc_gen_vaddsub2h_op(dest, b, c);

  gen_set_label(cc_done);
  tcg_temp_free(cc_temp);

  return DISAS_NEXT;
}

int
arc_gen_VADDSUB4H(DisasCtxt *ctx, TCGv dest, TCGv b, TCGv c)
{
  TCGv cc_temp = tcg_temp_local_new();
  TCGLabel *cc_done = gen_new_label();

  getCCFlag(cc_temp);
  tcg_gen_brcondi_tl(TCG_COND_EQ, cc_temp, 0, cc_done);

  arc_gen_vec_pair_i32(ctx, dest, b, c,
                       arc_gen_vaddsub4h_op);

  gen_set_label(cc_done);
  tcg_temp_free(cc_temp);

  return DISAS_NEXT;
}

/*
 * VSUBADD: VSUBADD, VSUBADD2H, VSUBADD4H
 */

int
arc_gen_VSUBADD(DisasCtxt *ctx, TCGv dest, TCGv b, TCGv c)
{
  TCGv cc_temp = tcg_temp_local_new();
  TCGLabel *cc_done = gen_new_label();

  getCCFlag(cc_temp);
  tcg_gen_brcondi_tl(TCG_COND_EQ, cc_temp, 0, cc_done);

  arc_gen_vec_pair_i32(ctx, dest, b, c,
                       arc_gen_vsubadd_op);

  gen_set_label(cc_done);
  tcg_temp_free(cc_temp);

  return DISAS_NEXT;
}

int
arc_gen_VSUBADD2H(DisasCtxt *ctx, TCGv dest, TCGv b, TCGv c)
{
  TCGv cc_temp = tcg_temp_local_new();
  TCGLabel *cc_done = gen_new_label();

  getCCFlag(cc_temp);
  tcg_gen_brcondi_tl(TCG_COND_EQ, cc_temp, 0, cc_done);

  arc_gen_vsubadd2h_op(dest, b, c);

  gen_set_label(cc_done);
  tcg_temp_free(cc_temp);

  return DISAS_NEXT;
}

int
arc_gen_VSUBADD4H(DisasCtxt *ctx, TCGv dest, TCGv b, TCGv c)
{
  TCGv cc_temp = tcg_temp_local_new();
  TCGLabel *cc_done = gen_new_label();

  getCCFlag(cc_temp);
  tcg_gen_brcondi_tl(TCG_COND_EQ, cc_temp, 0, cc_done);

  arc_gen_vec_pair_i32(ctx, dest, b, c,
                       arc_gen_vsubadd4h_op);

  gen_set_label(cc_done);
  tcg_temp_free(cc_temp);

  return DISAS_NEXT;
}

ARC_GEN_32BIT_INTERFACE(QMACH, PAIR, PAIR, PAIR, SIGNED, \
                        arc_gen_qmach_base_i64);

int
arc_gen_QMACH(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    ARC_GEN_SEMFUNC_INIT();

    arc_autogen_base32_QMACH(ctx, a, b, c);

    ARC_GEN_SEMFUNC_DEINIT();

    return DISAS_NEXT;
}

ARC_GEN_32BIT_INTERFACE(QMACHU, PAIR, PAIR, PAIR, UNSIGNED, \
                        arc_gen_qmach_base_i64);

int
arc_gen_QMACHU(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    ARC_GEN_SEMFUNC_INIT();

    arc_autogen_base32_QMACHU(ctx, a, b, c);

    ARC_GEN_SEMFUNC_DEINIT();

    return DISAS_NEXT;
}

ARC_GEN_32BIT_INTERFACE(DMACWH, PAIR, PAIR, NOT_PAIR, SIGNED, \
                        arc_gen_dmacwh_base_i64);

int
arc_gen_DMACWH(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    ARC_GEN_SEMFUNC_INIT();

    arc_autogen_base32_DMACWH(ctx, a, b, c);

    ARC_GEN_SEMFUNC_DEINIT();

    return DISAS_NEXT;
}

ARC_GEN_32BIT_INTERFACE(DMACWHU, PAIR, PAIR, NOT_PAIR, UNSIGNED, \
                        arc_gen_dmacwh_base_i64);

int
arc_gen_DMACWHU(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    ARC_GEN_SEMFUNC_INIT();

    arc_autogen_base32_DMACWHU(ctx, a, b, c);

    ARC_GEN_SEMFUNC_DEINIT();

    return DISAS_NEXT;
}

ARC_GEN_32BIT_INTERFACE(DMACH, NOT_PAIR, NOT_PAIR, NOT_PAIR, SIGNED, \
                        arc_gen_dmach_base_i64);

int
arc_gen_DMACH(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
  ARC_GEN_SEMFUNC_INIT();

  arc_autogen_base32_DMACH(ctx, a, b, c);

  ARC_GEN_SEMFUNC_DEINIT();

  return DISAS_NEXT;
}

ARC_GEN_32BIT_INTERFACE(DMACHU, NOT_PAIR, NOT_PAIR, NOT_PAIR, UNSIGNED, \
                        arc_gen_dmach_base_i64);

int
arc_gen_DMACHU(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
  ARC_GEN_SEMFUNC_INIT();

  arc_autogen_base32_DMACHU(ctx, a, b, c);

  ARC_GEN_SEMFUNC_DEINIT();

  return DISAS_NEXT;
}

ARC_GEN_32BIT_INTERFACE(DMPYH, NOT_PAIR, NOT_PAIR, NOT_PAIR, SIGNED, \
                        arc_gen_dmpyh_base_i64);

int
arc_gen_DMPYH(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    ARC_GEN_SEMFUNC_INIT();

    arc_autogen_base32_DMPYH(ctx, a, b, c);

    ARC_GEN_SEMFUNC_DEINIT();

    return DISAS_NEXT;
}

ARC_GEN_32BIT_INTERFACE(DMPYHU, NOT_PAIR, NOT_PAIR, NOT_PAIR, UNSIGNED, \
                        arc_gen_dmpyh_base_i64);

int
arc_gen_DMPYHU(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    ARC_GEN_SEMFUNC_INIT();

    arc_autogen_base32_DMPYHU(ctx, a, b, c);

    ARC_GEN_SEMFUNC_DEINIT();

    return DISAS_NEXT;
}

ARC_GEN_32BIT_INTERFACE(QMPYH, PAIR, PAIR, PAIR, SIGNED, \
                        arc_gen_qmpyh_base_i64);

int
arc_gen_QMPYH(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    ARC_GEN_SEMFUNC_INIT();

    arc_autogen_base32_QMPYH(ctx, a, b, c);

    ARC_GEN_SEMFUNC_DEINIT();

    return DISAS_NEXT;
}

ARC_GEN_32BIT_INTERFACE(QMPYHU, PAIR, PAIR, PAIR, UNSIGNED, \
                        arc_gen_qmpyh_base_i64);

int
arc_gen_QMPYHU(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    ARC_GEN_SEMFUNC_INIT();

    arc_autogen_base32_QMPYHU(ctx, a, b, c);

    ARC_GEN_SEMFUNC_DEINIT();

    return DISAS_NEXT;
}

ARC_GEN_32BIT_INTERFACE(DMPYWH, PAIR, PAIR, NOT_PAIR, SIGNED, \
                        arc_gen_dmpywh_base_i64);

int
arc_gen_DMPYWH(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    ARC_GEN_SEMFUNC_INIT();

    arc_autogen_base32_DMPYWH(ctx, a, b, c);

    ARC_GEN_SEMFUNC_DEINIT();

    return DISAS_NEXT;
}

ARC_GEN_32BIT_INTERFACE(DMPYWHU, PAIR, PAIR, NOT_PAIR, UNSIGNED, \
                        arc_gen_dmpywh_base_i64);

int
arc_gen_DMPYWHU(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    ARC_GEN_SEMFUNC_INIT();

    arc_autogen_base32_DMPYWHU(ctx, a, b, c);

    ARC_GEN_SEMFUNC_DEINIT();

    return DISAS_NEXT;
}

ARC_GEN_32BIT_INTERFACE(VMPY2H, PAIR, NOT_PAIR, NOT_PAIR, SIGNED, \
                        arc_gen_vmpy2h_base_i64);

int
arc_gen_VMPY2H(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    ARC_GEN_SEMFUNC_INIT();

    arc_autogen_base32_VMPY2H(ctx, a, b, c);

    ARC_GEN_SEMFUNC_DEINIT();

    return DISAS_NEXT;
}

ARC_GEN_32BIT_INTERFACE(VMPY2HU, PAIR, NOT_PAIR, NOT_PAIR, UNSIGNED, \
                        arc_gen_vmpy2h_base_i64);

int
arc_gen_VMPY2HU(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    ARC_GEN_SEMFUNC_INIT();

    arc_autogen_base32_VMPY2HU(ctx, a, b, c);

    ARC_GEN_SEMFUNC_DEINIT();

    return DISAS_NEXT;
}

ARC_GEN_32BIT_INTERFACE(MPYD, PAIR, NOT_PAIR, NOT_PAIR, SIGNED, \
                        arc_gen_mpyd_base_i64);

int
arc_gen_MPYD(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    ARC_GEN_SEMFUNC_INIT();

    arc_autogen_base32_MPYD(ctx, a, b, c);


    ARC_GEN_SEMFUNC_DEINIT();

    return DISAS_NEXT;
}

ARC_GEN_32BIT_INTERFACE(MPYDU, PAIR, NOT_PAIR, NOT_PAIR, UNSIGNED, \
                        arc_gen_mpyd_base_i64);

int
arc_gen_MPYDU(DisasCtxt *ctx, TCGv a, TCGv b, TCGv c)
{
    ARC_GEN_SEMFUNC_INIT();

    arc_autogen_base32_MPYDU(ctx, a, b, c);


    ARC_GEN_SEMFUNC_DEINIT();

    return DISAS_NEXT;
}
