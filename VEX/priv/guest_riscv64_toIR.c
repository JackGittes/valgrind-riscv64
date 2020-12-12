
/*--------------------------------------------------------------------*/
/*--- begin                                   guest_riscv64_toIR.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of Valgrind, a dynamic binary instrumentation
   framework.

   Copyright (C) 2020-2021 Petr Pavlu
      setup@dagobah.cz

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.

   The GNU General Public License is contained in the file COPYING.

   Neither the names of the U.S. Department of Energy nor the
   University of California nor the names of its contributors may be
   used to endorse or promote products derived from this software
   without prior written permission.
*/

/* Translates riscv64 code to IR. */

#include "libvex_guest_riscv64.h"

#include "guest_riscv64_defs.h"
#include "main_globals.h"
#include "main_util.h"

/*------------------------------------------------------------*/
/*--- Debugging output                                     ---*/
/*------------------------------------------------------------*/

#define DIP(format, args...)                                                   \
   do {                                                                        \
      if (vex_traceflags & VEX_TRACE_FE)                                       \
         vex_printf(format, ##args);                                           \
   } while (0)

#define DIS(buf, format, args...)                                              \
   do {                                                                        \
      if (vex_traceflags & VEX_TRACE_FE)                                       \
         vex_sprintf(buf, format, ##args);                                     \
   } while (0)

/*------------------------------------------------------------*/
/*--- Helper bits and pieces for deconstructing the        ---*/
/*--- riscv64 insn stream.                                 ---*/
/*------------------------------------------------------------*/

/* Do read of an instruction, which can be 16-bit (compressed) or 32-bit in
   size. */
static inline UInt getInsn(const UChar* p)
{
   Bool is_compressed = (p[0] & 0x3) != 0x3;
   UInt w = 0;
   if (!is_compressed) {
      w = (w << 8) | p[3];
      w = (w << 8) | p[2];
   }
   w = (w << 8) | p[1];
   w = (w << 8) | p[0];
   return w;
}

/* Sign extend a N-bit value up to 64 bits, by copying bit N-1 into all higher
   positions. */
static ULong sx_to_64(ULong x, UInt n)
{
   vassert(n > 1 && n < 64);
   x <<= (64 - n);
   Long r = (Long)x;
   r >>= (64 - n);
   return (ULong)r;
}

#define BITS2(_b1, _b0)      (((_b1) << 1) | (_b0))
#define BITS3(_b2, _b1, _b0) (((_b2) << 2) | ((_b1) << 1) | (_b0))

#define X00 BITS2(0, 0)
#define X01 BITS2(0, 1)
#define X10 BITS2(1, 0)
#define X11 BITS2(1, 1)

/* Produce _uint[_bMax:_bMin]. */
#define SLICE_UInt(_uint, _bMax, _bMin)                                        \
   ((((UInt)(_uint)) >> (_bMin)) &                                             \
    (UInt)((1ULL << ((_bMax) - (_bMin) + 1)) - 1ULL))

/*------------------------------------------------------------*/
/*--- Helpers for constructing IR.                         ---*/
/*------------------------------------------------------------*/

/* Generate a new temporary of the given type. */
static IRTemp newTemp(/*OUT*/ IRSB* irsb, IRType ty)
{
   vassert(isPlausibleIRType(ty));
   return newIRTemp(irsb->tyenv, ty);
}

/* Add a statement to the list held by irsb. */
static void stmt(/*OUT*/ IRSB* irsb, /*IN*/ IRStmt* st)
{
   addStmtToIRSB(irsb, st);
}

/* Create an expression to produce a constant. */
static IRExpr* mkU64(ULong i)
{
   return IRExpr_Const(IRConst_U64(i));
}

/*------------------------------------------------------------*/
/*--- Offsets of various parts of the riscv64 guest state  ---*/
/*------------------------------------------------------------*/

#define OFFB_X0  offsetof(VexGuestRISCV64State, guest_x0)
#define OFFB_X1  offsetof(VexGuestRISCV64State, guest_x1)
#define OFFB_X2  offsetof(VexGuestRISCV64State, guest_x2)
#define OFFB_X3  offsetof(VexGuestRISCV64State, guest_x3)
#define OFFB_X4  offsetof(VexGuestRISCV64State, guest_x4)
#define OFFB_X5  offsetof(VexGuestRISCV64State, guest_x5)
#define OFFB_X6  offsetof(VexGuestRISCV64State, guest_x6)
#define OFFB_X7  offsetof(VexGuestRISCV64State, guest_x7)
#define OFFB_X8  offsetof(VexGuestRISCV64State, guest_x8)
#define OFFB_X9  offsetof(VexGuestRISCV64State, guest_x9)
#define OFFB_X10 offsetof(VexGuestRISCV64State, guest_x10)
#define OFFB_X11 offsetof(VexGuestRISCV64State, guest_x11)
#define OFFB_X12 offsetof(VexGuestRISCV64State, guest_x12)
#define OFFB_X13 offsetof(VexGuestRISCV64State, guest_x13)
#define OFFB_X14 offsetof(VexGuestRISCV64State, guest_x14)
#define OFFB_X15 offsetof(VexGuestRISCV64State, guest_x15)
#define OFFB_X16 offsetof(VexGuestRISCV64State, guest_x16)
#define OFFB_X17 offsetof(VexGuestRISCV64State, guest_x17)
#define OFFB_X18 offsetof(VexGuestRISCV64State, guest_x18)
#define OFFB_X19 offsetof(VexGuestRISCV64State, guest_x19)
#define OFFB_X20 offsetof(VexGuestRISCV64State, guest_x20)
#define OFFB_X21 offsetof(VexGuestRISCV64State, guest_x21)
#define OFFB_X22 offsetof(VexGuestRISCV64State, guest_x22)
#define OFFB_X23 offsetof(VexGuestRISCV64State, guest_x23)
#define OFFB_X24 offsetof(VexGuestRISCV64State, guest_x24)
#define OFFB_X25 offsetof(VexGuestRISCV64State, guest_x25)
#define OFFB_X26 offsetof(VexGuestRISCV64State, guest_x26)
#define OFFB_X27 offsetof(VexGuestRISCV64State, guest_x27)
#define OFFB_X28 offsetof(VexGuestRISCV64State, guest_x28)
#define OFFB_X29 offsetof(VexGuestRISCV64State, guest_x29)
#define OFFB_X30 offsetof(VexGuestRISCV64State, guest_x30)
#define OFFB_X31 offsetof(VexGuestRISCV64State, guest_x31)
#define OFFB_PC  offsetof(VexGuestRISCV64State, guest_pc)

#define OFFB_EMNOTE        offsetof(VexGuestRISCV64State, guest_EMNOTE)
#define OFFB_CMSTART       offsetof(VexGuestRISCV64State, guest_CMSTART)
#define OFFB_CMLEN         offsetof(VexGuestRISCV64State, guest_CMLEN)
#define OFFB_NRADDR        offsetof(VexGuestRISCV64State, guest_NRADDR)
#define OFFB_IP_AT_SYSCALL offsetof(VexGuestRISCV64State, guest_IP_AT_SYSCALL)

/*------------------------------------------------------------*/
/*--- Integer registers                                    ---*/
/*------------------------------------------------------------*/

static Int offsetIReg64(UInt iregNo)
{
   switch (iregNo) {
      case 0:
         return OFFB_X0;
      case 1:
         return OFFB_X1;
      case 2:
         return OFFB_X2;
      case 3:
         return OFFB_X3;
      case 4:
         return OFFB_X4;
      case 5:
         return OFFB_X5;
      case 6:
         return OFFB_X6;
      case 7:
         return OFFB_X7;
      case 8:
         return OFFB_X8;
      case 9:
         return OFFB_X9;
      case 10:
         return OFFB_X10;
      case 11:
         return OFFB_X11;
      case 12:
         return OFFB_X12;
      case 13:
         return OFFB_X13;
      case 14:
         return OFFB_X14;
      case 15:
         return OFFB_X15;
      case 16:
         return OFFB_X16;
      case 17:
         return OFFB_X17;
      case 18:
         return OFFB_X18;
      case 19:
         return OFFB_X19;
      case 20:
         return OFFB_X20;
      case 21:
         return OFFB_X21;
      case 22:
         return OFFB_X22;
      case 23:
         return OFFB_X23;
      case 24:
         return OFFB_X24;
      case 25:
         return OFFB_X25;
      case 26:
         return OFFB_X26;
      case 27:
         return OFFB_X27;
      case 28:
         return OFFB_X28;
      case 29:
         return OFFB_X29;
      case 30:
         return OFFB_X30;
      case 31:
         return OFFB_X31;
      default:
         vassert(0);
   }
}

/* Obtain ABI name of a register. */
static const HChar* nameIReg64(UInt iregNo)
{
   vassert(iregNo < 32);
   static const HChar* names[32] = {
      "zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0",
      "a1",   "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
      "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
   return names[iregNo];
}

static IRExpr* getIReg64(UInt iregNo)
{
   vassert(iregNo < 32);
   return IRExpr_Get(offsetIReg64(iregNo), Ity_I64);
}

static void putIReg64(/*OUT*/ IRSB* irsb, UInt iregNo, /*IN*/ IRExpr* e)
{
   vassert(typeOfIRExpr(irsb->tyenv, e) == Ity_I64);
   stmt(irsb, IRStmt_Put(offsetIReg64(iregNo), e));
}

/*------------------------------------------------------------*/
/*--- Disassemble a single instruction                     ---*/
/*------------------------------------------------------------*/

static Bool dis_RISCV64_compressed_00(/*MB_OUT*/ DisResult* dres,
                                      /*OUT*/ IRSB*         irsb,
                                      UInt                  insn,
                                      Bool                  sigill_diag)
{
#define INSN(_bMax, _bMin) SLICE_UInt(insn, (_bMax), (_bMin))

   /* TODO */

   if (sigill_diag)
      vex_printf("RISCV64 front end: compressed_00\n");
   return False;
#undef INSN
}

static Bool dis_RISCV64_compressed_01(/*MB_OUT*/ DisResult* dres,
                                      /*OUT*/ IRSB*         irsb,
                                      UInt                  insn,
                                      Bool                  sigill_diag)
{
#define INSN(_bMax, _bMin) SLICE_UInt(insn, (_bMax), (_bMin))

   /* ---------------- lui rd, nzimm[17:12] ----------------- */
   if (INSN(15, 13) == BITS3(0, 1, 1)) {
      UInt rd = INSN(11, 7);
      UInt nzimm = INSN(12, 12) << 17 | INSN(6, 2) << 12;
      if (rd == 0 || rd == 2 || nzimm == 0) {
         /* Invalid C.LUI, fall through. */
      } else {
         putIReg64(irsb, rd, mkU64(sx_to_64(nzimm, 18)));
         DIP("lui %s, 0x%x\n", nameIReg64(rd), nzimm >> 12);
         return True;
      }
   }

   if (sigill_diag)
      vex_printf("RISCV64 front end: compressed_01\n");
   return False;
#undef INSN
}

static Bool dis_RISCV64_compressed_10(/*MB_OUT*/ DisResult* dres,
                                      /*OUT*/ IRSB*         irsb,
                                      UInt                  insn,
                                      Bool                  sigill_diag)
{
#define INSN(_bMax, _bMin) SLICE_UInt(insn, (_bMax), (_bMin))

   /* TODO */

   if (sigill_diag)
      vex_printf("RISCV64 front end: compressed_10\n");
   return False;
#undef INSN
}

/* Disassemble a single riscv64 instruction into IR. Returns True iff the
   instruction was decoded, in which case *dres will be set accordingly, or
   False, in which case *dres should be ignored by the caller. */
static Bool disInstr_RISCV64_WRK(/*MB_OUT*/ DisResult* dres,
                                 /*OUT*/ IRSB*         irsb,
                                 const UChar*          guest_instr,
                                 Addr                  guest_pc_curr_instr,
                                 const VexArchInfo*    archinfo,
                                 const VexAbiInfo*     abiinfo,
                                 Bool                  sigill_diag)
{
   /* A macro to fish bits out of 'insn'. */
#define INSN(_bMax, _bMin) SLICE_UInt(insn, (_bMax), (_bMin))

   /* Set result defaults. */
   dres->whatNext = Dis_Continue;
   dres->len = 4;
   dres->jk_StopHere = Ijk_INVALID;
   dres->hint = Dis_HintNone;

   /* Read the instruction word. */
   UInt insn = getInsn(guest_instr);

   if (0)
      vex_printf("insn: 0x%x\n", insn);

   DIP("\t(riscv64) 0x%llx:  ", (ULong)guest_pc_curr_instr);

   vassert(0 == (guest_pc_curr_instr & 3ULL));

   /* Spot "Special" instructions (see comment at top of file). */
   {
      /* TODO */
   }

   /* Main riscv64 instruction decoder starts here. */
   Bool ok = False;

   /* Parse insn[1:0] to determine whether the instruction is 16-bit
      (compressed) or 32-bit. */
   switch (INSN(1, 0)) {
      case X00:
         ok = dis_RISCV64_compressed_00(dres, irsb, insn, sigill_diag);
         break;
      case X01:
         ok = dis_RISCV64_compressed_01(dres, irsb, insn, sigill_diag);
         break;
      case X10:
         ok = dis_RISCV64_compressed_10(dres, irsb, insn, sigill_diag);
         break;
      case X11:
         /* TODO */
         break;
      default:
         vassert(0); /* Can't happen. */
   }

   /* If the next-level down decoders failed, make sure dres didn't get
      changed. */
   if (!ok) {
      vassert(dres->whatNext == Dis_Continue);
      vassert(dres->len == 4);
      vassert(dres->jk_StopHere == Ijk_INVALID);
   }

   return ok;
#undef INSN
}

/*------------------------------------------------------------*/
/*--- Top-level fn                                         ---*/
/*------------------------------------------------------------*/

/* Disassemble a single instruction into IR. The instruction is located in host
   memory at &guest_code[delta]. */
DisResult disInstr_RISCV64(IRSB*              irsb,
                           const UChar*       guest_code,
                           Long               delta,
                           Addr               guest_IP,
                           VexArch            guest_arch,
                           const VexArchInfo* archinfo,
                           const VexAbiInfo*  abiinfo,
                           VexEndness         host_endness,
                           Bool               sigill_diag)
{
   DisResult dres;
   vex_bzero(&dres, sizeof(dres));

   vassert(guest_arch == VexArchRISCV64);

   /* Try to decode. */
   Bool ok = disInstr_RISCV64_WRK(&dres,
                                  irsb,
                                  &guest_code[delta],
                                  guest_IP,
                                  archinfo,
                                  abiinfo,
                                  sigill_diag);
   if (ok) {
      /* All decode successes end up here. */
      vassert(dres.len == 4 || dres.len == 20);
      switch (dres.whatNext) {
         case Dis_Continue:
            stmt(irsb, IRStmt_Put(OFFB_PC, mkU64(guest_IP + dres.len)));
            break;
         case Dis_StopHere:
            break;
         default:
            vassert(0);
      }
      DIP("\n");
   } else {
      /* All decode failures end up here. */
      if (sigill_diag) {
         Int   i, j;
         UChar buf[64];
         UInt  insn = getInsn(&guest_code[delta]);
         vex_bzero(buf, sizeof(buf));
         for (i = j = 0; i < 32; i++) {
            if (i > 0) {
               if ((i & 7) == 0)
                  buf[j++] = ' ';
               else if ((i & 3) == 0)
                  buf[j++] = '\'';
            }
            buf[j++] = (insn & (1 << (31 - i))) ? '1' : '0';
         }
         vex_printf("disInstr(riscv64): unhandled instruction 0x%08x\n", insn);
         vex_printf("disInstr(riscv64): %s\n", buf);
      }

      /* Tell the dispatcher that this insn cannot be decoded, and so has not
         been executed, and (is currently) the next to be executed. The pc
         register should be up-to-date since it is made so at the start of each
         insn, but nevertheless be paranoid and update it again right now. */
      stmt(irsb, IRStmt_Put(OFFB_PC, mkU64(guest_IP)));
      dres.len = 0;
      dres.whatNext = Dis_StopHere;
      dres.jk_StopHere = Ijk_NoDecode;
   }
   return dres;
}

/*--------------------------------------------------------------------*/
/*--- end                                     guest_riscv64_toIR.c ---*/
/*--------------------------------------------------------------------*/
