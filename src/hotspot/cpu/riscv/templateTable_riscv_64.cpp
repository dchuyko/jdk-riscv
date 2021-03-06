/*
 * Copyright (c) 2014, 2018, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2013, 2017 SAP SE. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "gc/shared/barrierSetAssembler.hpp"
#include "interpreter/interpreter.hpp"
#include "interpreter/interpreterRuntime.hpp"
#include "interpreter/interp_masm.hpp"
#include "interpreter/templateInterpreter.hpp"
#include "interpreter/templateTable.hpp"
#include "memory/universe.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/oop.inline.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/safepointMechanism.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/synchronizer.hpp"
#include "utilities/macros.hpp"

#undef __
#define __ _masm->

// ============================================================================
// Misc helpers

// Do an oop store like *(base + index) = val OR *(base + offset) = val
// (only one of both variants is possible at the same time).
// Index can be noreg.
// Kills:
//   Rbase, Rtmp
static void do_oop_store(InterpreterMacroAssembler* _masm,
                         Register           base,
                         RegisterOrConstant offset,
                         Register           val,         // Noreg means always null.
                         Register           tmp1,
                         Register           tmp2,
                         Register           tmp3,
                         DecoratorSet       decorators) {
  assert_different_registers(tmp1, tmp2, tmp3, val, base);
  __ store_heap_oop(val, offset, base, tmp1, tmp2, tmp3, false, decorators);
}

static void do_oop_load(InterpreterMacroAssembler* _masm,
                        Register base,
                        RegisterOrConstant offset,
                        Register dst,
                        Register tmp1,
                        Register tmp2,
                        DecoratorSet decorators) {
  assert_different_registers(base, tmp1, tmp2);
  assert_different_registers(dst, tmp1, tmp2);
  __ load_heap_oop(dst, offset, base, tmp1, tmp2, false, decorators);
}

// ============================================================================
// Platform-dependent initialization

void TemplateTable::pd_initialize() {
  // No riscv64 specific initialization.
}

Address TemplateTable::at_bcp(int offset) {
  // Not used on riscv.
  ShouldNotReachHere();
  return Address();
}

// Patches the current bytecode (ptr to it located in bcp)
// in the bytecode stream with a new one.
void TemplateTable::patch_bytecode(Bytecodes::Code new_bc, Register Rnew_bc, Register Rtemp, bool load_bc_into_bc_reg /*=true*/, int byte_no) {
  // With sharing on, may need to test method flag.
  if (!RewriteBytecodes) return;
  Label L_patch_done, L_zero, L_after_switch;

  switch (new_bc) {
    case Bytecodes::_fast_aputfield:
    case Bytecodes::_fast_bputfield:
    case Bytecodes::_fast_zputfield:
    case Bytecodes::_fast_cputfield:
    case Bytecodes::_fast_dputfield:
    case Bytecodes::_fast_fputfield:
    case Bytecodes::_fast_iputfield:
    case Bytecodes::_fast_lputfield:
    case Bytecodes::_fast_sputfield:
    {
      // We skip bytecode quickening for putfield instructions when
      // the put_code written to the constant pool cache is zero.
      // This is required so that every execution of this instruction
      // calls out to InterpreterRuntime::resolve_get_put to do
      // additional, required work.
      assert(byte_no == f1_byte || byte_no == f2_byte, "byte_no out of range");
      assert(load_bc_into_bc_reg, "we use bc_reg as temp");
      __ get_cache_and_index_at_bcp(Rtemp /* dst = cache */, 1);
      // ((*(cache+indices))>>((1+byte_no)*8))&0xFF:
#if defined(VM_LITTLE_ENDIAN)
      __ lbu(Rnew_bc, Rtemp, in_bytes(ConstantPoolCache::base_offset() + ConstantPoolCacheEntry::indices_offset()) + 1 + byte_no);
#else
      __ lbu(Rnew_bc, Rtemp, in_bytes(ConstantPoolCache::base_offset() + ConstantPoolCacheEntry::indices_offset()) + 7 - (1 + byte_no));
#endif
      __ beqz(Rnew_bc, L_zero);
      __ li(Rnew_bc, (unsigned int)(unsigned char)new_bc);
      __ j(L_after_switch);

      __ bind(L_zero);
      __ li(Rnew_bc, (unsigned int)(unsigned char)new_bc);
      __ j(L_patch_done);
      break;
    }

    default:
      assert(byte_no == -1, "sanity");
      if (load_bc_into_bc_reg) {
        __ li(Rnew_bc, (unsigned int)(unsigned char)new_bc);
      }
  }

  __ bind(L_after_switch);

  if (JvmtiExport::can_post_breakpoint()) {
    Label L_fast_patch;
    __ lbz_PPC(Rtemp, 0, R22_bcp);
    __ cmpwi_PPC(CCR0, Rtemp, (unsigned int)(unsigned char)Bytecodes::_breakpoint);
    __ bne_PPC(CCR0, L_fast_patch);
    // Perform the quickening, slowly, in the bowels of the breakpoint table.
    __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::set_original_bytecode_at), R27_method, R22_bcp, Rnew_bc);
    __ b_PPC(L_patch_done);
    __ bind(L_fast_patch);
  }

  // Patch bytecode.
  __ sb(Rnew_bc, R22_bcp, 0);

  __ bind(L_patch_done);
}

// ============================================================================
// Individual instructions

void TemplateTable::nop() {
  transition(vtos, vtos);
  // Nothing to do.
}

void TemplateTable::shouldnotreachhere() {
  transition(vtos, vtos);
  __ stop("shouldnotreachhere bytecode");
}

void TemplateTable::aconst_null() {
  transition(vtos, atos);
  __ li(R25_tos, 0L);
}

void TemplateTable::iconst(int value) {
  transition(vtos, itos);
  assert(value >= -1 && value <= 5, "");
  __ li(R25_tos, value);
}

void TemplateTable::lconst(int value) {
  transition(vtos, ltos);
  assert(value >= -1 && value <= 5, "");
  __ li(R25_tos, value);
}

void TemplateTable::fconst(int value) {
  transition(vtos, ftos);
  static float zero = 0.0;
  static float one  = 1.0;
  static float two  = 2.0;
  switch (value) {
    default: ShouldNotReachHere();
    case 0: {
      int off = __ load_const_optimized(R5_scratch1, (address*)&zero, R6_scratch2, true);
      __ flw(F23_ftos, R5_scratch1, off);
      break;
    }
    case 1: {
      int off = __ load_const_optimized(R5_scratch1, (address*)&one, R6_scratch2, true);
      __ flw(F23_ftos, R5_scratch1, off);
      break;
    }
    case 2: {
      int off = __ load_const_optimized(R5_scratch1, (address*)&two, R6_scratch2, true);
      __ flw(F23_ftos, R5_scratch1, off);
      break;
    }
  }
}

void TemplateTable::dconst(int value) {
  transition(vtos, dtos);
  static double zero = 0.0;
  static double one  = 1.0;
  switch (value) {
    case 0: {
      int off = __ load_const_optimized(R5_scratch1, (address*)&zero, R6_scratch2, true);
      __ fld(F23_ftos, R5_scratch1, off);
      break;
    }
    case 1: {
      int off = __ load_const_optimized(R5_scratch1, (address*)&one, R6_scratch2, true);
      __ fld(F23_ftos, R5_scratch1, off);
      break;
    }
    default: ShouldNotReachHere();
  }
}

void TemplateTable::bipush() {
  transition(vtos, itos);
  __ lb(R25_tos, R22_bcp, 1);
}

void TemplateTable::sipush() {
  transition(vtos, itos);
  __ get_2_byte_integer_at_bcp(1, R25_tos, InterpreterMacroAssembler::Signed);
}

void TemplateTable::ldc(bool wide) {
  Register Rscratch1 = R5_scratch1,
           Rscratch2 = R6_scratch2,
           Rscratch3 = R7_TMP2,
           Rcpool    = R10_ARG0;

  transition(vtos, vtos);
  Label notInt, notFloat, isClass, exit;

  __ get_cpool_and_tags(Rcpool, Rscratch2); // Set Rscratch2 = &tags.
  if (wide) { // Read index.
    __ get_2_byte_integer_at_bcp(1, Rscratch1, InterpreterMacroAssembler::Unsigned);
  } else {
    __ lbu(Rscratch1, R22_bcp, 1);
  }

  const int base_offset = ConstantPool::header_size() * wordSize;
  const int tags_offset = Array<u1>::base_offset_in_bytes();

  // Get type from tags.
  __ addi(Rscratch2, Rscratch2, tags_offset);
  __ add(Rscratch2, Rscratch2, Rscratch1);
  __ lbu(Rscratch2, Rscratch2, 0);

  __ li(Rscratch3, JVM_CONSTANT_UnresolvedClass);
  __ beq(Rscratch3, Rscratch2, isClass);
  __ li(Rscratch3, JVM_CONSTANT_UnresolvedClassInError);
  __ beq(Rscratch3, Rscratch2, isClass);

  // Resolved class - need to call vm to get java mirror of the class.
  __ li(Rscratch3, JVM_CONSTANT_Class);
  __ beq(Rscratch3, Rscratch2, isClass);

  // Not a class
  __ addi(Rcpool, Rcpool, base_offset);
  __ slli(Rscratch1, Rscratch1, LogBytesPerWord);
  __ li(Rscratch3, JVM_CONSTANT_Integer);
  __ bne(Rscratch2, Rscratch3, notInt);

  // An integer
  __ add(Rscratch1, Rscratch1, Rcpool);
  __ lw(R25_tos, Rscratch1, 0);
  __ push(itos);
  __ j(exit);

  __ align(32, 12);
  __ bind(isClass);

  __ li(Rscratch1, wide ? 1 : 0);
  call_VM(R25_tos, CAST_FROM_FN_PTR(address, InterpreterRuntime::ldc), Rscratch1);
  __ push(atos);
  __ j(exit);

  __ align(32, 12);
  __ bind(notInt);
  __ li(Rscratch3, JVM_CONSTANT_Float);
  __ bne(Rscratch2, Rscratch3, notFloat);

  // A float
  __ add(Rscratch1, Rscratch1, Rcpool);
  __ flw(F23_ftos, Rscratch1, 0);
  __ push(ftos);
  __ j(exit);

  __ align(32, 12);
  // assume the tag is for condy; if not, the VM runtime will tell us
  __ bind(notFloat);
  condy_helper(exit);

  __ align(32, 12);
  __ bind(exit);
}

// Fast path for caching oop constants.
void TemplateTable::fast_aldc(bool wide) {
  transition(vtos, atos);

  int index_size = wide ? sizeof(u2) : sizeof(u1);
  const Register Rscratch = R5_scratch1;
  Label is_null;
  Label not_sentinel;

  // We are resolved if the resolved reference cache entry contains a
  // non-null object (CallSite, etc.)
  __ get_cache_index_at_bcp(Rscratch, 1, index_size);  // Load index.

  __ load_resolved_reference_at_index(R25_tos, Rscratch, &is_null);

  // Convert null sentinel to NULL.
  int simm12_rest = __ load_const_optimized(Rscratch, Universe::the_null_sentinel_addr(), noreg, true);
  __ ld(Rscratch, Rscratch, simm12_rest);
  __ bne(R25_tos, Rscratch, not_sentinel);
  __ li(R25_tos, 0L);
  __ bind(not_sentinel);

  __ verify_oop(R25_tos);
  __ dispatch_epilog(atos, Bytecodes::length_for(bytecode()));

  __ bind(is_null);
  __ load_const_optimized(R10_ARG0, (int)bytecode());

  address entry = CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_ldc);

  // First time invocation - must resolve first.
  __ call_VM(R25_tos, entry, R10_ARG0);
  __ verify_oop(R25_tos);
}

void TemplateTable::ldc2_w() {
  transition(vtos, vtos);
  Label not_double, not_long, exit;

  Register Rindex   = R5_scratch1,
           Rcpool   = R6_scratch2,
           Rscratch = R7_TMP2,
           Rtag     = R10_ARG0;
  __ get_cpool_and_tags(Rcpool, Rtag);
  __ get_2_byte_integer_at_bcp(1, Rindex, InterpreterMacroAssembler::Unsigned);

  const int base_offset = ConstantPool::header_size() * wordSize;
  const int tags_offset = Array<u1>::base_offset_in_bytes();
  // Get type from tags.
  __ addi(Rcpool, Rcpool, base_offset);
  __ addi(Rtag, Rtag, tags_offset);

  __ add(Rtag, Rtag, Rindex);
  __ lbu(Rtag, Rtag, 0);
  __ slli(Rindex, Rindex, LogBytesPerWord);

  __ li(Rscratch, JVM_CONSTANT_Double);
  __ bne(Rtag, Rscratch, not_double);
  __ add(Rindex, Rindex, Rcpool);
  __ fld(F23_ftos, Rindex, 0);
  __ push(dtos);
  __ j(exit);

  __ bind(not_double);
  __ li(Rscratch, JVM_CONSTANT_Long);
  __ bne(Rtag, Rscratch, not_long);
  __ add(Rindex, Rindex, Rcpool);
  __ ld(R25_tos, Rindex, 0);
  __ push(ltos);
  __ j(exit);

  __ bind(not_long);
  condy_helper(exit);

  __ align(32, 12);
  __ bind(exit);
}

void TemplateTable::condy_helper(Label& Done) {
  const Register obj   = R31;
  const Register off   = R5_scratch1;
  const Register flags = R6_scratch2;
  const Register rarg  = R4_ARG2_PPC;
  __ li_PPC(rarg, (int)bytecode());
  call_VM(obj, CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_ldc), rarg);
  __ get_vm_result_2(flags);

  // VMr = obj = base address to find primitive value to push
  // VMr2 = flags = (tos, off) using format of CPCE::_flags
  __ andi_PPC(off, flags, ConstantPoolCacheEntry::field_index_mask);

  // What sort of thing are we loading?
  __ rldicl_PPC(flags, flags, 64-ConstantPoolCacheEntry::tos_state_shift, 64-ConstantPoolCacheEntry::tos_state_bits);

  switch (bytecode()) {
  case Bytecodes::_ldc:
  case Bytecodes::_ldc_w:
    {
      // tos in (itos, ftos, stos, btos, ctos, ztos)
      Label notInt, notFloat, notShort, notByte, notChar, notBool;
      __ cmplwi_PPC(CCR0, flags, itos);
      __ bne_PPC(CCR0, notInt);
      // itos
      __ lwax_PPC(R25_tos, obj, off);
      __ push(itos);
      __ b_PPC(Done);

      __ bind(notInt);
      __ cmplwi_PPC(CCR0, flags, ftos);
      __ bne_PPC(CCR0, notFloat);
      // ftos
      __ lfsx_PPC(F23_ftos, obj, off);
      __ push(ftos);
      __ b_PPC(Done);

      __ bind(notFloat);
      __ cmplwi_PPC(CCR0, flags, stos);
      __ bne_PPC(CCR0, notShort);
      // stos
      __ lhax_PPC(R25_tos, obj, off);
      __ push(stos);
      __ b_PPC(Done);

      __ bind(notShort);
      __ cmplwi_PPC(CCR0, flags, btos);
      __ bne_PPC(CCR0, notByte);
      // btos
      __ lbzx_PPC(R25_tos, obj, off);
      __ extsb_PPC(R25_tos, R25_tos);
      __ push(btos);
      __ b_PPC(Done);

      __ bind(notByte);
      __ cmplwi_PPC(CCR0, flags, ctos);
      __ bne_PPC(CCR0, notChar);
      // ctos
      __ lhzx_PPC(R25_tos, obj, off);
      __ push(ctos);
      __ b_PPC(Done);

      __ bind(notChar);
      __ cmplwi_PPC(CCR0, flags, ztos);
      __ bne_PPC(CCR0, notBool);
      // ztos
      __ lbzx_PPC(R25_tos, obj, off);
      __ push(ztos);
      __ b_PPC(Done);

      __ bind(notBool);
      break;
    }

  case Bytecodes::_ldc2_w:
    {
      Label notLong, notDouble;
      __ cmplwi_PPC(CCR0, flags, ltos);
      __ bne_PPC(CCR0, notLong);
      // ltos
      __ ldx_PPC(R25_tos, obj, off);
      __ push(ltos);
      __ b_PPC(Done);

      __ bind(notLong);
      __ cmplwi_PPC(CCR0, flags, dtos);
      __ bne_PPC(CCR0, notDouble);
      // dtos
      __ lfdx_PPC(F23_ftos, obj, off);
      __ push(dtos);
      __ b_PPC(Done);

      __ bind(notDouble);
      break;
    }

  default:
    ShouldNotReachHere();
  }

  __ stop("bad ldc/condy");
}

// Get the locals index located in the bytecode stream at bcp + offset.
void TemplateTable::locals_index(Register Rdst, int offset) {
  __ lbu(Rdst, R22_bcp, offset);
}

void TemplateTable::iload() {
  iload_internal();
}

void TemplateTable::nofast_iload() {
  iload_internal(may_not_rewrite);
}

void TemplateTable::iload_internal(RewriteControl rc) {
  transition(vtos, itos);

  // Get the local value into tos
  const Register Rindex = R6_scratch2;
  locals_index(Rindex);

  // Rewrite iload,iload  pair into fast_iload2
  //         iload,caload pair into fast_icaload
  if (RewriteFrequentPairs && rc == may_rewrite) {
    Label Lrewrite, Ldone;
    Register Rnext_byte  = R10_ARG0,
             Rrewrite_to = R13_ARG3,
             Rscratch    = R5_scratch1;

    // get next byte
    __ lbu(Rnext_byte, R22_bcp, Bytecodes::length_for(Bytecodes::_iload));

    // if _iload, wait to rewrite to iload2. We only want to rewrite the
    // last two iloads in a pair. Comparing against fast_iload means that
    // the next bytecode is neither an iload or a caload, and therefore
    // an iload pair.
    __ li(Rscratch, (unsigned int)(unsigned char)Bytecodes::_iload);
    __ beq(Rnext_byte, Rscratch, Ldone);

    __ li(Rscratch, (unsigned int)(unsigned char)Bytecodes::_fast_iload);
    __ li(Rrewrite_to, (unsigned int)(unsigned char)Bytecodes::_fast_iload2);
    __ beq(Rnext_byte, Rscratch, Lrewrite);

    __ li(Rscratch, (unsigned int)(unsigned char)Bytecodes::_caload);
    __ li(Rrewrite_to, (unsigned int)(unsigned char)Bytecodes::_fast_icaload);
    __ beq(Rnext_byte, Rscratch, Lrewrite);

    __ li(Rrewrite_to, (unsigned int)(unsigned char)Bytecodes::_fast_iload);

    __ bind(Lrewrite);
    patch_bytecode(Bytecodes::_iload, Rrewrite_to, Rscratch, false);
    __ bind(Ldone);
  }

  __ load_local_int(R25_tos, Rindex, Rindex);
}

// Load 2 integers in a row without dispatching
void TemplateTable::fast_iload2() {
  transition(vtos, itos);

  __ lbu(R10_ARG0, R22_bcp, 1);
  __ lbu(R25_tos, R22_bcp, Bytecodes::length_for(Bytecodes::_iload) + 1);

  __ load_local_int(R10_ARG0, R5_scratch1, R10_ARG0);
  __ load_local_int(R25_tos, R6_scratch2, R25_tos);
  __ push_i(R10_ARG0);
}

void TemplateTable::fast_iload() {
  transition(vtos, itos);
  // Get the local value into tos

  const Register Rindex = R5_scratch1;
  locals_index(Rindex);
  __ load_local_int(R25_tos, Rindex, Rindex);
}

// Load a local variable type long from locals area to TOS cache register.
// Local index resides in bytecodestream.
void TemplateTable::lload() {
  transition(vtos, ltos);

  const Register Rindex = R5_scratch1;
  locals_index(Rindex);
  __ load_local_long(R25_tos, Rindex, Rindex);
}

void TemplateTable::fload() {
  transition(vtos, ftos);

  const Register Rindex = R5_scratch1;
  locals_index(Rindex);
  __ load_local_float(F23_ftos, Rindex, Rindex);
}

void TemplateTable::dload() {
  transition(vtos, dtos);

  const Register Rindex = R5_scratch1;
  locals_index(Rindex);
  __ load_local_double(F23_ftos, Rindex, Rindex);
}

void TemplateTable::aload() {
  transition(vtos, atos);

  const Register Rindex = R5_scratch1;
  locals_index(Rindex);
  __ load_local_ptr(R25_tos, Rindex, Rindex);
}

void TemplateTable::locals_index_wide(Register Rdst) {
  // Offset is 2, not 1, because Lbcp points to wide prefix code.
  __ get_2_byte_integer_at_bcp(2, Rdst, InterpreterMacroAssembler::Unsigned);
}

void TemplateTable::wide_iload() {
  // Get the local value into tos.

  const Register Rindex = R5_scratch1;
  locals_index_wide(Rindex);
  __ load_local_int(R25_tos, Rindex, Rindex);
}

void TemplateTable::wide_lload() {
  transition(vtos, ltos);

  const Register Rindex = R5_scratch1;
  locals_index_wide(Rindex);
  __ load_local_long(R25_tos, Rindex, Rindex);
}

void TemplateTable::wide_fload() {
  transition(vtos, ftos);

  const Register Rindex = R5_scratch1;
  locals_index_wide(Rindex);
  __ load_local_float(F23_ftos, Rindex, Rindex);
}

void TemplateTable::wide_dload() {
  transition(vtos, dtos);

  const Register Rindex = R5_scratch1;
  locals_index_wide(Rindex);
  __ load_local_double(F23_ftos, Rindex, Rindex);
}

void TemplateTable::wide_aload() {
  transition(vtos, atos);

  const Register Rindex = R5_scratch1;
  locals_index_wide(Rindex);
  __ load_local_ptr(R25_tos, Rindex, Rindex);
}

void TemplateTable::iaload() {
  transition(itos, itos);

  const Register Rload_addr = R10_ARG0,
                 Rarray     = R11_ARG1,
                 Rtemp      = R12_ARG2;
  __ index_check(Rarray, R25_tos /* index */, LogBytesPerInt, Rtemp, Rload_addr);
  __ lw(R25_tos, Rload_addr, arrayOopDesc::base_offset_in_bytes(T_INT));
}

void TemplateTable::laload() {
  transition(itos, ltos);

  const Register Rload_addr = R10_ARG0,
                 Rarray     = R11_ARG1,
                 Rtemp      = R12_ARG2;
  __ index_check(Rarray, R25_tos /* index */, LogBytesPerLong, Rtemp, Rload_addr);
  __ ld(R25_tos, Rload_addr, arrayOopDesc::base_offset_in_bytes(T_LONG));
}

void TemplateTable::faload() {
  transition(itos, ftos);

  const Register Rload_addr = R10_ARG0,
                 Rarray     = R11_ARG1,
                 Rtemp      = R12_ARG2;
  __ index_check(Rarray, R25_tos /* index */, LogBytesPerInt, Rtemp, Rload_addr);
  __ flw(F23_ftos, Rload_addr, arrayOopDesc::base_offset_in_bytes(T_FLOAT));
}

void TemplateTable::daload() {
  transition(itos, dtos);

  const Register Rload_addr = R10_ARG0,
                 Rarray     = R11_ARG1,
                 Rtemp      = R12_ARG2;
  __ index_check(Rarray, R25_tos /* index */, LogBytesPerLong, Rtemp, Rload_addr);
  __ fld(F23_ftos, Rload_addr, arrayOopDesc::base_offset_in_bytes(T_DOUBLE));
}

void TemplateTable::aaload() {
  transition(itos, atos);

  // tos: index
  // result tos: array
  const Register Rload_addr = R10_ARG0,
                 Rarray     = R11_ARG1,
                 Rtemp      = R12_ARG2,
                 Rtemp2     = R31_TMP6;
  __ index_check(Rarray, R25_tos /* index */, UseCompressedOops ? 2 : LogBytesPerWord, Rtemp, Rload_addr);
  do_oop_load(_masm, Rload_addr, arrayOopDesc::base_offset_in_bytes(T_OBJECT), R25_tos, Rtemp, Rtemp2, IS_ARRAY);
  __ verify_oop(R25_tos);
}

void TemplateTable::baload() {
  transition(itos, itos);

  const Register Rload_addr = R10_ARG0,
                 Rarray     = R11_ARG1,
                 Rtemp      = R12_ARG2;
  __ index_check(Rarray, R25_tos /* index */, 0, Rtemp, Rload_addr);
  __ lb(R25_tos, Rload_addr, arrayOopDesc::base_offset_in_bytes(T_BYTE));
}

void TemplateTable::caload() {
  transition(itos, itos);

  const Register Rload_addr = R10_ARG0,
                 Rarray     = R11_ARG1,
                 Rtemp      = R12_ARG2;
  __ index_check(Rarray, R25_tos /* index */, LogBytesPerShort, Rtemp, Rload_addr);
  __ lhu(R25_tos, Rload_addr, arrayOopDesc::base_offset_in_bytes(T_CHAR));
}

// Iload followed by caload frequent pair.
void TemplateTable::fast_icaload() {
  transition(vtos, itos);

  const Register Rload_addr = R10_ARG0,
                 Rarray     = R11_ARG1,
                 Rtemp      = R12_ARG2;

  locals_index(R25_tos);
  __ load_local_int(R25_tos, Rtemp, R25_tos);
  __ index_check(Rarray, R25_tos /* index */, LogBytesPerShort, Rtemp, Rload_addr);
  __ lhu(R25_tos, Rload_addr, arrayOopDesc::base_offset_in_bytes(T_CHAR));
}

void TemplateTable::saload() {
  transition(itos, itos);

  const Register Rload_addr = R10_ARG0,
                 Rarray     = R11_ARG1,
                 Rtemp      = R12_ARG2;
  __ index_check(Rarray, R25_tos /* index */, LogBytesPerShort, Rtemp, Rload_addr);
  __ lh(R25_tos, Rload_addr, arrayOopDesc::base_offset_in_bytes(T_SHORT));
}

void TemplateTable::iload(int n) {
  transition(vtos, itos);
  __ lw(R25_tos, R26_locals, Interpreter::local_offset_in_bytes(n));
}

void TemplateTable::lload(int n) {
  transition(vtos, ltos);
  __ ld(R25_tos, R26_locals, Interpreter::local_offset_in_bytes(n + 1));
}

void TemplateTable::fload(int n) {
  transition(vtos, ftos);
  __ flw(F23_ftos, R26_locals, Interpreter::local_offset_in_bytes(n));
}

void TemplateTable::dload(int n) {
  transition(vtos, dtos);
  __ fld(F23_ftos, R26_locals, Interpreter::local_offset_in_bytes(n + 1));
}

void TemplateTable::aload(int n) {
  transition(vtos, atos);
  __ ld(R25_tos, R26_locals, Interpreter::local_offset_in_bytes(n));
}

void TemplateTable::aload_0() {
  //aload_0_internal(); FIXME_RISCV
  aload(0);
}

void TemplateTable::nofast_aload_0() {
  aload_0_internal(may_not_rewrite);
}

void TemplateTable::aload_0_internal(RewriteControl rc) {
  transition(vtos, atos);
  // According to bytecode histograms, the pairs:
  //
  // _aload_0, _fast_igetfield
  // _aload_0, _fast_agetfield
  // _aload_0, _fast_fgetfield
  //
  // occur frequently. If RewriteFrequentPairs is set, the (slow)
  // _aload_0 bytecode checks if the next bytecode is either
  // _fast_igetfield, _fast_agetfield or _fast_fgetfield and then
  // rewrites the current bytecode into a pair bytecode; otherwise it
  // rewrites the current bytecode into _0 that doesn't do
  // the pair check anymore.
  //
  // Note: If the next bytecode is _getfield, the rewrite must be
  //       delayed, otherwise we may miss an opportunity for a pair.
  //
  // Also rewrite frequent pairs
  //   aload_0, aload_1
  //   aload_0, iload_1
  // These bytecodes with a small amount of code are most profitable
  // to rewrite.

  if (RewriteFrequentPairs && rc == may_rewrite) {

    Label Lrewrite, Ldont_rewrite;
    Register Rnext_byte  = R10_ARG0,
             Rrewrite_to = R13_ARG3,
             Rscratch    = R5_scratch1;

    // Get next byte.
    __ lbu(Rnext_byte, R22_bcp, Bytecodes::length_for(Bytecodes::_aload_0));

    // If _getfield, wait to rewrite. We only want to rewrite the last two bytecodes in a pair.
    __ li(Rscratch, (unsigned int)(unsigned char)Bytecodes::_getfield);
    __ beq(Rnext_byte, Rscratch, Ldont_rewrite);

    __ li(Rscratch, (unsigned int)(unsigned char)Bytecodes::_fast_igetfield);
    __ li(Rrewrite_to, (unsigned int)(unsigned char)Bytecodes::_fast_iaccess_0);
    __ beq(Rnext_byte, Rscratch, Lrewrite);

    __ li(Rscratch, (unsigned int)(unsigned char)Bytecodes::_fast_agetfield);
    __ li(Rrewrite_to, (unsigned int)(unsigned char)Bytecodes::_fast_aaccess_0);
    __ beq(Rnext_byte, Rscratch, Lrewrite);

    __ li(Rscratch, (unsigned int)(unsigned char)Bytecodes::_fast_fgetfield);
    __ li(Rrewrite_to, (unsigned int)(unsigned char)Bytecodes::_fast_faccess_0);
    __ beq(Rnext_byte, Rscratch, Lrewrite);

    __ li(Rrewrite_to, (unsigned int)(unsigned char)Bytecodes::_fast_aload_0);

    __ bind(Lrewrite);
    patch_bytecode(Bytecodes::_aload_0, Rrewrite_to, Rscratch, false);
    __ bind(Ldont_rewrite);
  }

  // Do actual aload_0 (must do this after patch_bytecode which might call VM and GC might change oop).
  aload(0);
}

void TemplateTable::istore() {
  transition(itos, vtos);

  const Register Rindex = R5_scratch1;
  locals_index(Rindex);
  __ store_local_int(R25_tos, Rindex);
}

void TemplateTable::lstore() {
  transition(ltos, vtos);
  const Register Rindex = R5_scratch1;
  locals_index(Rindex);
  __ store_local_long(R25_tos, Rindex);
}

void TemplateTable::fstore() {
  transition(ftos, vtos);

  const Register Rindex = R5_scratch1;
  locals_index(Rindex);
  __ store_local_float(F23_ftos, Rindex);
}

void TemplateTable::dstore() {
  transition(dtos, vtos);

  const Register Rindex = R5_scratch1;
  locals_index(Rindex);
  __ store_local_double(F23_ftos, Rindex);
}

void TemplateTable::astore() {
  transition(vtos, vtos);

  const Register Rindex = R5_scratch1;
  __ pop_ptr();
  __ verify_oop_or_return_address(R25_tos, Rindex);
  locals_index(Rindex);
  __ store_local_ptr(R25_tos, Rindex);
}

void TemplateTable::wide_istore() {
  transition(vtos, vtos);

  const Register Rindex = R5_scratch1;
  __ pop_i();
  locals_index_wide(Rindex);
  __ store_local_int(R25_tos, Rindex);
}

void TemplateTable::wide_lstore() {
  transition(vtos, vtos);

  const Register Rindex = R5_scratch1;
  __ pop_l();
  locals_index_wide(Rindex);
  __ store_local_long(R25_tos, Rindex);
}

void TemplateTable::wide_fstore() {
  transition(vtos, vtos);

  const Register Rindex = R5_scratch1;
  __ pop_f();
  locals_index_wide(Rindex);
  __ store_local_float(F23_ftos, Rindex);
}

void TemplateTable::wide_dstore() {
  transition(vtos, vtos);

  const Register Rindex = R5_scratch1;
  __ pop_d();
  locals_index_wide(Rindex);
  __ store_local_double(F23_ftos, Rindex);
}

void TemplateTable::wide_astore() {
  transition(vtos, vtos);

  const Register Rindex = R5_scratch1;
  __ pop_ptr();
  __ verify_oop_or_return_address(R25_tos, Rindex);
  locals_index_wide(Rindex);
  __ store_local_ptr(R25_tos, Rindex);
}

void TemplateTable::iastore() {
  transition(itos, vtos);

  const Register Rindex      = R10_ARG0,
                 Rarray      = R11_ARG1,
                 Rstore_addr = R12_ARG2,
                 Rtemp       = R13_ARG3;
  __ pop_i(Rindex);
  __ index_check(Rarray, Rindex, LogBytesPerInt, Rtemp, Rstore_addr);
  __ sw(R25_tos, Rstore_addr, arrayOopDesc::base_offset_in_bytes(T_INT));
}

void TemplateTable::lastore() {
  transition(ltos, vtos);

  const Register Rindex      = R10_ARG0,
                 Rarray      = R11_ARG1,
                 Rstore_addr = R12_ARG2,
                 Rtemp       = R13_ARG3;
  __ pop_i(Rindex);
  __ index_check(Rarray, Rindex, LogBytesPerLong, Rtemp, Rstore_addr);
  __ sd(R25_tos, Rstore_addr, arrayOopDesc::base_offset_in_bytes(T_LONG));
}

void TemplateTable::fastore() {
  transition(ftos, vtos);

  const Register Rindex      = R10_ARG0,
                 Rarray      = R11_ARG1,
                 Rstore_addr = R12_ARG2,
                 Rtemp       = R13_ARG3;
  __ pop_i(Rindex);
  __ index_check(Rarray, Rindex, LogBytesPerInt, Rtemp, Rstore_addr);
  __ fsw(F23_ftos, Rstore_addr, arrayOopDesc::base_offset_in_bytes(T_FLOAT));
  }

void TemplateTable::dastore() {
  transition(dtos, vtos);

  const Register Rindex      = R10_ARG0,
                 Rarray      = R11_ARG1,
                 Rstore_addr = R12_ARG2,
                 Rtemp       = R13_ARG3;
  __ pop_i(Rindex);
  __ index_check(Rarray, Rindex, LogBytesPerLong, Rtemp, Rstore_addr);
  __ fsd(F23_ftos, Rstore_addr, arrayOopDesc::base_offset_in_bytes(T_DOUBLE));
}

// Pop 3 values from the stack and...
void TemplateTable::aastore() {
  transition(vtos, vtos);

  Label Lstore_ok, Lis_null, Ldone;
  const Register Rindex    = R10_ARG0,
                 Rarray    = R11_ARG1,
                 Rscratch  = R5_scratch1,
                 Rscratch2 = R6_scratch2,
                 Rarray_klass = R12_ARG2,
                 Rarray_element_klass = Rarray_klass,
                 Rvalue_klass = R13_ARG3,
                 Rstore_addr = R14_ARG4;    // Use register which survives VM call.

  __ ld(R25_tos, R23_esp, Interpreter::expr_offset_in_bytes(0)); // Get value to store.
  __ lwu(Rindex, R23_esp, Interpreter::expr_offset_in_bytes(1)); // Get index.
  __ ld(Rarray, R23_esp, Interpreter::expr_offset_in_bytes(2));  // Get array.

  __ verify_oop(R25_tos);
  __ index_check_without_pop(Rarray, Rindex, UseCompressedOops ? 2 : LogBytesPerWord, Rscratch, Rstore_addr);
  // Rindex is dead!
  Register Rscratch3 = Rindex;

  // Do array store check - check for NULL value first.
  __ beqz(R25_tos, Lis_null);

  __ load_klass(Rarray_klass, Rarray);
  __ load_klass(Rvalue_klass, R25_tos);

  // Do fast instanceof cache test.
  __ ld(Rarray_element_klass, Rarray_klass, in_bytes(ObjArrayKlass::element_klass_offset()));

  // Generate a fast subtype check. Branch to store_ok if no failure. Throw if failure.
  // FIXME_RISCV this call falls in different registers assert
  __ j(Lstore_ok); // __ gen_subtype_check(Rvalue_klass /*subklass*/, Rarray_element_klass /*superklass*/, Rscratch, Rscratch2, Rscratch3, Lstore_ok);

  // Fell through: subtype check failed => throw an exception.
  __ load_dispatch_table(R5_scratch1, (address*)Interpreter::_throw_ArrayStoreException_entry);
  __ jr(R5_scratch1);

  __ bind(Lis_null);
  do_oop_store(_masm, Rstore_addr, arrayOopDesc::base_offset_in_bytes(T_OBJECT), noreg /* 0 */,
               Rscratch, Rscratch2, Rscratch3, IS_ARRAY);
  __ profile_null_seen(Rscratch, Rscratch2);
  __ j(Ldone);

  // Store is OK.
  __ bind(Lstore_ok);
  do_oop_store(_masm, Rstore_addr, arrayOopDesc::base_offset_in_bytes(T_OBJECT), R25_tos /* value */,
               Rscratch, Rscratch2, Rscratch3, IS_ARRAY | IS_NOT_NULL);

  __ bind(Ldone);
  // Adjust sp (pops array, index and value).
  __ addi(R23_esp, R23_esp, 3 * Interpreter::stackElementSize);
}

void TemplateTable::bastore() {
  transition(itos, vtos);

  const Register Rindex   = R5_scratch1,
                 Rarray   = R11_ARG1,
                 Rscratch = R10_ARG0;
  Label L_skip;

  __ pop_i(Rindex);
  __ pop_ptr(Rarray);
  // tos: val

  // Need to check whether array is boolean or byte
  // since both types share the bastore bytecode.
  __ load_klass(Rscratch, Rarray);
  __ lwu(Rscratch, Rscratch, in_bytes(Klass::layout_helper_offset()));
  int diffbit = exact_log2(Klass::layout_helper_boolean_diffbit());
  __ srli(Rscratch, Rscratch, diffbit);
  __ andi(Rscratch, Rscratch, 1);
  __ beqz(Rscratch, L_skip);

  __ andi(R25_tos, R25_tos, 1);  // if it is a T_BOOLEAN array, mask the stored value to 0/1
  __ bind(L_skip);

  __ index_check_without_pop(Rarray, Rindex, 0, Rscratch, Rarray);
  __ sb(R25_tos, Rarray, arrayOopDesc::base_offset_in_bytes(T_BYTE));
}

void TemplateTable::castore() {
  transition(itos, vtos);

  const Register Rindex   = R5_scratch1,
                 Rarray   = R11_ARG1,
                 Rscratch = R10_ARG0;
  __ pop_i(Rindex);
  // tos: val
  // Rarray: array ptr (popped by index_check)
  __ index_check(Rarray, Rindex, LogBytesPerShort, Rscratch, Rarray);
  __ sh(R25_tos, Rarray, arrayOopDesc::base_offset_in_bytes(T_CHAR));
}

void TemplateTable::sastore() {
  castore();
}

void TemplateTable::istore(int n) {
  transition(itos, vtos);
  __ sw(R25_tos, R26_locals, Interpreter::local_offset_in_bytes(n));
}

void TemplateTable::lstore(int n) {
  transition(ltos, vtos);
  __ sd(R25_tos, R26_locals, Interpreter::local_offset_in_bytes(n + 1));
}

void TemplateTable::fstore(int n) {
  transition(ftos, vtos);
  __ fsw(F23_ftos, R26_locals, Interpreter::local_offset_in_bytes(n));
}

void TemplateTable::dstore(int n) {
  transition(dtos, vtos);
  __ fsd(F23_ftos, R26_locals, Interpreter::local_offset_in_bytes(n + 1));
}

void TemplateTable::astore(int n) {
  transition(vtos, vtos);

  __ pop_ptr();
  __ verify_oop_or_return_address(R25_tos, R5_scratch1);
  __ sd(R25_tos, R26_locals, Interpreter::local_offset_in_bytes(n));
}

void TemplateTable::pop() {
  transition(vtos, vtos);

  __ addi(R23_esp, R23_esp, Interpreter::stackElementSize);
}

void TemplateTable::pop2() {
  transition(vtos, vtos);

  __ addi(R23_esp, R23_esp, Interpreter::stackElementSize * 2);
}

void TemplateTable::dup() {
  transition(vtos, vtos);
  __ ld(R5_scratch1, R23_esp, Interpreter::stackElementSize);
  __ push_ptr(R5_scratch1);
}

void TemplateTable::dup_x1() {
  transition(vtos, vtos);

  Register Ra = R5_scratch1,
           Rb = R6_scratch2;
  // stack: ..., a, b
  __ ld(Rb, R23_esp, Interpreter::stackElementSize);
  __ ld(Ra, R23_esp, Interpreter::stackElementSize * 2);
  __ sd(Rb, R23_esp, Interpreter::stackElementSize * 2);
  __ sd(Ra, R23_esp, Interpreter::stackElementSize);
  __ push_ptr(Rb);
  // stack: ..., b, a, b
}

void TemplateTable::dup_x2() {
  transition(vtos, vtos);

  Register Ra = R5_scratch1,
           Rb = R6_scratch2,
           Rc = R10_ARG0;

  // stack: ..., a, b, c
  __ ld(Rc, R23_esp, Interpreter::stackElementSize);  // load c
  __ ld(Ra, R23_esp, Interpreter::stackElementSize * 3);  // load a
  __ sd(Rc, R23_esp, Interpreter::stackElementSize * 3); // store c in a
  __ ld(Rb, R23_esp, Interpreter::stackElementSize * 2);  // load b
  // stack: ..., c, b, c
  __ sd(Ra, R23_esp, Interpreter::stackElementSize * 2); // store a in b
  // stack: ..., c, a, c
  __ sd(Rb, R23_esp, Interpreter::stackElementSize); // store b in c
  __ push_ptr(Rc);                                        // push c
  // stack: ..., c, a, b, c
}

void TemplateTable::dup2() {
  transition(vtos, vtos);

  Register Ra = R5_scratch1,
           Rb = R6_scratch2;
  // stack: ..., a, b
  __ ld(Rb, R23_esp, Interpreter::stackElementSize);
  __ ld(Ra, R23_esp, Interpreter::stackElementSize * 2);
  __ push_2ptrs(Ra, Rb);
  // stack: ..., a, b, a, b
}

void TemplateTable::dup2_x1() {
  transition(vtos, vtos);

  Register Ra = R5_scratch1,
           Rb = R6_scratch2,
           Rc = R10_ARG0;
  // stack: ..., a, b, c
  __ ld(Rc, R23_esp, Interpreter::stackElementSize);
  __ ld(Rb, R23_esp, Interpreter::stackElementSize * 2);
  __ sd(Rc, R23_esp, Interpreter::stackElementSize * 2);
  __ ld(Ra, R23_esp, Interpreter::stackElementSize * 3);
  __ sd(Ra, R23_esp, Interpreter::stackElementSize);
  __ sd(Rb, R23_esp, Interpreter::stackElementSize * 3);
  // stack: ..., b, c, a
  __ push_2ptrs(Rb, Rc);
  // stack: ..., b, c, a, b, c
}

void TemplateTable::dup2_x2() {
  transition(vtos, vtos);

  Register Ra = R5_scratch1,
           Rb = R6_scratch2,
           Rc = R10_ARG0,
           Rd = R11_ARG1;
  // stack: ..., a, b, c, d
  __ ld(Rb, R23_esp, Interpreter::stackElementSize * 3);
  __ ld(Rd, R23_esp, Interpreter::stackElementSize);
  __ sd(Rb, R23_esp, Interpreter::stackElementSize);  // store b in d
  __ sd(Rd, R23_esp, Interpreter::stackElementSize * 3);  // store d in b
  __ ld(Ra, R23_esp, Interpreter::stackElementSize * 4);
  __ ld(Rc, R23_esp, Interpreter::stackElementSize * 2);
  __ sd(Ra, R23_esp, Interpreter::stackElementSize * 2);  // store a in c
  __ sd(Rc, R23_esp, Interpreter::stackElementSize * 4);  // store c in a
  // stack: ..., c, d, a, b
  __ push_2ptrs(Rc, Rd);
  // stack: ..., c, d, a, b, c, d
}

void TemplateTable::swap() {
  transition(vtos, vtos);
  // stack: ..., a, b

  Register Ra = R5_scratch1,
           Rb = R6_scratch2;
  // stack: ..., a, b
  __ ld(Rb, R23_esp, Interpreter::stackElementSize);
  __ ld(Ra, R23_esp, Interpreter::stackElementSize * 2);
  __ sd(Rb, R23_esp, Interpreter::stackElementSize * 2);
  __ sd(Ra, R23_esp, Interpreter::stackElementSize);
  // stack: ..., b, a
}

void TemplateTable::iop2(Operation op) {
  transition(itos, itos);

  Register Rscratch = R5_scratch1;

  __ pop_i(Rscratch);
  // tos  = number of bits to shift
  // Rscratch = value to shift
  switch (op) {
    case  add:   __ addw(R25_tos, Rscratch, R25_tos); break;
    case  sub:   __ subw(R25_tos, Rscratch, R25_tos); break;
    case  mul:   __ mulw(R25_tos, Rscratch, R25_tos); break;
    case  _and:  __ andr(R25_tos, Rscratch, R25_tos); break;
    case  _or:   __ orr(R25_tos, Rscratch, R25_tos); break;
    case  _xor:  __ xorr(R25_tos, Rscratch, R25_tos); break;
    case  shl:   __ sllw(R25_tos, Rscratch, R25_tos); break;
    case  shr:   __ sraw(R25_tos, Rscratch, R25_tos); break;
    case  ushr:  __ srlw(R25_tos, Rscratch, R25_tos); break;
    default:     ShouldNotReachHere();
  }
}

void TemplateTable::lop2(Operation op) {
  transition(ltos, ltos);

  Register Rscratch = R5_scratch1;
  __ pop_l(Rscratch);
  switch (op) {
    case  add:   __ add(R25_tos, Rscratch, R25_tos); break;
    case  sub:   __ sub(R25_tos, Rscratch, R25_tos); break;
    case  _and:  __ andr(R25_tos, Rscratch, R25_tos); break;
    case  _or:   __ orr(R25_tos, Rscratch, R25_tos); break;
    case  _xor:  __ xorr(R25_tos, Rscratch, R25_tos); break;
    default:     ShouldNotReachHere();
  }
}

void TemplateTable::idiv() {
  transition(itos, itos);

  Label Lnormal, Lexception, Ldone;
  Register Rdividend = R5_scratch1; // Used by irem.

  __ addi(R7_TMP2, R25_tos, 1);
  __ addi(R28_TMP3, R0_ZERO, 2);
  __ bgeu(R7_TMP2, R28_TMP3, Lnormal); // divisor <-1 or >1

  __ beqz(R25_tos, Lexception); // divisor == 0

  __ pop_i(Rdividend);
  __ mulw(R25_tos, Rdividend, R25_tos); // div by +/-1
  __ j(Ldone);

  __ bind(Lexception);
  __ load_dispatch_table(R7_TMP2, (address*)Interpreter::_throw_ArithmeticException_entry);
  __ jr(R7_TMP2);

  __ align(32, 12);
  __ bind(Lnormal);
  __ pop_i(Rdividend);
  __ divw(R25_tos, Rdividend, R25_tos); // Can't divide minint/-1.
  __ bind(Ldone);
}

void TemplateTable::irem() {
  transition(itos, itos);

  __ mv(R6_scratch2, R25_tos);
  idiv();
  __ mulw(R25_tos, R25_tos, R6_scratch2);
  __ subw(R25_tos, R5_scratch1, R25_tos); // Dividend (R5_scratch1) set by idiv.
}

void TemplateTable::lmul() {
  transition(ltos, ltos);

  __ pop_l(R5_scratch1);
  __ mul(R25_tos, R5_scratch1, R25_tos);
}

void TemplateTable::ldiv() {
  transition(ltos, ltos);

  Label Lnormal, Lexception, Ldone;
  Register Rdividend = R5_scratch1; // Used by lrem.

  __ addi(R7_TMP2, R25_tos, 1);
  __ addi(R28_TMP3, R0_ZERO, 2);
  __ bgeu(R7_TMP2, R28_TMP3, Lnormal); // divisor <-1 or >1

  __ beqz(R25_tos, Lexception); // divisor == 0

  __ pop_l(Rdividend);
  __ mul(R25_tos, Rdividend, R25_tos); // div by +/-1
  __ j(Ldone);

  __ bind(Lexception);
  __ load_dispatch_table(R7_TMP2, (address*)Interpreter::_throw_ArithmeticException_entry);
  __ jr(R7_TMP2);

  __ align(32, 12);
  __ bind(Lnormal);
  __ pop_l(Rdividend);
  __ div(R25_tos, Rdividend, R25_tos); // Can't divide minint/-1.
  __ bind(Ldone);
}

void TemplateTable::lrem() {
  transition(ltos, ltos);

  __ mv(R6_scratch2, R25_tos);
  ldiv();
  __ mul(R25_tos, R25_tos, R6_scratch2);
  __ sub(R25_tos, R5_scratch1, R25_tos); // Dividend (R5_scratch1) set by idiv.
}

void TemplateTable::lshl() {
  transition(itos, ltos);

  __ pop_l(R5_scratch1);
  __ sll(R25_tos, R5_scratch1, R25_tos);
}

void TemplateTable::lshr() {
  transition(itos, ltos);

  __ pop_l(R5_scratch1);
  __ sra(R25_tos, R5_scratch1, R25_tos);
}

void TemplateTable::lushr() {
  transition(itos, ltos);

  __ pop_l(R5_scratch1);
  __ srl(R25_tos, R5_scratch1, R25_tos);
}

void TemplateTable::fop2(Operation op) {
  transition(ftos, ftos);

  switch (op) {
    case add: __ pop_f(F0_TMP0); __ fadds(F23_ftos, F0_TMP0, F23_ftos, Assembler::RNE); break;
    case sub: __ pop_f(F0_TMP0); __ fsubs(F23_ftos, F0_TMP0, F23_ftos, Assembler::RNE); break;
    case mul: __ pop_f(F0_TMP0); __ fmuls(F23_ftos, F0_TMP0, F23_ftos, Assembler::RNE); break;
    case div: __ pop_f(F0_TMP0); __ fdivs(F23_ftos, F0_TMP0, F23_ftos, Assembler::RNE); break;
    case rem:
      __ pop_f(F10_ARG0);
      __ fmvs(F11_ARG1, F23_ftos);
      __ call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::frem));
      __ fmvs(F23_ftos, F10_RET);
      break;

    default: ShouldNotReachHere();
  }
}

void TemplateTable::dop2(Operation op) {
  transition(dtos, dtos);

  switch (op) {
    case add: __ pop_d(F0_TMP0); __ faddd(F23_ftos, F0_TMP0, F23_ftos, Assembler::RNE); break;
    case sub: __ pop_d(F0_TMP0); __ fsubd(F23_ftos, F0_TMP0, F23_ftos, Assembler::RNE); break;
    case mul: __ pop_d(F0_TMP0); __ fmuld(F23_ftos, F0_TMP0, F23_ftos, Assembler::RNE); break;
    case div: __ pop_d(F0_TMP0); __ fdivd(F23_ftos, F0_TMP0, F23_ftos, Assembler::RNE); break;
    case rem:
      __ pop_d(F10_ARG0);
      __ fmvs(F11_ARG1, F23_ftos);
      __ call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::drem));
      __ fmvs(F23_ftos, F10_RET);
      break;

    default: ShouldNotReachHere();
  }
}

// Negate the value in the TOS cache.
void TemplateTable::ineg() {
  transition(itos, itos);

  __ negw(R25_tos, R25_tos);
}

// Negate the value in the TOS cache.
void TemplateTable::lneg() {
  transition(ltos, ltos);

  __ neg(R25_tos, R25_tos);
}

void TemplateTable::fneg() {
  transition(ftos, ftos);

  __ fnegs(F23_ftos, F23_ftos);
}

void TemplateTable::dneg() {
  transition(dtos, dtos);

  __ fnegd(F23_ftos, F23_ftos);
}

// Increments a local variable in place.
void TemplateTable::iinc() {
  transition(vtos, vtos);

  const Register Rindex     = R5_scratch1,
                 Rincrement = R7_TMP2,
                 Rvalue     = R6_scratch2;

  locals_index(Rindex);             // Load locals index from bytecode stream.
  __ lb(Rincrement, R22_bcp, 2);    // Load increment from the bytecode stream.

  __ load_local_int(Rvalue, Rindex, Rindex); // Puts address of local into Rindex.

  __ addw(Rvalue, Rincrement, Rvalue);
  __ sw(Rvalue, Rindex, 0);
}

void TemplateTable::wide_iinc() {
  transition(vtos, vtos);

  const Register Rindex       = R5_scratch1,
           Rlocals_addr = Rindex,
           Rincr        = R6_scratch2;
  locals_index_wide(Rindex);
  __ get_2_byte_integer_at_bcp(4, Rincr, InterpreterMacroAssembler::Signed);
  __ load_local_int(R25_tos, Rlocals_addr, Rindex);
  __ addw(R25_tos, Rincr, R25_tos);
  __ sw(R25_tos, Rlocals_addr, 0);
}

void TemplateTable::convert() {
  // %%%%% Factor this first part accross platforms
#ifdef ASSERT
  TosState tos_in  = ilgl;
  TosState tos_out = ilgl;
  switch (bytecode()) {
    case Bytecodes::_i2l: // fall through
    case Bytecodes::_i2f: // fall through
    case Bytecodes::_i2d: // fall through
    case Bytecodes::_i2b: // fall through
    case Bytecodes::_i2c: // fall through
    case Bytecodes::_i2s: tos_in = itos; break;
    case Bytecodes::_l2i: // fall through
    case Bytecodes::_l2f: // fall through
    case Bytecodes::_l2d: tos_in = ltos; break;
    case Bytecodes::_f2i: // fall through
    case Bytecodes::_f2l: // fall through
    case Bytecodes::_f2d: tos_in = ftos; break;
    case Bytecodes::_d2i: // fall through
    case Bytecodes::_d2l: // fall through
    case Bytecodes::_d2f: tos_in = dtos; break;
    default             : ShouldNotReachHere();
  }
  switch (bytecode()) {
    case Bytecodes::_l2i: // fall through
    case Bytecodes::_f2i: // fall through
    case Bytecodes::_d2i: // fall through
    case Bytecodes::_i2b: // fall through
    case Bytecodes::_i2c: // fall through
    case Bytecodes::_i2s: tos_out = itos; break;
    case Bytecodes::_i2l: // fall through
    case Bytecodes::_f2l: // fall through
    case Bytecodes::_d2l: tos_out = ltos; break;
    case Bytecodes::_i2f: // fall through
    case Bytecodes::_l2f: // fall through
    case Bytecodes::_d2f: tos_out = ftos; break;
    case Bytecodes::_i2d: // fall through
    case Bytecodes::_l2d: // fall through
    case Bytecodes::_f2d: tos_out = dtos; break;
    default             : ShouldNotReachHere();
  }
  transition(tos_in, tos_out);
#endif

  // Conversion
  Label done;
  switch (bytecode()) {
    case Bytecodes::_i2l:
      // Nothing to do
      break;

    case Bytecodes::_l2i:
      __ addiw(R25_tos, R25_tos, 0);
      break;

    case Bytecodes::_i2b:
      __ slli(R25_tos, R25_tos, 56);
      __ srai(R25_tos, R25_tos, 56);
      break;

    case Bytecodes::_i2c:
      __ slli(R25_tos, R25_tos, 48);
      __ srli(R25_tos, R25_tos, 48);
      break;

    case Bytecodes::_i2s:
      __ slli(R25_tos, R25_tos, 48);
      __ srai(R25_tos, R25_tos, 48);
      break;

    case Bytecodes::_i2d:
      __ fcvtdw(F23_ftos, R25_tos, Assembler::RNE);
      break;

    case Bytecodes::_l2d:
      __ fcvtdl(F23_ftos, R25_tos, Assembler::RNE);
      break;

    case Bytecodes::_i2f:
      __ fcvtsw(F23_ftos, R25_tos, Assembler::RNE);
      break;

    case Bytecodes::_l2f:
      __ fcvtsl(F23_ftos, R25_tos, Assembler::RNE);
      break;

    case Bytecodes::_f2d:
      __ fcvtds(F23_ftos, F23_ftos, Assembler::RNE);
      break;

    case Bytecodes::_d2f:
      __ fcvtsd(F23_ftos, F23_ftos, Assembler::RNE);
      break;

    case Bytecodes::_f2i:
    case Bytecodes::_d2l:
    case Bytecodes::_f2l:
    case Bytecodes::_d2i: {
      // RISC-V does the wrong thing with NaN (convert to INT_MAX)
      // Java specification demands that NaN be converted to 0
      Label Lnan;
      if (bytecode() == Bytecodes::_f2i || bytecode() == Bytecodes::_f2l)
        __ fclasss(R5_scratch1, F23_ftos);
      else
        __ fclassd(R5_scratch1, F23_ftos);
      __ andi(R5_scratch1, R5_scratch1, (1 << 8) | (1 << 9)); // R5_scratch1 != 0 if NaN
      __ bnez(R5_scratch1, Lnan);
      switch (bytecode()) {
        case Bytecodes::_f2i: __ fcvtws(R25_tos, F23_ftos, Assembler::RTZ); break;
        case Bytecodes::_d2l: __ fcvtld(R25_tos, F23_ftos, Assembler::RTZ); break;
        case Bytecodes::_f2l: __ fcvtls(R25_tos, F23_ftos, Assembler::RTZ); break;
        case Bytecodes::_d2i: __ fcvtwd(R25_tos, F23_ftos, Assembler::RTZ); break;
        default: ShouldNotReachHere();
      }
      __ j(done);
      __ bind(Lnan);
      __ addi(R25_tos, R0_ZERO, 0);
      __ j(done);
      }
      break;


    default: ShouldNotReachHere();
  }
  __ bind(done);
}

// Long compare
void TemplateTable::lcmp() {
  transition(ltos, itos);

  Label Lless, Lgreater, Ldone;

  const Register Rscratch = R5_scratch1;
  __ pop_l(Rscratch); // first operand, deeper in stack

  __ blt(Rscratch, R25_tos, Lless);
  __ bgt(Rscratch, R25_tos, Lgreater);

  __ mv(R25_tos, R0_ZERO);
  __ j(Ldone);

  __ bind(Lless);
  __ addi(R25_tos, R0_ZERO, -1);
  __ j(Ldone);

  __ bind(Lgreater);
  __ addi(R25_tos, R0_ZERO, 1);

  __ bind(Ldone);
}

// fcmpl/fcmpg and dcmpl/dcmpg bytecodes
// unordered_result == -1 => fcmpl or dcmpl
// unordered_result ==  1 => fcmpg or dcmpg
void TemplateTable::float_cmp(bool is_float, int unordered_result) {
  const FloatRegister Rfirst  = F0_TMP0,
                      Rsecond = F23_ftos;
  const Register Rscratch1 = R5_scratch1;
  const Register Rscratch2 = R6_scratch2;

  if (is_float) {
    __ pop_f(Rfirst);
  } else {
    __ pop_d(Rfirst);
  }

  Label Lunordered, Ldone;
  if (unordered_result) {
    if (is_float) {
        __ fclasss(Rscratch1, Rfirst);  // set bit 8 or 9 if NaN
        __ fclasss(Rscratch2, Rsecond); // set bit 8 or 9 if NaN
    } else {
        __ fclassd(Rscratch1, Rfirst);  // set bit 8 or 9 if NaN
        __ fclassd(Rscratch2, Rsecond); // set bit 8 or 9 if NaN
    }
    __ orr(Rscratch1, Rscratch1, Rscratch2);
    __ srli(Rscratch1, Rscratch1, 8);
    __ bnez(Rscratch1, Lunordered);
  }
  if (is_float) {
      __ flts(Rscratch1, Rfirst, Rsecond);
      __ flts(Rscratch2, Rsecond, Rfirst);
  } else {
      __ fltd(Rscratch1, Rfirst, Rsecond);
      __ fltd(Rscratch2, Rsecond, Rfirst);
  }
  __ neg(Rscratch1, Rscratch1);
  __ orr(R25_tos, Rscratch1, Rscratch2); // set result as follows: <: -1, =: 0, >: 1
  if (unordered_result) {
    __ j(Ldone);
    __ bind(Lunordered);
    __ li(R25_tos, unordered_result);
  }
  __ bind(Ldone);
}

void TemplateTable::branch(bool is_jsr, bool is_wide) {
  // Note: on SPARC, we use InterpreterMacroAssembler::if_cmp also.
  __ verify_thread();

  const Register Rscratch1    = R5_scratch1,
                 Rscratch2    = R6_scratch2,
                 Rscratch3    = R10_ARG0,
                 Rcounters    = R11_ARG1,
                 bumped_count = R31_TMP6,
                 Rdisp        = R30_TMP5;

  __ profile_taken_branch(Rscratch1, bumped_count);

  // Get (wide) offset.
  if (is_wide) {
    __ get_4_byte_integer_at_bcp(1, Rdisp, InterpreterMacroAssembler::Signed);
  } else {
    __ get_2_byte_integer_at_bcp(1, Rdisp, InterpreterMacroAssembler::Signed);
  }

  // --------------------------------------------------------------------------
  // Handle all the JSR stuff here, then exit.
  // It's much shorter and cleaner than intermingling with the
  // non-JSR normal-branch stuff occurring below.
  if (is_jsr) {
    // Compute return address as bci in Otos_i.
    __ ld(Rscratch1, R27_method, in_bytes(Method::const_offset()));
    __ addi(Rscratch2, R22_bcp, -in_bytes(ConstMethod::codes_offset()) + (is_wide ? 5 : 3));
    __ sub(R25_tos, Rscratch2, Rscratch1);

    // Bump bcp to target of JSR.
    __ add(R22_bcp, Rdisp, R22_bcp);
    // Push returnAddress for "ret" on stack.
    __ push_ptr(R25_tos);
    // And away we go!
    __ dispatch_next(vtos, 0, true);
    return;
  }

  // --------------------------------------------------------------------------
  // Normal (non-jsr) branch handling

  // Bump bytecode pointer by displacement (take the branch).
  __ add(R22_bcp, Rdisp, R22_bcp); // Add to bc addr.

  const bool increment_invocation_counter_for_backward_branches = UseCompiler && UseLoopCounter;
  if (increment_invocation_counter_for_backward_branches) {
    __ unimplemented("Increment invocation counter for backward branches is not implemented");
    Label Lforward;

    // Check branch direction.
    __ cmpdi_PPC(CCR0, Rdisp, 0);
    __ bgt_PPC(CCR0, Lforward);

    __ get_method_counters(R27_method, Rcounters, Lforward);

    if (TieredCompilation) {
      Label Lno_mdo, Loverflow;
      const int increment = InvocationCounter::count_increment;
      if (ProfileInterpreter) {
        Register Rmdo = Rscratch1;

        // If no method data exists, go to profile_continue.
        __ ld_PPC(Rmdo, in_bytes(Method::method_data_offset()), R27_method);
        __ cmpdi_PPC(CCR0, Rmdo, 0);
        __ beq_PPC(CCR0, Lno_mdo);

        // Increment backedge counter in the MDO.
        const int mdo_bc_offs = in_bytes(MethodData::backedge_counter_offset()) + in_bytes(InvocationCounter::counter_offset());
        __ lwz_PPC(Rscratch2, mdo_bc_offs, Rmdo);
        __ lwz_PPC(Rscratch3, in_bytes(MethodData::backedge_mask_offset()), Rmdo);
        __ addi_PPC(Rscratch2, Rscratch2, increment);
        __ stw_PPC(Rscratch2, mdo_bc_offs, Rmdo);
        if (UseOnStackReplacement) {
          __ and__PPC(Rscratch3, Rscratch2, Rscratch3);
          __ bne_PPC(CCR0, Lforward);
          __ b_PPC(Loverflow);
        } else {
          __ b_PPC(Lforward);
        }
      }

      // If there's no MDO, increment counter in method.
      const int mo_bc_offs = in_bytes(MethodCounters::backedge_counter_offset()) + in_bytes(InvocationCounter::counter_offset());
      __ bind(Lno_mdo);
      __ lwz_PPC(Rscratch2, mo_bc_offs, Rcounters);
      __ lwz_PPC(Rscratch3, in_bytes(MethodCounters::backedge_mask_offset()), Rcounters);
      __ addi_PPC(Rscratch2, Rscratch2, increment);
      __ stw_PPC(Rscratch2, mo_bc_offs, Rcounters);
      if (UseOnStackReplacement) {
        __ and__PPC(Rscratch3, Rscratch2, Rscratch3);
        __ bne_PPC(CCR0, Lforward);
      } else {
        __ b_PPC(Lforward);
      }
      __ bind(Loverflow);

      // Notify point for loop, pass branch bytecode.
      __ subf_PPC(R4_ARG2_PPC, Rdisp, R22_bcp); // Compute branch bytecode (previous bcp).
      __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::frequency_counter_overflow), R4_ARG2_PPC, true);

      // Was an OSR adapter generated?
      __ cmpdi_PPC(CCR0, R3_RET_PPC, 0);
      __ beq_PPC(CCR0, Lforward);

      // Has the nmethod been invalidated already?
      __ lbz_PPC(R0, nmethod::state_offset(), R3_RET_PPC);
      __ cmpwi_PPC(CCR0, R0, nmethod::in_use);
      __ bne_PPC(CCR0, Lforward);

      // Migrate the interpreter frame off of the stack.
      // We can use all registers because we will not return to interpreter from this point.

      // Save nmethod.
      const Register osr_nmethod = R31;
      __ mr_PPC(osr_nmethod, R3_RET_PPC);
      __ set_top_ijava_frame_at_SP_as_last_Java_frame(R1_SP_PPC, noreg, R5_scratch1);
      __ call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::OSR_migration_begin), R24_thread);
      __ reset_last_Java_frame();
      // OSR buffer is in ARG1.

      // Remove the interpreter frame.
      __ pop_java_frame();

      // Jump to the osr code.
      __ ld_PPC(R5_scratch1, nmethod::osr_entry_point_offset(), osr_nmethod);
      __ jr(R5_scratch1);

    } else {

      const Register invoke_ctr = Rscratch1;
      // Update Backedge branch separately from invocations.
      __ increment_backedge_counter(Rcounters, invoke_ctr, Rscratch2, Rscratch3);

      if (ProfileInterpreter) {
        __ test_invocation_counter_for_mdp(invoke_ctr, Rcounters, Rscratch2, Lforward);
        if (UseOnStackReplacement) {
          __ test_backedge_count_for_osr(bumped_count, Rcounters, R22_bcp, Rdisp, Rscratch2);
        }
      } else {
        if (UseOnStackReplacement) {
          __ test_backedge_count_for_osr(invoke_ctr, Rcounters, R22_bcp, Rdisp, Rscratch2);
        }
      }
    }

    __ bind(Lforward);
  }
  __ dispatch_next(vtos, 0, true);
}

// Helper function for if_cmp* methods below.
// Factored out common compare and branch code.
void TemplateTable::if_cmp_common(Condition cc, Register Rfirst, Register Rsecond, Register Rscratch1, Register Rscratch2) {
  // assume branch is more often taken than not (loops use backward branches)
  Label not_taken;
  switch (cc) {
    case equal:         __ bne(Rfirst, Rsecond, not_taken); break;
    case not_equal:     __ beq(Rfirst, Rsecond, not_taken); break;
    case less:          __ bge(Rfirst, Rsecond, not_taken); break;
    case less_equal:    __ bgt(Rfirst, Rsecond, not_taken); break;
    case greater:       __ ble(Rfirst, Rsecond, not_taken); break;
    case greater_equal: __ blt(Rfirst, Rsecond, not_taken); break;
  }

  branch(false, false);
  __ bind(not_taken);
  __ profile_not_taken_branch(Rscratch1, Rscratch2);
}

// Compare integer values with zero and fall through if CC holds, branch away otherwise.
void TemplateTable::if_0cmp(Condition cc) {
  transition(itos, vtos);
  if_cmp_common(cc, R25_tos, R0_ZERO, R5_scratch1, R6_scratch2);
}

// Compare integer values and fall through if CC holds, branch away otherwise.
//
// Interface:
//  - Rfirst: First operand  (older stack value)
//  - tos:    Second operand (younger stack value)
void TemplateTable::if_icmp(Condition cc) {
  transition(itos, vtos);

  const Register Rfirst  = R30_TMP5,
                 Rsecond = R25_tos;

  __ pop_i(Rfirst);
  if_cmp_common(cc, Rfirst, Rsecond, R5_scratch1, R6_scratch2);
}

void TemplateTable::if_nullcmp(Condition cc) {
  transition(atos, vtos);
  if_cmp_common(cc, R25_tos, R0_ZERO, R5_scratch1, R6_scratch2);
}

void TemplateTable::if_acmp(Condition cc) {
  transition(atos, vtos);

  const Register Rfirst  = R30_TMP5,
                 Rsecond = R25_tos;

  __ pop_ptr(Rfirst);
  if_cmp_common(cc, Rfirst, Rsecond, R5_scratch1, R6_scratch2);
}

void TemplateTable::ret() {
  locals_index(R5_scratch1);
  __ load_local_ptr(R25_tos, R5_scratch1, R5_scratch1);

  __ profile_ret(vtos, R25_tos, R5_scratch1, R6_scratch2);

  __ ld(R5_scratch1, R27_method, in_bytes(Method::const_offset()));
  __ add(R5_scratch1, R25_tos, R5_scratch1);
  __ addi(R22_bcp, R5_scratch1, in_bytes(ConstMethod::codes_offset()));
  __ dispatch_next(vtos, 0, true);
}

void TemplateTable::wide_ret() {
  transition(vtos, vtos);

  const Register Rindex = R10_ARG0,
                 Rscratch1 = R5_scratch1,
                 Rscratch2 = R6_scratch2;

  locals_index_wide(Rindex);
  __ load_local_ptr(R25_tos, R25_tos, Rindex);
  __ profile_ret(vtos, R25_tos, Rscratch1, R6_scratch2);
  // Tos now contains the bci, compute the bcp from that.
  __ ld(Rscratch1, R27_method, in_bytes(Method::const_offset()));
  __ addi(Rscratch2, R25_tos, in_bytes(ConstMethod::codes_offset()));
  __ add(R22_bcp, Rscratch1, Rscratch2);
  __ dispatch_next(vtos, 0, true);
}

void TemplateTable::tableswitch() {
  transition(itos, vtos);

  Label Ldispatch, Ldefault_case;
  Register Rlow_byte         = R10_ARG0,
           Rindex            = Rlow_byte,
           Rhigh_byte        = R11_ARG1,
           Rdef_offset_addr  = R12_ARG2, // is going to contain address of default offset
           Rscratch1         = R5_scratch1,
           Rscratch2         = R6_scratch2,
           Roffset           = R14_ARG4;

  // Align bcp.
  __ addi(Rdef_offset_addr, R22_bcp, BytesPerInt);
  __ andi(Rdef_offset_addr, Rdef_offset_addr, -BytesPerInt); // clear low bits

  // Load lo & hi.
  __ get_u4(Rlow_byte, Rdef_offset_addr, BytesPerInt, InterpreterMacroAssembler::Unsigned);
  __ get_u4(Rhigh_byte, Rdef_offset_addr, 2 *BytesPerInt, InterpreterMacroAssembler::Unsigned);

  // Check for default case (=index outside [low,high]).
  __ blt(R25_tos, Rlow_byte, Ldefault_case);
  __ bgt(R25_tos, Rhigh_byte, Ldefault_case);

  // Lookup dispatch offset.
  __ sub(Rindex, R25_tos, Rlow_byte);
  __ profile_switch_case(Rindex, Rhigh_byte /* scratch */, Rscratch1, Rscratch2);
  __ slli(Rindex, Rindex, LogBytesPerInt);
  __ addi(Rindex, Rindex, 3 * BytesPerInt);
  __ add(Rdef_offset_addr, Rdef_offset_addr, Rindex);
  __ get_u4(Roffset, Rdef_offset_addr, 0, InterpreterMacroAssembler::Signed);
  __ j(Ldispatch);

  __ bind(Ldefault_case);
  __ profile_switch_default(Rhigh_byte, Rscratch1);
  __ get_u4(Roffset, Rdef_offset_addr, 0, InterpreterMacroAssembler::Signed);

  __ bind(Ldispatch);

  __ add(R22_bcp, Roffset, R22_bcp);
  __ dispatch_next(vtos, 0, true);
}

void TemplateTable::lookupswitch() {
  transition(itos, itos);
  __ stop("lookupswitch bytecode should have been rewritten");
}

// Table switch using linear search through cases.
// Bytecode stream format:
// Bytecode (1) | 4-byte padding | default offset (4) | count (4) | value/offset pair1 (8) | value/offset pair2 (8) | ...
// Note: Everything is big-endian format here.
void TemplateTable::fast_linearswitch() {
  transition(itos, vtos);

  Label Lloop_entry, Lsearch_loop, Lcontinue_execution, Ldefault_case;
  Register Rcount           = R10_ARG0,
           Rcurrent_pair    = R11_ARG1,
           Rdef_offset_addr = R12_ARG2, // Is going to contain address of default offset.
           Roffset          = R31,     // Might need to survive C call.
           Rvalue           = R6_scratch2,
           Rscratch         = R5_scratch1,
           Rcmp_value       = R25_tos;

  // Align bcp.
  __ addi(Rdef_offset_addr, R22_bcp, BytesPerInt);
  __ andi(Rdef_offset_addr, Rdef_offset_addr, -BytesPerInt); // clear low bits

  // Setup loop counter and limit.
  __ get_u4(Rcount, Rdef_offset_addr, BytesPerInt, InterpreterMacroAssembler::Unsigned);
  __ addi(Rcurrent_pair, Rdef_offset_addr, 2 * BytesPerInt); // Rcurrent_pair now points to first pair.

  __ bnez(Rcount, Lloop_entry);

  // Default case
  __ bind(Ldefault_case);
  __ get_u4(Roffset, Rdef_offset_addr, 0, InterpreterMacroAssembler::Signed);
  if (ProfileInterpreter) {
    __ profile_switch_default(Rdef_offset_addr, Rcount/* scratch */);
  }
  __ j(Lcontinue_execution);

  // Next iteration
  __ bind(Lsearch_loop);
  __ addi(Rcount, Rcount, -1);
  __ beqz(Rcount, Ldefault_case);
  __ addi(Rcurrent_pair, Rcurrent_pair, 2 * BytesPerInt);

  __ bind(Lloop_entry);
  __ get_u4(Rvalue, Rcurrent_pair, 0, InterpreterMacroAssembler::Signed);
  __ bne(Rvalue, Rcmp_value, Lsearch_loop);

  // Found, load offset.
  __ get_u4(Roffset, Rcurrent_pair, BytesPerInt, InterpreterMacroAssembler::Signed);
  // Calculate case index and profile
  if (ProfileInterpreter) {
    __ sub(Rcurrent_pair, Rcurrent_pair, Rdef_offset_addr);
    __ addi(Rcurrent_pair, Rcurrent_pair, -2 * BytesPerInt);
    __ srli(Rcurrent_pair, Rcurrent_pair, exact_log2(2 * BytesPerInt));
    __ profile_switch_case(Rcurrent_pair, Rcount /*scratch*/, Rdef_offset_addr/*scratch*/, Rscratch);
  }

  __ bind(Lcontinue_execution);
  __ add(R22_bcp, Roffset, R22_bcp);
  __ dispatch_next(vtos, 0, true);
}

// Table switch using binary search (value/offset pairs are ordered).
// Bytecode stream format:
// Bytecode (1) | 4-byte padding | default offset (4) | count (4) | value/offset pair1 (8) | value/offset pair2 (8) | ...
// Note: Everything is big-endian format here. So on little endian machines, we have to revers offset and count and cmp value.
void TemplateTable::fast_binaryswitch() {

  transition(itos, vtos);
  // Implementation using the following core algorithm: (copied from Intel)
  //
  // int binary_search(int key, LookupswitchPair* array, int n) {
  //   // Binary search according to "Methodik des Programmierens" by
  //   // Edsger W. Dijkstra and W.H.J. Feijen, Addison Wesley Germany 1985.
  //   int i = 0;
  //   int j = n;
  //   while (i+1 < j) {
  //     // invariant P: 0 <= i < j <= n and (a[i] <= key < a[j] or Q)
  //     // with      Q: for all i: 0 <= i < n: key < a[i]
  //     // where a stands for the array and assuming that the (inexisting)
  //     // element a[n] is infinitely big.
  //     int h = (i + j) >> 1;
  //     // i < h < j
  //     if (key < array[h].fast_match()) {
  //       j = h;
  //     } else {
  //       i = h;
  //     }
  //   }
  //   // R: a[i] <= key < a[i+1] or Q
  //   // (i.e., if key is within array, i is the correct index)
  //   return i;
  // }

  // register allocation
  const Register Rkey      = R25_tos;          // already set (tosca)
  const Register Rarray    = R10_ARG0;
  const Register Ri        = R11_ARG1;
  const Register Rj        = R12_ARG2;
  const Register Rh        = R13_ARG3;
  const Register Rscratch  = R5_scratch1;
  const Register Rscratch2 = R6_scratch2;

  const int log_entry_size = 3;
  const int entry_size = 1 << log_entry_size;

  Label found;

  // Find Array start,
  __ addi(Rarray, R22_bcp, 3 * BytesPerInt);
  __ andi(Rarray, Rarray, -BytesPerInt);

  // initialize i and j
  __ li(Ri, 0L);
  __ get_u4(Rj, Rarray, -BytesPerInt, InterpreterMacroAssembler::Unsigned);

  // and start.
  Label entry;
  __ j(entry);

  // binary search loop
  { Label loop;
    __ bind(loop);
    // int h = (i + j) >> 1;
    __ add(Rh, Ri, Rj);
    __ srli(Rh, Rh, 1);
    // if (key < array[h].fast_match()) {
    //   j = h;
    // } else {
    //   i = h;
    // }
    __ slli(Rscratch2, Rh, log_entry_size);
    __ add(Rscratch2, Rscratch2, Rarray);
    __ get_u4(Rscratch, Rscratch2, 0, InterpreterMacroAssembler::Signed);

    // if (key < current value)
    //   Rj = Rh
    // else
    //   Ri = Rh
    Label Lgreater;
    __ bge(Rkey, Rscratch, Lgreater);
    __ mv(Rj, Rh);
    __ j(entry);
    __ bind(Lgreater);
    __ mv(Ri, Rh);

    // while (i+1 < j)
    __ bind(entry);
    __ addi(Rscratch, Ri, 1);
    __ blt(Rscratch, Rj, loop);
  }

  // End of binary search, result index is i (must check again!).
  Label default_case;
  Label continue_execution;
  if (ProfileInterpreter) {
    __ mv(Rh, Ri);              // Save index in i for profiling.
  }
  // Ri = value offset
  __ slli(Ri, Ri, log_entry_size);
  __ add(Ri, Ri, Rarray);
  __ get_u4(Rscratch, Ri, 0, InterpreterMacroAssembler::Unsigned);

  Label not_found;
  // Ri = offset offset
  __ beq(Rkey, Rscratch, not_found);
  // entry not found -> j = default offset
  __ get_u4(Rj, Rarray, -2 * BytesPerInt, InterpreterMacroAssembler::Unsigned);
  __ j(default_case);

  __ bind(not_found);
  // entry found -> j = offset
  __ profile_switch_case(Rh, Rj, Rscratch, Rkey);
  __ get_u4(Rj, Ri, BytesPerInt, InterpreterMacroAssembler::Unsigned);

  if (ProfileInterpreter) {
    __ j(continue_execution);
  }

  __ bind(default_case); // fall through (if not profiling)
  __ profile_switch_default(Ri, Rscratch);

  __ bind(continue_execution);

  __ add(R22_bcp, Rj, R22_bcp);
  __ dispatch_next(vtos, 0, true);
}

void TemplateTable::_return(TosState state) {
  transition(state, state);
  assert(_desc->calls_vm(),
         "inconsistent calls_vm information"); // call in remove_activation

  if (_desc->bytecode() == Bytecodes::_return_register_finalizer) {
    Register Rscratch     = R5_scratch1,
             Rklass       = R6_scratch2,
             Rklass_flags = Rklass;
    Label Lskip_register_finalizer;

    // Check if the method has the FINALIZER flag set and call into the VM to finalize in this case.
    assert(state == vtos, "only valid state");
    __ ld(R25_tos, R26_locals, 0);

    // Load klass of this obj.
    __ load_klass(Rklass, R25_tos);
    __ lwu(Rklass_flags, Rklass, in_bytes(Klass::access_flags_offset()));
    __ li(Rscratch, JVM_ACC_HAS_FINALIZER);
    __ andr(Rscratch, Rklass_flags,Rscratch);
    __ beqz(Rscratch, Lskip_register_finalizer);

    __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::register_finalizer), R25_tos /* obj */);

    __ align(32, 12);
    __ bind(Lskip_register_finalizer);
  }

  if (SafepointMechanism::uses_thread_local_poll() && _desc->bytecode() != Bytecodes::_return_register_finalizer) {
    Label no_safepoint;
    __ ld(R5_scratch1, R24_thread, in_bytes(Thread::polling_page_offset()));
    __ andi(R5_scratch1, R5_scratch1, SafepointMechanism::poll_bit());
    __ beqz(R5_scratch1, no_safepoint);
    __ push(state);
    __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::at_safepoint));
    __ pop(state);
    __ bind(no_safepoint);
  }

  // Move the result value into the correct register and remove memory stack frame.
  __ remove_activation(state, /* throw_monitor_exception */ true);
  // Restoration of lr done by remove_activation.
  switch (state) {
    case itos:
    case ltos:
    case atos: __ mv(R10_RET1, R25_tos); break;
    case ftos:
    case dtos: __ fmvd(F10_RET, F23_ftos); break;
    case vtos: // This might be a constructor. Final fields (and volatile fields on RISCV64) need
               // to get visible before the reference to the object gets stored anywhere.
               __ membar(Assembler::StoreStore); break;
    default  : ShouldNotReachHere();
  }
  __ ret();
}

// ============================================================================
// Constant pool cache access
//
// Memory ordering:
//
// Like done in C++ interpreter, we load the fields
//   - _indices
//   - _f12_oop
// acquired, because these are asked if the cache is already resolved. We don't
// want to float loads above this check.
// See also comments in ConstantPoolCacheEntry::bytecode_1(),
// ConstantPoolCacheEntry::bytecode_2() and ConstantPoolCacheEntry::f1();

// Call into the VM if call site is not yet resolved
//
// Input regs:
//   - None, all passed regs are outputs.
//
// Returns:
//   - Rcache:  The const pool cache entry that contains the resolved result.
//   - Rresult: Either noreg or output for f1/f2.
//
// Kills:
//   - Rscratch
void TemplateTable::resolve_cache_and_index(int byte_no, Register Rcache, Register Rscratch, size_t index_size) {
  __ get_cache_and_index_at_bcp(Rcache, 1, index_size);

  Label Lresolved, Ldone, L_clinit_barrier_slow;

  Bytecodes::Code code = bytecode();
  switch (code) {
    case Bytecodes::_nofast_getfield: code = Bytecodes::_getfield; break;
    case Bytecodes::_nofast_putfield: code = Bytecodes::_putfield; break;
    default:
      break;
  }

  assert(byte_no == f1_byte || byte_no == f2_byte, "byte_no out of range");

  // We are resolved if the indices offset contains the current bytecode.
#if defined(VM_LITTLE_ENDIAN)
  __ lbu(Rscratch, Rcache, in_bytes(ConstantPoolCache::base_offset() + ConstantPoolCacheEntry::indices_offset()) + byte_no + 1);
#else
  __ lbu(Rscratch, Rcache, in_bytes(ConstantPoolCache::base_offset() + ConstantPoolCacheEntry::indices_offset()) + 7 - (byte_no + 1));
#endif

  __ li(R11_ARG1, (int) code);

  __ beq(Rscratch, R11_ARG1, Lresolved);


  // Class initialization barrier slow path lands here as well.
  __ bind(L_clinit_barrier_slow);

  address entry = CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_from_cache);

  __ li(R11_ARG1, (int) code);
  __ call_VM(noreg, entry, R11_ARG1, true);


  // Update registers with resolved info.
  __ get_cache_and_index_at_bcp(Rcache, 1, index_size);

  __ j(Ldone);

  __ bind(Lresolved);

  __ acquire();

  // Class initialization barrier for static methods
  if (VM_Version::supports_fast_class_init_checks() && bytecode() == Bytecodes::_invokestatic) {
    const Register method = Rscratch;
    const Register klass  = Rscratch;

    __ load_resolved_method_at_index(byte_no, Rcache, method);
    __ load_method_holder(klass, method);
    __ clinit_barrier(klass, R24_thread, NULL /*L_fast_path*/, &L_clinit_barrier_slow);
  }

  __ bind(Ldone);
}

// Load the constant pool cache entry at field accesses into registers.
// The Rcache and Rindex registers must be set before call.
// Input:
//   - Rcache, Rindex
// Output:
//   - Robj, Roffset, Rflags
void TemplateTable::load_field_cp_cache_entry(Register Robj,
                                              Register Rcache,
                                              Register Rindex /* unused on RISCV64 */,
                                              Register Roffset,
                                              Register Rflags,
                                              bool is_static = false) {
  assert_different_registers(Rcache, Rflags, Roffset);
  // assert(Rindex == noreg, "parameter not used on RISCV64");

  ByteSize cp_base_offset = ConstantPoolCache::base_offset();
  __ ld(Rflags, Rcache, in_bytes(cp_base_offset) + in_bytes(ConstantPoolCacheEntry::flags_offset()));
  __ ld(Roffset, Rcache, in_bytes(cp_base_offset) + in_bytes(ConstantPoolCacheEntry::f2_offset()));
  if (is_static) {
    __ ld(Robj, Rcache, in_bytes(cp_base_offset) + in_bytes(ConstantPoolCacheEntry::f1_offset()));
    __ ld(Robj, Robj, in_bytes(Klass::java_mirror_offset()));
    __ resolve_oop_handle(Robj);
    // Acquire not needed here. Following access has an address dependency on this value.
  }
}

// Load the constant pool cache entry at invokes into registers.
// Resolve if necessary.

// Input Registers:
//   - None, bcp is used, though
//
// Return registers:
//   - Rmethod       (f1 field or f2 if invokevirtual)
//   - Ritable_index (f2 field)
//   - Rflags        (flags field)
//
// Kills:
//   - R21
//
void TemplateTable::load_invoke_cp_cache_entry(int byte_no,
                                               Register Rmethod,
                                               Register Ritable_index,
                                               Register Rflags,
                                               bool is_invokevirtual,
                                               bool is_invokevfinal,
                                               bool is_invokedynamic) {
  ByteSize cp_base_offset = ConstantPoolCache::base_offset();
  // Determine constant pool cache field offsets.
  assert(is_invokevirtual == (byte_no == f2_byte), "is_invokevirtual flag redundant");
  const int method_offset = in_bytes(cp_base_offset + (is_invokevirtual ? ConstantPoolCacheEntry::f2_offset() : ConstantPoolCacheEntry::f1_offset()));
  const int flags_offset  = in_bytes(cp_base_offset + ConstantPoolCacheEntry::flags_offset());
  // Access constant pool cache fields.
  const int index_offset  = in_bytes(cp_base_offset + ConstantPoolCacheEntry::f2_offset());

  {
    Register Rcache = Rflags;

    if (is_invokevfinal) {
      assert(Ritable_index == noreg, "register not used");
      // Already resolved.
      __ get_cache_and_index_at_bcp(Rcache, 1);
    } else {
      resolve_cache_and_index(byte_no, Rcache, /* temp */ Rmethod, is_invokedynamic ? sizeof(u4) : sizeof(u2));
    }

    if (Ritable_index != noreg) {
      __ ld(Ritable_index, Rcache, index_offset);
    }

    __ ld(Rmethod, Rcache, method_offset);
    __ ld(Rflags, Rcache, flags_offset); // Rcache is dead now
  }
}

// ============================================================================
// Field access

// Volatile variables demand their effects be made known to all CPU's
// in order. Store buffers on most chips allow reads & writes to
// reorder; the JMM's ReadAfterWrite.java test fails in -Xint mode
// without some kind of memory barrier (i.e., it's not sufficient that
// the interpreter does not reorder volatile references, the hardware
// also must not reorder them).
//
// According to the new Java Memory Model (JMM):
// (1) All volatiles are serialized wrt to each other. ALSO reads &
//     writes act as aquire & release, so:
// (2) A read cannot let unrelated NON-volatile memory refs that
//     happen after the read float up to before the read. It's OK for
//     non-volatile memory refs that happen before the volatile read to
//     float down below it.
// (3) Similar a volatile write cannot let unrelated NON-volatile
//     memory refs that happen BEFORE the write float down to after the
//     write. It's OK for non-volatile memory refs that happen after the
//     volatile write to float up before it.
//
// We only put in barriers around volatile refs (they are expensive),
// not _between_ memory refs (that would require us to track the
// flavor of the previous memory refs). Requirements (2) and (3)
// require some barriers before volatile stores and after volatile
// loads. These nearly cover requirement (1) but miss the
// volatile-store-volatile-load case.  This final case is placed after
// volatile-stores although it could just as well go before
// volatile-loads.

// The registers cache and index expected to be set before call.
// Correct values of the cache and index registers are preserved.
// Kills:
//   Rcache (if has_tos)
//   Rscratch
void TemplateTable::jvmti_post_field_access(Register Rcache, Register Rscratch, bool is_static, bool has_tos) {

  assert_different_registers(Rcache, Rscratch);

  if (JvmtiExport::can_post_field_access()) {
    ByteSize cp_base_offset = ConstantPoolCache::base_offset();
    Label Lno_field_access_post;

    // Check if post field access in enabled.
    int offs = __ load_const_optimized(Rscratch, JvmtiExport::get_field_access_count_addr(), R0, true);
    __ lwz_PPC(Rscratch, offs, Rscratch);

    __ cmpwi_PPC(CCR0, Rscratch, 0);
    __ beq_PPC(CCR0, Lno_field_access_post);

    // Post access enabled - do it!
    __ addi_PPC(Rcache, Rcache, in_bytes(cp_base_offset));
    if (is_static) {
      __ li_PPC(R25_tos, 0);
    } else {
      if (has_tos) {
        // The fast bytecode versions have obj ptr in register.
        // Thus, save object pointer before call_VM() clobbers it
        // put object on tos where GC wants it.
        __ push_ptr(R25_tos);
      } else {
        // Load top of stack (do not pop the value off the stack).
        __ ld_PPC(R25_tos, Interpreter::expr_offset_in_bytes(0), R23_esp);
      }
      __ verify_oop(R25_tos);
    }
    // tos:   object pointer or NULL if static
    // cache: cache entry pointer
    __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::post_field_access), R25_tos, Rcache);
    if (!is_static && has_tos) {
      // Restore object pointer.
      __ pop_ptr(R25_tos);
      __ verify_oop(R25_tos);
    } else {
      // Cache is still needed to get class or obj.
      __ get_cache_and_index_at_bcp(Rcache, 1);
    }

    __ align(32, 12);
    __ bind(Lno_field_access_post);
  }
}

// kills R5_scratch1
void TemplateTable::pop_and_check_object(Register Roop) {
  Register Rtmp = R5_scratch1;

  assert_different_registers(Rtmp, Roop);
  __ pop_ptr(Roop);
  // For field access must check obj.
  //__ null_check_throw(Roop, -1, Rtmp); FIXME_RISCV
  //__ verify_oop(Roop); FIXME_RISCV
}

// RISCV64: implement volatile loads as fence-store-acquire.
void TemplateTable::getfield_or_static(int byte_no, bool is_static, RewriteControl rc) {
  transition(vtos, vtos);

  Label Lisync;

  const Register Rcache        = R12_ARG2,
                 Rclass_or_obj = R7_TMP2,
                 Roffset       = R28_TMP3,
                 Rflags        = R31_TMP6,
                 Rbtable       = R13_ARG3,
                 Rbc           = R14_ARG4,
                 Rscratch      = R6_scratch2;

  static address field_branch_table[number_of_states],
                 static_branch_table[number_of_states];

  address* branch_table = (is_static || rc == may_not_rewrite) ? static_branch_table : field_branch_table;

  // Get field offset.
  resolve_cache_and_index(byte_no, Rcache, Rscratch, sizeof(u2));

  // JVMTI support
  //jvmti_post_field_access(Rcache, Rscratch, is_static, false); FIXME_RISCV

  // Load after possible GC.
  load_field_cp_cache_entry(Rclass_or_obj, Rcache, noreg, Roffset, Rflags, is_static);

  // Load pointer to branch table.
  __ li(Rbtable, (long)(unsigned long)(address)branch_table);

  // Get volatile flag.
  __ srli(Rscratch, Rflags, ConstantPoolCacheEntry::is_volatile_shift);
  __ andi(Rscratch, Rscratch, 1); // Extract volatile bit.
  // Note: sync is needed before volatile load on RISCV64.

  // Check field type.
  __ srli(Rflags, Rflags, ConstantPoolCacheEntry::tos_state_shift);
  __ andi(Rflags, Rflags, (1 << ConstantPoolCacheEntry::tos_state_bits) - 1);

#ifdef ASSERT
  Label LFlagInvalid;
  __ addi(Rcache, R0_ZERO, number_of_states);
  __ bge(Rflags, Rcache, LFlagInvalid);
#endif

  // Load from branch table and dispatch (volatile case: one instruction ahead).
  __ slli(Rflags, Rflags, LogBytesPerWord);
  if (support_IRIW_for_not_multiple_copy_atomic_cpu) {
    __ slli(Rscratch, Rscratch, exact_log2(BytesPerInstWord)); // Volatile ? size of 1 instruction : 0.
  }
  __ add(Rbtable, Rbtable, Rflags);
  __ ld(Rbtable, Rbtable, 0);

  // Get the obj from stack.
  if (!is_static) {
    pop_and_check_object(Rclass_or_obj); // Kills R5_scratch1.
  } else {
    //__ verify_oop(Rclass_or_obj); FIXME_RISCV
  }

  if (support_IRIW_for_not_multiple_copy_atomic_cpu) {
    __ sub(Rbtable, Rbtable, Rscratch); // Point to volatile/non-volatile entry point.
  }
  __ jr(Rbtable);

#ifdef ASSERT
  __ bind(LFlagInvalid);
  __ stop("got invalid flag", 0x654);
#endif

  if (!is_static && rc == may_not_rewrite) {
    // We reuse the code from is_static.  It's jumped to via the table above.
    return;
  }

#ifdef ASSERT
  // __ bind(Lvtos);
  address pc_before_fence = __ pc();
  __ fence(); // Volatile entry point (one instruction before non-volatile_entry point).
  assert(__ pc() - pc_before_fence == (ptrdiff_t)BytesPerInstWord, "must be single instruction");
  assert(branch_table[vtos] == 0, "can't compute twice");
  branch_table[vtos] = __ pc(); // non-volatile_entry point
  __ stop("vtos unexpected", 0x655);
#endif

  __ align(32, 28, 28); // Align load.
  // __ bind(Ldtos);
  //__ Assembler::fence(Assembler::RW_OP, Assembler::RW_OP); // Volatile entry point (one instruction before non-volatile_entry point).
  __ nop();
  assert(branch_table[dtos] == 0, "can't compute twice");
  branch_table[dtos] = __ pc(); // non-volatile_entry point
  __ add(R30_TMP5, Rclass_or_obj, Roffset);
  __ fld(F23_ftos, R30_TMP5, 0);
  __ push(dtos);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_dgetfield, Rbc, Rscratch);
  }
  {
    __ bnez(Rscratch, Lisync); // Volatile?
    __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));
  }

  __ align(32, 28, 28); // Align load.
  // __ bind(Lftos);
  //__ Assembler::fence(Assembler::RW_OP, Assembler::RW_OP); // Volatile entry point (one instruction before non-volatile_entry point).
  __ nop();
  assert(branch_table[ftos] == 0, "can't compute twice");
  branch_table[ftos] = __ pc(); // non-volatile_entry point
  __ add(R30_TMP5, Rclass_or_obj, Roffset);
  __ flw(F23_ftos, R30_TMP5, 0);
  __ push(ftos);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_fgetfield, Rbc, Rscratch);
  }
  {
    __ bnez(Rscratch, Lisync); // Volatile?
    __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));
  }

  __ align(32, 28, 28); // Align load.
  // __ bind(Litos);
  //__ Assembler::fence(Assembler::RW_OP, Assembler::RW_OP); // Volatile entry point (one instruction before non-volatile_entry point).
  __ nop();
  assert(branch_table[itos] == 0, "can't compute twice");
  branch_table[itos] = __ pc(); // non-volatile_entry point
  __ add(R30_TMP5, Rclass_or_obj, Roffset);
  __ lwu(R25_tos, R30_TMP5, 0);
  __ push(itos);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_igetfield, Rbc, Rscratch);
  }
  __ bnez(Rscratch, Lisync); // Volatile?
  __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

  __ align(32, 28, 28); // Align load.
  // __ bind(Lltos);
  //__ Assembler::fence(Assembler::RW_OP, Assembler::RW_OP); // Volatile entry point (one instruction before non-volatile_entry point).
  __ nop();
  assert(branch_table[ltos] == 0, "can't compute twice");
  branch_table[ltos] = __ pc(); // non-volatile_entry point
  __ add(R30_TMP5, Rclass_or_obj, Roffset);
  __ ld(R25_tos, R30_TMP5, 0);
  __ push(ltos);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_lgetfield, Rbc, Rscratch);
  }
  __ bnez(Rscratch, Lisync); // Volatile?
  __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

  __ align(32, 28, 28); // Align load.
  // __ bind(Lbtos);
  //__ Assembler::fence(Assembler::RW_OP, Assembler::RW_OP); // Volatile entry point (one instruction before non-volatile_entry point).
  __ nop();
  assert(branch_table[btos] == 0, "can't compute twice");
  branch_table[btos] = __ pc(); // non-volatile_entry point
  __ add(R30_TMP5, Rclass_or_obj, Roffset);
  __ lb(R25_tos, R30_TMP5, 0);
  __ push(btos);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_bgetfield, Rbc, Rscratch);
  }
  __ bnez(Rscratch, Lisync); // Volatile?
  __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

  __ align(32, 28, 28); // Align load.
  // __ bind(Lztos); (same code as btos)
  //__ Assembler::fence(Assembler::RW_OP, Assembler::RW_OP); // Volatile entry point (one instruction before non-volatile_entry point).
  __ nop();
  assert(branch_table[ztos] == 0, "can't compute twice");
  branch_table[ztos] = __ pc(); // non-volatile_entry point
  __ add(R30_TMP5, Rclass_or_obj, Roffset);
  __ lbu(R25_tos, R30_TMP5, 0);
  __ push(ztos);
  if (!is_static && rc == may_rewrite) {
    // use btos rewriting, no truncating to t/f bit is needed for getfield.
    patch_bytecode(Bytecodes::_fast_bgetfield, Rbc, Rscratch);
  }
  __ bnez(Rscratch, Lisync); // Volatile?
  __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

  __ align(32, 28, 28); // Align load.
  // __ bind(Lctos);
  //__ Assembler::fence(Assembler::RW_OP, Assembler::RW_OP); // Volatile entry point (one instruction before non-volatile_entry point).
  __ nop();
  assert(branch_table[ctos] == 0, "can't compute twice");
  branch_table[ctos] = __ pc(); // non-volatile_entry point
  __ add(R30_TMP5, Rclass_or_obj, Roffset);
  __ lhu(R25_tos, R30_TMP5, 0);
  __ push(ctos);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_cgetfield, Rbc, Rscratch);
  }
  __ bnez(Rscratch, Lisync); // Volatile?
  __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

  __ align(32, 28, 28); // Align load.
  // __ bind(Lstos);
  //__ Assembler::fence(Assembler::RW_OP, Assembler::RW_OP); // Volatile entry point (one instruction before non-volatile_entry point).
  __ nop();
  assert(branch_table[stos] == 0, "can't compute twice");
  branch_table[stos] = __ pc(); // non-volatile_entry point
  __ add(R30_TMP5, Rclass_or_obj, Roffset);
  __ lh(R25_tos, R30_TMP5, 0);
  __ push(stos);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_sgetfield, Rbc, Rscratch);
  }
  __ bnez(Rscratch, Lisync); // Volatile?
  __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

  __ align(32, 28, 28); // Align load.
  // __ bind(Latos);
  //__ Assembler::fence(Assembler::RW_OP, Assembler::RW_OP); // Volatile entry point (one instruction before non-volatile_entry point).
  __ nop();
  assert(branch_table[atos] == 0, "can't compute twice");
  branch_table[atos] = __ pc(); // non-volatile_entry point
  do_oop_load(_masm, Rclass_or_obj, Roffset, R25_tos, R5_scratch1, /* nv temp */ Rflags, IN_HEAP);
  //__ verify_oop(R25_tos); FIXME_RISCV
  __ push(atos);
  //__ dcbt_PPC(R25_tos); // prefetch
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_agetfield, Rbc, Rscratch);
  }
  __ bnez(Rscratch, Lisync); // Volatile?
  __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

  __ bind(Lisync);
  __ acquire();
#ifdef ASSERT
  for (int i = 0; i<number_of_states; ++i) {
    assert(branch_table[i], "get initialization");
    //tty->print_cr("get: %s_branch_table[%d] = 0x%llx (opcode 0x%llx)",
    //              is_static ? "static" : "field", i, branch_table[i], *((unsigned int*)branch_table[i]));
  }
#endif
}

void TemplateTable::getfield(int byte_no) {
  getfield_or_static(byte_no, false);
}

void TemplateTable::nofast_getfield(int byte_no) {
  tty->print_cr("nofast_getstatic #%i: %p", byte_no, __ pc());
  getfield_or_static(byte_no, false, may_not_rewrite);
}

void TemplateTable::getstatic(int byte_no) {
  getfield_or_static(byte_no, true);
}

// The registers cache and index expected to be set before call.
// The function may destroy various registers, just not the cache and index registers.
void TemplateTable::jvmti_post_field_mod(Register Rcache, Register Rscratch, bool is_static) {

  // FIXME_RISCV change registers
//  assert_different_registers(Rcache, Rscratch, R14_ARG4);

  if (JvmtiExport::can_post_field_modification()) {
    Label Lno_field_mod_post;

    // Check if post field access in enabled.
    int offs = __ load_const_optimized(Rscratch, JvmtiExport::get_field_modification_count_addr(), R0, true);
    __ lwz_PPC(Rscratch, offs, Rscratch);

    __ cmpwi_PPC(CCR0, Rscratch, 0);
    __ beq_PPC(CCR0, Lno_field_mod_post);

    // Do the post
    ByteSize cp_base_offset = ConstantPoolCache::base_offset();
    const Register Robj = Rscratch;

    __ addi_PPC(Rcache, Rcache, in_bytes(cp_base_offset));
    if (is_static) {
      // Life is simple. Null out the object pointer.
      __ li_PPC(Robj, 0);
    } else {
      // In case of the fast versions, value lives in registers => put it back on tos.
      int offs = Interpreter::expr_offset_in_bytes(0);
      Register base = R23_esp;
      switch(bytecode()) {
        case Bytecodes::_fast_aputfield: __ push_ptr(); offs+= Interpreter::stackElementSize; break;
        case Bytecodes::_fast_iputfield: // Fall through
        case Bytecodes::_fast_bputfield: // Fall through
        case Bytecodes::_fast_zputfield: // Fall through
        case Bytecodes::_fast_cputfield: // Fall through
        case Bytecodes::_fast_sputfield: __ push_i(); offs+=  Interpreter::stackElementSize; break;
        case Bytecodes::_fast_lputfield: __ push_l(); offs+=2*Interpreter::stackElementSize; break;
        case Bytecodes::_fast_fputfield: __ push_f(); offs+=  Interpreter::stackElementSize; break;
        case Bytecodes::_fast_dputfield: __ push_d(); offs+=2*Interpreter::stackElementSize; break;
        default: {
          offs = 0;
          base = Robj;
          const Register Rflags = Robj;
          Label is_one_slot;
          // Life is harder. The stack holds the value on top, followed by the
          // object. We don't know the size of the value, though; it could be
          // one or two words depending on its type. As a result, we must find
          // the type to determine where the object is.
          __ ld_PPC(Rflags, in_bytes(ConstantPoolCacheEntry::flags_offset()), Rcache); // Big Endian
          __ rldicl_PPC(Rflags, Rflags, 64-ConstantPoolCacheEntry::tos_state_shift, 64-ConstantPoolCacheEntry::tos_state_bits);

          __ cmpwi_PPC(CCR0, Rflags, ltos);
          __ cmpwi_PPC(CCR1, Rflags, dtos);
          __ addi_PPC(base, R23_esp, Interpreter::expr_offset_in_bytes(1));
          __ crnor_PPC(CCR0, Assembler::equal, CCR1, Assembler::equal);
          __ beq_PPC(CCR0, is_one_slot);
          __ addi_PPC(base, R23_esp, Interpreter::expr_offset_in_bytes(2));
          __ bind(is_one_slot);
          break;
        }
      }
      __ ld_PPC(Robj, offs, base);
      __ verify_oop(Robj);
    }

    __ addi_PPC(R6_ARG4_PPC, R23_esp, Interpreter::expr_offset_in_bytes(0));
    __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::post_field_modification), Robj, Rcache, R6_ARG4_PPC);
    __ get_cache_and_index_at_bcp(Rcache, 1);

    // In case of the fast versions, value lives in registers => put it back on tos.
    switch(bytecode()) {
      case Bytecodes::_fast_aputfield: __ pop_ptr(); break;
      case Bytecodes::_fast_iputfield: // Fall through
      case Bytecodes::_fast_bputfield: // Fall through
      case Bytecodes::_fast_zputfield: // Fall through
      case Bytecodes::_fast_cputfield: // Fall through
      case Bytecodes::_fast_sputfield: __ pop_i(); break;
      case Bytecodes::_fast_lputfield: __ pop_l(); break;
      case Bytecodes::_fast_fputfield: __ pop_f(); break;
      case Bytecodes::_fast_dputfield: __ pop_d(); break;
      default: break; // Nothin' to do.
    }

    __ align(32, 12);
    __ bind(Lno_field_mod_post);
  }
}

// RISCV64: implement volatile stores as release-store (return bytecode contains an additional release).
void TemplateTable::putfield_or_static(int byte_no, bool is_static, RewriteControl rc) {
  Label Lvolatile;

  const Register Rcache        = R13_ARG3,  // Do not use ARG1/2 (causes trouble in jvmti_post_field_mod).
                 Rclass_or_obj = R31_TMP6,  // Needs to survive C call.
                 Roffset       = R7_TMP2,   // Needs to survive C call.
                 Rflags        = R11_ARG1,
                 Rbtable       = R12_ARG2,
                 Rscratch      = R6_scratch2,
                 Rscratch2     = R5_scratch1,
                 Rscratch3     = R14_ARG4,
                 Rbc           = Rscratch3;

  static address field_rw_branch_table[number_of_states],
                 field_norw_branch_table[number_of_states],
                 static_branch_table[number_of_states];

  address* branch_table = is_static ? static_branch_table :
    (rc == may_rewrite ? field_rw_branch_table : field_norw_branch_table);

  // Stack (grows up):
  //  value
  //  obj

  // Load the field offset.
  resolve_cache_and_index(byte_no, Rcache, Rscratch, sizeof(u2));
  //jvmti_post_field_mod(Rcache, Rscratch, is_static); //FIXME_RISCV
  load_field_cp_cache_entry(Rclass_or_obj, Rcache, noreg, Roffset, Rflags, is_static);

  // Load pointer to branch table.
  __ li(Rbtable, (address)branch_table);

  // Get volatile flag.
  __ srli(Rscratch, Rflags, ConstantPoolCacheEntry::is_volatile_shift);
  __ andi(Rscratch, Rscratch, 1); // Extract volatile bit.

  // Check the field type.
  __ srli(Rflags, Rflags, ConstantPoolCacheEntry::tos_state_shift);
  __ andi(Rflags, Rflags, (1 << ConstantPoolCacheEntry::tos_state_bits) - 1);

#ifdef ASSERT
  // FIXME_RISCV
  Label LFlagInvalid;
//  __ addi(Rscratch2, R0_ZERO, number_of_states);
//  __ bge(Rflags, Rscratch2, LFlagInvalid);
#endif

  // Load from branch table and dispatch (volatile case: one instruction ahead).
  __ slli(Rflags, Rflags, LogBytesPerWord);
  if (!support_IRIW_for_not_multiple_copy_atomic_cpu) {
    // FIXME_RISCV
    //__ cmpwi_PPC(CR_is_vol, Rscratch, 1);  // Volatile?
  }
  __ slli(Rscratch, Rscratch, exact_log2(BytesPerInstWord)); // Volatile ? size of 1 instruction : 0.
  __ add(Rbtable, Rbtable, Rflags);
  __ ld(Rbtable, Rbtable, 0);

  __ sub(Rbtable, Rbtable, Rscratch); // Point to volatile/non-volatile entry point.
  __ jr(Rbtable);

#ifdef ASSERT
  __ bind(LFlagInvalid);
  __ stop("got invalid flag", 0x656);

  // __ bind(Lvtos);
  address pc_before_release = __ pc();
  __ release(); // Volatile entry point (one instruction before non-volatile_entry point).
  assert(__ pc() - pc_before_release == (ptrdiff_t)BytesPerInstWord, "must be single instruction");
  assert(branch_table[vtos] == 0, "can't compute twice");
  branch_table[vtos] = __ pc(); // non-volatile_entry point
  __ stop("vtos unexpected", 0x657);
#endif

  __ align(32, 28, 28); // Align pop.
  // __ bind(Ldtos);
  __ release(); // Volatile entry point (one instruction before non-volatile_entry point).
  assert(branch_table[dtos] == 0, "can't compute twice");
  branch_table[dtos] = __ pc(); // non-volatile_entry point
  __ pop(dtos);
  if (!is_static) {
    pop_and_check_object(Rclass_or_obj);  // Kills R5_scratch1.
  }
  __ add(Rclass_or_obj, Rclass_or_obj, Roffset);
  __ fsd(F23_ftos, Rclass_or_obj, 0);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_dputfield, Rbc, Rscratch, true, byte_no);
  }
  __ bnez(Rscratch, Lvolatile); // Volatile?
  __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

  __ align(32, 28, 28); // Align pop.
  // __ bind(Lftos);
  __ release(); // Volatile entry point (one instruction before non-volatile_entry point).
  assert(branch_table[ftos] == 0, "can't compute twice");
  branch_table[ftos] = __ pc(); // non-volatile_entry point
  __ pop(ftos);
  if (!is_static) { pop_and_check_object(Rclass_or_obj); } // Kills R5_scratch1.
  __ add(Rclass_or_obj, Rclass_or_obj, Roffset);
  __ fsw(F23_ftos, Rclass_or_obj, 0);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_fputfield, Rbc, Rscratch, true, byte_no);
  }
  __ bnez(Rscratch, Lvolatile); // Volatile?
  __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

  __ align(32, 28, 28); // Align pop.
  // __ bind(Litos);
  __ release(); // Volatile entry point (one instruction before non-volatile_entry point).
  assert(branch_table[itos] == 0, "can't compute twice");
  branch_table[itos] = __ pc(); // non-volatile_entry point
  __ pop(itos);
  if (!is_static) { pop_and_check_object(Rclass_or_obj); } // Kills R5_scratch1.
  __ add(Rclass_or_obj, Rclass_or_obj, Roffset);
  __ sw(R25_tos, Rclass_or_obj, 0);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_iputfield, Rbc, Rscratch, true, byte_no);
  }
  __ bnez(Rscratch, Lvolatile); // Volatile?
  __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

  __ align(32, 28, 28); // Align pop.
  // __ bind(Lltos);
  __ release(); // Volatile entry point (one instruction before non-volatile_entry point).
  assert(branch_table[ltos] == 0, "can't compute twice");
  branch_table[ltos] = __ pc(); // non-volatile_entry point
  __ pop(ltos);
  if (!is_static) { pop_and_check_object(Rclass_or_obj); } // Kills R5_scratch1.
  __ add(Rclass_or_obj, Rclass_or_obj, Roffset);
  __ sd(R25_tos, Rclass_or_obj, 0);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_lputfield, Rbc, Rscratch, true, byte_no);
  }
  __ bnez(Rscratch, Lvolatile); // Volatile?
  __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

  __ align(32, 28, 28); // Align pop.
  // __ bind(Lbtos);
  __ release(); // Volatile entry point (one instruction before non-volatile_entry point).
  assert(branch_table[btos] == 0, "can't compute twice");
  branch_table[btos] = __ pc(); // non-volatile_entry point
  __ pop(btos);
  if (!is_static) { pop_and_check_object(Rclass_or_obj); } // Kills R5_scratch1.
  __ add(Rclass_or_obj, Rclass_or_obj, Roffset);
  __ sb(R25_tos, Rclass_or_obj, 0);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_bputfield, Rbc, Rscratch, true, byte_no);
  }
  __ bnez(Rscratch, Lvolatile); // Volatile?
  __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

  __ align(32, 28, 28); // Align pop.
  // __ bind(Lztos);
  __ release(); // Volatile entry point (one instruction before non-volatile_entry point).
  assert(branch_table[ztos] == 0, "can't compute twice");
  branch_table[ztos] = __ pc(); // non-volatile_entry point
  __ pop(ztos);
  if (!is_static) { pop_and_check_object(Rclass_or_obj); } // Kills R5_scratch1.
  __ andi(R25_tos, R25_tos, 0x1);
  __ add(Rclass_or_obj, Rclass_or_obj, Roffset);
  __ sb(R25_tos, Rclass_or_obj, 0);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_zputfield, Rbc, Rscratch, true, byte_no);
  }
  __ bnez(Rscratch, Lvolatile); // Volatile?
  __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

  __ align(32, 28, 28); // Align pop.
  // __ bind(Lctos);
  __ release(); // Volatile entry point (one instruction before non-volatile_entry point).
  assert(branch_table[ctos] == 0, "can't compute twice");
  branch_table[ctos] = __ pc(); // non-volatile_entry point
  __ pop(ctos);
  if (!is_static) { pop_and_check_object(Rclass_or_obj); } // Kills R5_scratch1..
  __ add(Rclass_or_obj, Rclass_or_obj, Roffset);
  __ sh(R25_tos, Rclass_or_obj, 0);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_cputfield, Rbc, Rscratch, true, byte_no);
  }
  __ bnez(Rscratch, Lvolatile); // Volatile?
  __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

  __ align(32, 28, 28); // Align pop.
  // __ bind(Lstos);
  __ release(); // Volatile entry point (one instruction before non-volatile_entry point).
  assert(branch_table[stos] == 0, "can't compute twice");
  branch_table[stos] = __ pc(); // non-volatile_entry point
  __ pop(stos);
  if (!is_static) { pop_and_check_object(Rclass_or_obj); } // Kills R5_scratch1.
  __ add(Rclass_or_obj, Rclass_or_obj, Roffset);
  __ sh(R25_tos, Rclass_or_obj, 0);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_sputfield, Rbc, Rscratch, true, byte_no);
  }
  __ bnez(Rscratch, Lvolatile); // Volatile?
  __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

  __ align(32, 28, 28); // Align pop.
  // __ bind(Latos);
  __ release(); // Volatile entry point (one instruction before non-volatile_entry point).
  assert(branch_table[atos] == 0, "can't compute twice");
  branch_table[atos] = __ pc(); // non-volatile_entry point
  __ pop(atos);
  if (!is_static) { pop_and_check_object(Rclass_or_obj); } // kills R5_scratch1
  do_oop_store(_masm, Rclass_or_obj, Roffset, R25_tos, Rscratch3, Rscratch2, Rscratch, IN_HEAP);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_aputfield, Rbc, Rscratch, true, byte_no);
  }

  __ bnez(Rscratch, Lvolatile); // Volatile?
  __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

  __ align(32, 12);
  __ bind(Lvolatile);
  __ fence(); //__ Assembler::fence(Assembler::W_OP, Assembler::R_OP);
#ifdef ASSERT
  for (int i = 0; i<number_of_states; ++i) {
    assert(branch_table[i], "put initialization");
    //tty->print_cr("put: %s_branch_table[%d] = 0x%llx (opcode 0x%llx)",
    //              is_static ? "static" : "field", i, branch_table[i], *((unsigned int*)branch_table[i]));
  }
#endif
}

void TemplateTable::putfield(int byte_no) {
  putfield_or_static(byte_no, false);
}

void TemplateTable::nofast_putfield(int byte_no) {
  tty->print_cr("nofast_putfield #%i: %p", byte_no, __ pc());
  putfield_or_static(byte_no, false, may_not_rewrite);
}

void TemplateTable::putstatic(int byte_no) {
  putfield_or_static(byte_no, true);
}

// See SPARC. On RISCV64, we have a different jvmti_post_field_mod which does the job.
void TemplateTable::jvmti_post_fast_field_mod() {
  __ should_not_reach_here();
}

void TemplateTable::fast_storefield(TosState state) {
  transition(state, vtos);

  const Register Rcache        = R5_ARG3_PPC,  // Do not use ARG1/2 (causes trouble in jvmti_post_field_mod).
                 Rclass_or_obj = R31,      // Needs to survive C call.
                 Roffset       = R22_tmp2_PPC, // Needs to survive C call.
                 Rflags        = R3_ARG1_PPC,
                 Rscratch      = R5_scratch1,
                 Rscratch2     = R6_scratch2,
                 Rscratch3     = R4_ARG2_PPC;
  const ConditionRegister CR_is_vol = CCR2; // Non-volatile condition register (survives runtime call in do_oop_store).

  // Constant pool already resolved => Load flags and offset of field.
  __ get_cache_and_index_at_bcp(Rcache, 1);
  jvmti_post_field_mod(Rcache, Rscratch, false /* not static */);
  load_field_cp_cache_entry(noreg, Rcache, noreg, Roffset, Rflags, false);

  // Get the obj and the final store addr.
  pop_and_check_object(Rclass_or_obj); // Kills R5_scratch1.

  // Get volatile flag.
  __ rldicl__PPC(Rscratch, Rflags, 64-ConstantPoolCacheEntry::is_volatile_shift, 63); // Extract volatile bit.
  if (!support_IRIW_for_not_multiple_copy_atomic_cpu) { __ cmpdi_PPC(CR_is_vol, Rscratch, 1); }
  {
    Label LnotVolatile;
    __ beq_PPC(CCR0, LnotVolatile);
    __ release();
    __ align(32, 12);
    __ bind(LnotVolatile);
  }

  // Do the store and fencing.
  switch(bytecode()) {
    case Bytecodes::_fast_aputfield:
      // Store into the field.
      do_oop_store(_masm, Rclass_or_obj, Roffset, R25_tos, Rscratch, Rscratch2, Rscratch3, IN_HEAP);
      break;

    case Bytecodes::_fast_iputfield:
      __ stwx_PPC(R25_tos, Rclass_or_obj, Roffset);
      break;

    case Bytecodes::_fast_lputfield:
      __ stdx_PPC(R25_tos, Rclass_or_obj, Roffset);
      break;

    case Bytecodes::_fast_zputfield:
      __ andi_PPC(R25_tos, R25_tos, 0x1);  // boolean is true if LSB is 1
      // fall through to bputfield
    case Bytecodes::_fast_bputfield:
      __ stbx_PPC(R25_tos, Rclass_or_obj, Roffset);
      break;

    case Bytecodes::_fast_cputfield:
    case Bytecodes::_fast_sputfield:
      __ sthx_PPC(R25_tos, Rclass_or_obj, Roffset);
      break;

    case Bytecodes::_fast_fputfield:
      __ stfsx_PPC(F23_ftos, Rclass_or_obj, Roffset);
      break;

    case Bytecodes::_fast_dputfield:
      __ stfdx_PPC(F23_ftos, Rclass_or_obj, Roffset);
      break;

    default: ShouldNotReachHere();
  }

  if (!support_IRIW_for_not_multiple_copy_atomic_cpu) {
    Label LVolatile;
    __ beq_PPC(CR_is_vol, LVolatile);
    __ dispatch_epilog(vtos, Bytecodes::length_for(bytecode()));

    __ align(32, 12);
    __ bind(LVolatile);
    __ fence();
  }
}

void TemplateTable::fast_accessfield(TosState state) {
  transition(atos, state);

  Label LisVolatile;
  ByteSize cp_base_offset = ConstantPoolCache::base_offset();

  const Register Rcache        = R3_ARG1_PPC,
                 Rclass_or_obj = R25_tos,
                 Roffset       = R22_tmp2_PPC,
                 Rflags        = R23_tmp3_PPC,
                 Rscratch      = R6_scratch2;

  // Constant pool already resolved. Get the field offset.
  __ get_cache_and_index_at_bcp(Rcache, 1);
  load_field_cp_cache_entry(noreg, Rcache, noreg, Roffset, Rflags, false);

  // JVMTI support
  jvmti_post_field_access(Rcache, Rscratch, false, true);

  // Get the load address.
  __ null_check_throw(Rclass_or_obj, -1, Rscratch);

  // Get volatile flag.
  __ rldicl__PPC(Rscratch, Rflags, 64-ConstantPoolCacheEntry::is_volatile_shift, 63); // Extract volatile bit.
  __ bne_PPC(CCR0, LisVolatile);

  switch(bytecode()) {
    case Bytecodes::_fast_agetfield:
    {
      do_oop_load(_masm, Rclass_or_obj, Roffset, R25_tos, Rscratch, /* nv temp */ Rflags, IN_HEAP);
      __ verify_oop(R25_tos);
      __ dispatch_epilog(state, Bytecodes::length_for(bytecode()));

      __ bind(LisVolatile);
      if (support_IRIW_for_not_multiple_copy_atomic_cpu) { __ fence(); }
      do_oop_load(_masm, Rclass_or_obj, Roffset, R25_tos, Rscratch, /* nv temp */ Rflags, IN_HEAP);
      __ verify_oop(R25_tos);
      __ twi_0_PPC(R25_tos);
      __ isync_PPC();
      break;
    }
    case Bytecodes::_fast_igetfield:
    {
      __ lwax_PPC(R25_tos, Rclass_or_obj, Roffset);
      __ dispatch_epilog(state, Bytecodes::length_for(bytecode()));

      __ bind(LisVolatile);
      if (support_IRIW_for_not_multiple_copy_atomic_cpu) { __ fence(); }
      __ lwax_PPC(R25_tos, Rclass_or_obj, Roffset);
      __ twi_0_PPC(R25_tos);
      __ isync_PPC();
      break;
    }
    case Bytecodes::_fast_lgetfield:
    {
      __ ldx_PPC(R25_tos, Rclass_or_obj, Roffset);
      __ dispatch_epilog(state, Bytecodes::length_for(bytecode()));

      __ bind(LisVolatile);
      if (support_IRIW_for_not_multiple_copy_atomic_cpu) { __ fence(); }
      __ ldx_PPC(R25_tos, Rclass_or_obj, Roffset);
      __ twi_0_PPC(R25_tos);
      __ isync_PPC();
      break;
    }
    case Bytecodes::_fast_bgetfield:
    {
      __ lbzx_PPC(R25_tos, Rclass_or_obj, Roffset);
      __ extsb_PPC(R25_tos, R25_tos);
      __ dispatch_epilog(state, Bytecodes::length_for(bytecode()));

      __ bind(LisVolatile);
      if (support_IRIW_for_not_multiple_copy_atomic_cpu) { __ fence(); }
      __ lbzx_PPC(R25_tos, Rclass_or_obj, Roffset);
      __ twi_0_PPC(R25_tos);
      __ extsb_PPC(R25_tos, R25_tos);
      __ isync_PPC();
      break;
    }
    case Bytecodes::_fast_cgetfield:
    {
      __ lhzx_PPC(R25_tos, Rclass_or_obj, Roffset);
      __ dispatch_epilog(state, Bytecodes::length_for(bytecode()));

      __ bind(LisVolatile);
      if (support_IRIW_for_not_multiple_copy_atomic_cpu) { __ fence(); }
      __ lhzx_PPC(R25_tos, Rclass_or_obj, Roffset);
      __ twi_0_PPC(R25_tos);
      __ isync_PPC();
      break;
    }
    case Bytecodes::_fast_sgetfield:
    {
      __ lhax_PPC(R25_tos, Rclass_or_obj, Roffset);
      __ dispatch_epilog(state, Bytecodes::length_for(bytecode()));

      __ bind(LisVolatile);
      if (support_IRIW_for_not_multiple_copy_atomic_cpu) { __ fence(); }
      __ lhax_PPC(R25_tos, Rclass_or_obj, Roffset);
      __ twi_0_PPC(R25_tos);
      __ isync_PPC();
      break;
    }
    case Bytecodes::_fast_fgetfield:
    {
      __ lfsx_PPC(F23_ftos, Rclass_or_obj, Roffset);
      __ dispatch_epilog(state, Bytecodes::length_for(bytecode()));

      __ bind(LisVolatile);
      Label Ldummy;
      if (support_IRIW_for_not_multiple_copy_atomic_cpu) { __ fence(); }
      __ lfsx_PPC(F23_ftos, Rclass_or_obj, Roffset);
      __ fcmpu_PPC(CCR0, F23_ftos, F23_ftos); // Acquire by cmp-br-isync.
      __ bne_predict_not_taken_PPC(CCR0, Ldummy);
      __ bind(Ldummy);
      __ isync_PPC();
      break;
    }
    case Bytecodes::_fast_dgetfield:
    {
      __ lfdx_PPC(F23_ftos, Rclass_or_obj, Roffset);
      __ dispatch_epilog(state, Bytecodes::length_for(bytecode()));

      __ bind(LisVolatile);
      Label Ldummy;
      if (support_IRIW_for_not_multiple_copy_atomic_cpu) { __ fence(); }
      __ lfdx_PPC(F23_ftos, Rclass_or_obj, Roffset);
      __ fcmpu_PPC(CCR0, F23_ftos, F23_ftos); // Acquire by cmp-br-isync.
      __ bne_predict_not_taken_PPC(CCR0, Ldummy);
      __ bind(Ldummy);
      __ isync_PPC();
      break;
    }
    default: ShouldNotReachHere();
  }
}

void TemplateTable::fast_xaccess(TosState state) {
  transition(vtos, state);

  Label LisVolatile;
  ByteSize cp_base_offset = ConstantPoolCache::base_offset();
  const Register Rcache        = R3_ARG1_PPC,
                 Rclass_or_obj = R25_tos,
                 Roffset       = R22_tmp2_PPC,
                 Rflags        = R23_tmp3_PPC,
                 Rscratch      = R6_scratch2;

  __ ld_PPC(Rclass_or_obj, 0, R26_locals);

  // Constant pool already resolved. Get the field offset.
  __ get_cache_and_index_at_bcp(Rcache, 2);
  load_field_cp_cache_entry(noreg, Rcache, noreg, Roffset, Rflags, false);

  // JVMTI support not needed, since we switch back to single bytecode as soon as debugger attaches.

  // Needed to report exception at the correct bcp.
  __ addi_PPC(R22_bcp, R22_bcp, 1);

  // Get the load address.
  __ null_check_throw(Rclass_or_obj, -1, Rscratch);

  // Get volatile flag.
  __ rldicl__PPC(Rscratch, Rflags, 64-ConstantPoolCacheEntry::is_volatile_shift, 63); // Extract volatile bit.
  __ bne_PPC(CCR0, LisVolatile);

  switch(state) {
  case atos:
    {
      do_oop_load(_masm, Rclass_or_obj, Roffset, R25_tos, Rscratch, /* nv temp */ Rflags, IN_HEAP);
      __ verify_oop(R25_tos);
      __ dispatch_epilog(state, Bytecodes::length_for(bytecode()) - 1); // Undo bcp increment.

      __ bind(LisVolatile);
      if (support_IRIW_for_not_multiple_copy_atomic_cpu) { __ fence(); }
      do_oop_load(_masm, Rclass_or_obj, Roffset, R25_tos, Rscratch, /* nv temp */ Rflags, IN_HEAP);
      __ verify_oop(R25_tos);
      __ twi_0_PPC(R25_tos);
      __ isync_PPC();
      break;
    }
  case itos:
    {
      __ lwax_PPC(R25_tos, Rclass_or_obj, Roffset);
      __ dispatch_epilog(state, Bytecodes::length_for(bytecode()) - 1); // Undo bcp increment.

      __ bind(LisVolatile);
      if (support_IRIW_for_not_multiple_copy_atomic_cpu) { __ fence(); }
      __ lwax_PPC(R25_tos, Rclass_or_obj, Roffset);
      __ twi_0_PPC(R25_tos);
      __ isync_PPC();
      break;
    }
  case ftos:
    {
      __ lfsx_PPC(F23_ftos, Rclass_or_obj, Roffset);
      __ dispatch_epilog(state, Bytecodes::length_for(bytecode()) - 1); // Undo bcp increment.

      __ bind(LisVolatile);
      Label Ldummy;
      if (support_IRIW_for_not_multiple_copy_atomic_cpu) { __ fence(); }
      __ lfsx_PPC(F23_ftos, Rclass_or_obj, Roffset);
      __ fcmpu_PPC(CCR0, F23_ftos, F23_ftos); // Acquire by cmp-br-isync.
      __ bne_predict_not_taken_PPC(CCR0, Ldummy);
      __ bind(Ldummy);
      __ isync_PPC();
      break;
    }
  default: ShouldNotReachHere();
  }
  __ addi_PPC(R22_bcp, R22_bcp, -1);
}

// ============================================================================
// Calls

// Common code for invoke
//
// Input:
//   - byte_no
//
// Output:
//   - Rmethod:        The method to invoke next or i-klass (invokeinterface).
//   - Rret_addr:      The return address to return to.
//   - Rindex:         MethodType (invokehandle), CallSite obj (invokedynamic) or Method (invokeinterface)
//   - Rrecv:          Cache for "this" pointer, might be noreg if static call.
//   - Rflags:         Method flags from const pool cache.
//
//  Kills:
//   - Rscratch1
//
void TemplateTable::prepare_invoke(int byte_no,
                                   Register Rmethod,  // linked method (or i-klass)
                                   Register Rret_addr,// return address
                                   Register Rindex,   // itable index, MethodType, Method, etc.
                                   Register Rrecv,    // If caller wants to see it.
                                   Register Rflags,   // If caller wants to test it.
                                   Register Rscratch
                                   ) {
  // Determine flags.
  const Bytecodes::Code code = bytecode();
  const bool is_invokeinterface  = code == Bytecodes::_invokeinterface;
  const bool is_invokedynamic    = code == Bytecodes::_invokedynamic;
  const bool is_invokehandle     = code == Bytecodes::_invokehandle;
  const bool is_invokevirtual    = code == Bytecodes::_invokevirtual;
  const bool is_invokespecial    = code == Bytecodes::_invokespecial;
  const bool load_receiver       = (Rrecv != noreg);
  assert(load_receiver == (code != Bytecodes::_invokestatic && code != Bytecodes::_invokedynamic), "");

  assert_different_registers(Rmethod, Rindex, Rflags, Rscratch);
  assert_different_registers(Rmethod, Rrecv, Rflags, Rscratch);
  assert_different_registers(Rret_addr, Rscratch);

  load_invoke_cp_cache_entry(byte_no, Rmethod, Rindex, Rflags, is_invokevirtual, false, is_invokedynamic);

  // Saving of SP done in call_from_interpreter.

  // Maybe push "appendix" to arguments.
  if (is_invokedynamic || is_invokehandle) {
    __ unimplemented("unimplemented part of TemplateTable::prepare_invoke");
    Label Ldone;
    __ rldicl__PPC(R0, Rflags, 64-ConstantPoolCacheEntry::has_appendix_shift, 63);
    __ beq_PPC(CCR0, Ldone);
    // Push "appendix" (MethodType, CallSite, etc.).
    // This must be done before we get the receiver,
    // since the parameter_size includes it.
    __ load_resolved_reference_at_index(Rscratch, Rindex);
    __ verify_oop(Rscratch);
    __ push_ptr(Rscratch);
    __ bind(Ldone);
  }

  // Load receiver if needed (after appendix is pushed so parameter size is correct).
  if (load_receiver) {
  //  __ unimplemented("unimplemented part of TemplateTable::prepare_invoke");
    const Register Rparam_count = Rscratch;
    __ andi(Rparam_count, Rflags, ConstantPoolCacheEntry::parameter_size_mask);

    __ load_receiver(Rparam_count, Rrecv);

    __ verify_oop(Rrecv);
  }

  // Get return address.
  {
    Register Rtable_addr = Rscratch;
    Register Rret_type = Rret_addr;
    address table_addr = (address) Interpreter::invoke_return_entry_table_for(code);

    // Get return type. It's coded into the upper 4 bits of the lower half of the 64 bit value.
    __ srli(Rret_type, Rflags, ConstantPoolCacheEntry::tos_state_shift);
    __ andi(Rret_type, Rret_type, ConstantPoolCacheEntry::tos_state_mask);
    __ load_dispatch_table(Rtable_addr, (address*)table_addr);

    __ slli(Rret_type, Rret_type, LogBytesPerWord);
    // Get return address.
    __ add(Rtable_addr, Rtable_addr, Rret_type);

    __ ld(Rret_addr, Rtable_addr);
  }
}

// Helper for virtual calls. Load target out of vtable and jump off!
// Kills all passed registers.
void TemplateTable::generate_vtable_call(Register Rrecv_klass, Register Rindex, Register Rret, Register Rtemp) {

  assert_different_registers(Rrecv_klass, Rtemp, Rret);
  const Register Rtarget_method = Rindex;

  // Get target method & entry point.
  const int base = in_bytes(Klass::vtable_start_offset());
  // Calc vtable addr scale the vtable index by 8.
  __ slli(Rindex, Rindex, exact_log2(vtableEntry::size_in_bytes()));
  // Load target.
  assert(Assembler::is_simm12(base + vtableEntry::method_offset_in_bytes()), "Argument should be small");
  __ addi(Rrecv_klass, Rrecv_klass, base + vtableEntry::method_offset_in_bytes());

  __ ld(Rtarget_method, Rindex, Rrecv_klass);
  // Argument and return type profiling.
  __ profile_arguments_type(Rtarget_method, Rrecv_klass /* scratch1 */, Rtemp /* scratch2 */, true);

  __ call_from_interpreter(Rtarget_method, Rret, Rrecv_klass /* scratch1 */, Rtemp /* scratch2 */);
}

// Virtual or final call. Final calls are rewritten on the fly to run through "fast_finalcall" next time.
void TemplateTable::invokevirtual(int byte_no) {
  transition(vtos, vtos);

  Register Rtable_addr = R5_scratch1,
           Rret_type = R6_scratch2,
           Rret_addr = R12_ARG2,
           Rflags = R7_TMP2,
           Rrecv = R10_ARG0,
           Rrecv_klass = Rrecv,
           Rvtableindex_or_method = R28_TMP3,
           Rnum_params = R11_ARG1,
           Rnew_bc = R13_ARG3,
           Rtmp2 = AS_REGISTER(Register, R6); //R7_scratch3;

  Label LnotFinal;
  assert_different_registers(Rtable_addr, Rret_type);

  load_invoke_cp_cache_entry(byte_no, Rvtableindex_or_method, noreg, Rflags, /*virtual*/ true, false, false);

  __ li(Rtmp2, 1 << ConstantPoolCacheEntry::is_vfinal_shift);
  __ andr(Rtmp2, Rflags, Rtmp2);
  __ beqz(Rtmp2, LnotFinal);

  
  if (RewriteBytecodes && !UseSharedSpaces && !DumpSharedSpaces) {
	  __ unimplemented("invokevirtual - patch bytecode");
	  // TODO make Rflags and Rvtableindex_or_method nonvolatile or save it somewhere
	  assert(Rflags->is_nonvolatile(), "Rflags should be nonvlolatile");
	  assert(Rvtableindex_or_method->is_nonvolatile(), "Rvtableindex_or_method should be nonvlolatile");
    patch_bytecode(Bytecodes::_fast_invokevfinal, Rnew_bc, R6_scratch2);
  }

  //__ j(LnotFinal); // fixme riscv
  invokevfinal_helper(Rvtableindex_or_method, Rflags, R5_scratch1, R6_scratch2);

  __ align(32, 12);
  __ bind(LnotFinal);

  // Load "this" pointer (receiver).
  __ andi(Rnum_params, Rflags, ConstantPoolCacheEntry::parameter_size_mask);
  __ load_receiver(Rnum_params, Rrecv);
  __ verify_oop(Rrecv);

  // Get return type. It's coded into the upper 4 bits of the lower half of the 64 bit value.
  __ srli(Rret_type,  Rflags, ConstantPoolCacheEntry::tos_state_shift);
  __ andi(Rret_type,  Rret_type, ConstantPoolCacheEntry::tos_state_mask);
  __ slli(Rret_type, Rret_type, LogBytesPerWord);
  __ load_dispatch_table(Rtable_addr, Interpreter::invoke_return_entry_table());
  __ ld(Rret_addr, Rret_type, Rtable_addr);

  __ null_check_throw(Rrecv, oopDesc::klass_offset_in_bytes(), R5_scratch1);
  __ load_klass(Rrecv_klass, Rrecv);
  __ verify_klass_ptr(Rrecv_klass);
  __ profile_virtual_call(Rrecv_klass, R5_scratch1, R6_scratch2, false);

  generate_vtable_call(Rrecv_klass, Rvtableindex_or_method, Rret_addr, R5_scratch1);
}

void TemplateTable::fast_invokevfinal(int byte_no) {
  transition(vtos, vtos);

  assert(byte_no == f2_byte, "use this argument");
  Register Rflags  = R7_TMP2;
  load_invoke_cp_cache_entry(byte_no, R27_method, noreg, Rflags, /*virtual*/ true, /*is_invokevfinal*/ true, false);
  invokevfinal_helper(R27_method, Rflags, R5_scratch1, R6_scratch2);
}

void TemplateTable::invokevfinal_helper(Register Rmethod, Register Rflags, Register Rscratch1, Register Rscratch2) {

  assert_different_registers(Rmethod, Rflags, Rscratch1, Rscratch2);

  // Load receiver from stack slot.
  Register Rrecv = Rscratch2;
  Register Rnum_params = Rrecv;

  /*
  // Load receiver if needed (after appendix is pushed so parameter size is correct).
  if (load_receiver) {
  //  __ unimplemented("unimplemented part of TemplateTable::prepare_invoke");
    const Register Rparam_count = Rscratch;
    __ andi(Rparam_count, Rflags, ConstantPoolCacheEntry::parameter_size_mask);
    printf("prepare_invoke-4.1: %p\n", __ pc());

    __ load_receiver(Rparam_count, Rrecv);
     printf("prepare_invoke-4.2: %p\n", __ pc());

    __ verify_oop(Rrecv);
  }

  // Get return address.
  {
    Register Rtable_addr = Rscratch;
    Register Rret_type = Rret_addr;
    address table_addr = (address) Interpreter::invoke_return_entry_table_for(code);

    // Get return type. It's coded into the upper 4 bits of the lower half of the 64 bit value.
    __ srli(Rret_type, Rflags, ConstantPoolCacheEntry::tos_state_shift);
    __ andi(Rret_type, Rret_type, ConstantPoolCacheEntry::tos_state_mask);
    __ load_dispatch_table(Rtable_addr, (address*)table_addr);
    __ slli(Rret_type, Rret_type, LogBytesPerWord);
    // Get return address.
    __ add(Rtable_addr, Rtable_addr, Rret_type);
    __ ld(Rret_addr, Rtable_addr);
  }
}
   */

  __ ld(Rnum_params, Rmethod, in_bytes(Method::const_offset()));
  __ lhu(Rnum_params /* number of params */, Rnum_params, in_bytes(ConstMethod::size_of_parameters_offset()));

  // Get return address.
  Register Rtable_addr = Rscratch1,
           Rret_addr   = Rflags,
           Rret_type   = Rret_addr;
  // Get return type. It's coded into the upper 4 bits of the lower half of the 64 bit value.

  __ srli(Rret_type, Rflags, ConstantPoolCacheEntry::tos_state_shift); 
  __ andi(Rret_type, Rret_type, ConstantPoolCacheEntry::tos_state_mask);

  //__ rldicl_PPC(Rret_type, Rflags, 64-ConstantPoolCacheEntry::tos_state_shift, 64-ConstantPoolCacheEntry::tos_state_bits);
  __ load_dispatch_table(Rtable_addr, Interpreter::invoke_return_entry_table());

  __ slli(Rret_type, Rret_type, LogBytesPerWord);

  __ ld(Rret_addr, Rret_type, Rtable_addr);

  // Load receiver and receiver NULL check.
  __ load_receiver(Rnum_params, Rrecv);
 // __ null_check_throw(Rrecv, -1, Rscratch1); FixMe RISCV

//  __ profile_final_call(Rrecv, Rscratch1); FixMe RISCV
  // Argument and return type profiling.
//  __ profile_arguments_type(Rmethod, Rscratch1, Rscratch2, true);  FixMe RISCV

  // Do the call.
  __ call_from_interpreter(Rmethod, Rret_addr, Rscratch1, Rscratch2);
}

void TemplateTable::invokespecial(int byte_no) {
  assert(byte_no == f1_byte, "use this argument");
  transition(vtos, vtos);

  Register Rtable_addr = R10_ARG0,
           Rret_addr   = R11_ARG1,
           Rflags      = R12_ARG2,
           Rreceiver   = R13_ARG3;

  prepare_invoke(byte_no, R27_method, Rret_addr, noreg, Rreceiver, Rflags, R5_scratch1);

  // Receiver NULL check.
  //__ null_check_throw(Rreceiver, -1, R5_scratch1);

  //__ profile_call(R5_scratch1, R6_scratch2);
  // Argument and return type profiling.
  //__ profile_arguments_type(R27_method, R5_scratch1, R6_scratch2, false);
  __ call_from_interpreter(R27_method, Rret_addr, R5_scratch1, R6_scratch2);
}

void TemplateTable::invokestatic(int byte_no) {
  assert(byte_no == f1_byte, "use this argument");
  transition(vtos, vtos);

  Register Rtable_addr = R10_ARG0,
           Rret_addr   = R11_ARG1,
           Rflags      = R12_ARG2;

  prepare_invoke(byte_no, R27_method, Rret_addr, noreg, noreg, Rflags, R5_scratch1);

  //__ profile_call(R5_scratch1, R6_scratch2);
  // Argument and return type profiling.
  // FIXME_RISCV
  // __ profile_arguments_type(R27_method, R5_scratch1, R6_scratch2, false);
 
  __ call_from_interpreter(R27_method, Rret_addr, R5_scratch1, R6_scratch2);
}

void TemplateTable::invokeinterface_object_method(Register Rrecv_klass,
                                                  Register Rret,
                                                  Register Rflags,
                                                  Register Rmethod,
                                                  Register Rtemp1,
                                                  Register Rtemp2) {
  assert_different_registers(Rmethod, Rret, Rrecv_klass, Rflags, Rtemp1, Rtemp2);
  Label LnotFinal;

  // Check for vfinal.
  __ testbitdi_PPC(CCR0, R0, Rflags, ConstantPoolCacheEntry::is_vfinal_shift);
  __ bfalse_PPC(CCR0, LnotFinal);

  Register Rscratch = Rflags; // Rflags is dead now.

  // Final call case.
  __ profile_final_call(Rtemp1, Rscratch);
  // Argument and return type profiling.
  __ profile_arguments_type(Rmethod, Rscratch, Rrecv_klass /* scratch */, true);
  // Do the final call - the index (f2) contains the method.
  __ call_from_interpreter(Rmethod, Rret, Rscratch, Rrecv_klass /* scratch */);

  // Non-final callc case.
  __ bind(LnotFinal);
  __ profile_virtual_call(Rrecv_klass, Rtemp1, Rscratch, false);
  generate_vtable_call(Rrecv_klass, Rmethod, Rret, Rscratch);
}

void TemplateTable::invokeinterface(int byte_no) {
  assert(byte_no == f1_byte, "use this argument");
  transition(vtos, vtos);

  const Register Rscratch1        = R5_scratch1,
                 Rscratch2        = R6_scratch2,
                 Rmethod          = R13_ARG3,
                 Rmethod2         = R16_ARG6,
                 Rinterface_klass = R12_ARG2,
                 Rret_addr        = R15_ARG5,
                 Rindex           = R17_ARG7,
                 Rreceiver        = R10_ARG0,
                 Rrecv_klass      = R11_ARG1,
                 Rflags           = R14_ARG4;

  prepare_invoke(byte_no, Rinterface_klass, Rret_addr, Rmethod, Rreceiver, Rflags, Rscratch1);

  // First check for Object case, then private interface method,
  // then regular interface method.

  // Get receiver klass - this is also a null check
//  __ null_check_throw(Rreceiver, oopDesc::klass_offset_in_bytes(), Rscratch2);
  __ load_klass(Rrecv_klass, Rreceiver);

  // Check corner case object method.
  // Special case of invokeinterface called for virtual method of
  // java.lang.Object. See ConstantPoolCacheEntry::set_method() for details:
  // The invokeinterface was rewritten to a invokevirtual, hence we have
  // to handle this corner case.

  Label LnotObjectMethod, Lthrow_ame;

  __ li(Rscratch2, 1 << ConstantPoolCacheEntry::is_forced_virtual_shift);
  __ andr(Rscratch2, Rflags, Rscratch2); 
  __ beqz(Rscratch2, LnotObjectMethod);

//  __ testbitdi_PPC(CCR0, R0, Rflags, ConstantPoolCacheEntry::is_forced_virtual_shift);
//  __ bfalse_PPC(CCR0, LnotObjectMethod);

  invokeinterface_object_method(Rrecv_klass, Rret_addr, Rflags, Rmethod, Rscratch1, Rscratch2);
  __ bind(LnotObjectMethod);

  // Check for private method invocation - indicated by vfinal
  Label LnotVFinal, L_no_such_interface, L_subtype;

  __ li(Rscratch2, 1 << ConstantPoolCacheEntry::is_vfinal_shift);
  __ andr(Rscratch2, Rflags, Rscratch2);
  __ beqz(Rscratch2, LnotVFinal);

//  __ testbitdi_PPC(CCR0, R0, Rflags, ConstantPoolCacheEntry::is_vfinal_shift);
//  __ bfalse_PPC(CCR0, LnotVFinal);

  __ check_klass_subtype(Rrecv_klass, Rinterface_klass, Rscratch1, Rscratch2, L_subtype);
  // If we get here the typecheck failed
  __ b_PPC(L_no_such_interface);
  __ bind(L_subtype);

  // do the call

  Register Rscratch = Rflags; // Rflags is dead now.

  __ profile_final_call(Rscratch1, Rscratch);
  __ profile_arguments_type(Rmethod, Rscratch, Rrecv_klass /* scratch */, true);

  __ call_from_interpreter(Rmethod, Rret_addr, Rscratch, Rrecv_klass /* scratch */);

  __ bind(LnotVFinal);

  __ lookup_interface_method(Rrecv_klass, Rinterface_klass, noreg, noreg, Rscratch1, Rscratch2,
                             L_no_such_interface, /*return_method=*/false);

  __ profile_virtual_call(Rrecv_klass, Rscratch1, Rscratch2, false);

  // Find entry point to call.

  // Get declaring interface class from method

  __ load_method_holder(Rinterface_klass, Rmethod);

  // Get itable index from method
  //__ lwa_PPC(Rindex, in_bytes(Method::itable_index_offset()), Rmethod);

  __ lw(Rindex, Rmethod, in_bytes(Method::itable_index_offset()));

  __ sub(Rindex, R0_ZERO, Rindex);  
  __ addi(Rindex, Rindex, Method::itable_index_max);


//  __ subfic_PPC(Rindex, Rindex, Method::itable_index_max);

  __ lookup_interface_method(Rrecv_klass, Rinterface_klass, Rindex, Rmethod2, Rscratch1, Rscratch2,
                             L_no_such_interface);

   __ sub(Rmethod2, Rmethod2, 0);
//  __ cmpdi_PPC(CCR0, Rmethod2, 0);
  __ beqz(Rmethod2, Lthrow_ame);
  // Found entry. Jump off!
  // Argument and return type profiling.
 
  __ profile_arguments_type(Rmethod2, Rscratch1, Rscratch2, true);


  //__ profile_called_method(Rindex, Rscratch1);
  __ call_from_interpreter(Rmethod2, Rret_addr, Rscratch1, Rscratch2);

  // Vtable entry was NULL => Throw abstract method error.
  __ bind(Lthrow_ame);
  // Pass arguments for generating a verbose error message.
  call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::throw_AbstractMethodErrorVerbose),
          Rrecv_klass, Rmethod);

  // Interface was not found => Throw incompatible class change error.
  __ bind(L_no_such_interface);
  // Pass arguments for generating a verbose error message.
  call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::throw_IncompatibleClassChangeErrorVerbose),
          Rrecv_klass, Rinterface_klass);
  DEBUG_ONLY( __ should_not_reach_here(); )
}

void TemplateTable::invokedynamic(int byte_no) {
  transition(vtos, vtos);

  const Register Rret_addr = R3_ARG1_PPC,
                 Rflags    = R4_ARG2_PPC,
                 Rmethod   = R22_tmp2_PPC,
                 Rscratch1 = R5_scratch1,
                 Rscratch2 = R6_scratch2;

  prepare_invoke(byte_no, Rmethod, Rret_addr, Rscratch1, noreg, Rflags, Rscratch2);

  // Profile this call.
  __ profile_call(Rscratch1, Rscratch2);

  // Off we go. With the new method handles, we don't jump to a method handle
  // entry any more. Instead, we pushed an "appendix" in prepare invoke, which happens
  // to be the callsite object the bootstrap method returned. This is passed to a
  // "link" method which does the dispatch (Most likely just grabs the MH stored
  // inside the callsite and does an invokehandle).
  // Argument and return type profiling.
  __ profile_arguments_type(Rmethod, Rscratch1, Rscratch2, false);
  __ call_from_interpreter(Rmethod, Rret_addr, Rscratch1 /* scratch1 */, Rscratch2 /* scratch2 */);
}

void TemplateTable::invokehandle(int byte_no) {
  transition(vtos, vtos);

  const Register Rret_addr = R3_ARG1_PPC,
                 Rflags    = R4_ARG2_PPC,
                 Rrecv     = R5_ARG3_PPC,
                 Rmethod   = R22_tmp2_PPC,
                 Rscratch1 = R5_scratch1,
                 Rscratch2 = R6_scratch2;

  prepare_invoke(byte_no, Rmethod, Rret_addr, Rscratch1, Rrecv, Rflags, Rscratch2);
  __ verify_method_ptr(Rmethod);
  __ null_check_throw(Rrecv, -1, Rscratch2);

  __ profile_final_call(Rrecv, Rscratch1);

  // Still no call from handle => We call the method handle interpreter here.
  // Argument and return type profiling.
  __ profile_arguments_type(Rmethod, Rscratch1, Rscratch2, true);
  __ call_from_interpreter(Rmethod, Rret_addr, Rscratch1 /* scratch1 */, Rscratch2 /* scratch2 */);
}

// =============================================================================
// Allocation

// Puts allocated obj ref onto the expression stack.
void TemplateTable::_new() {
  transition(vtos, atos);

  Label Lslow_case,
        Ldone;

  const Register RallocatedObject = R25_tos,
                 RinstanceKlass   = R16_ARG6,
                 Rscratch         = R5_scratch1,
                 Roffset          = R15_ARG5,
                 Rinstance_size   = Roffset,
                 Rcpool           = R11_ARG1,
                 Rtags            = R10_ARG0,
                 Rindex           = R12_ARG2;

  // --------------------------------------------------------------------------
  // Check if fast case is possible.

  // Load pointers to const pool and const pool's tags array.
  __ get_cpool_and_tags(Rcpool, Rtags);
  // Load index of constant pool entry.
  __ get_2_byte_integer_at_bcp(1, Rindex, InterpreterMacroAssembler::Unsigned);

  // Note: compared to other architectures, RISCV's implementation always goes
  // to the slow path if TLAB is used and fails.
  if (false /*UseTLAB*/) { //FIXME_RISCV
    // Make sure the class we're about to instantiate has been resolved
    // This is done before loading instanceKlass to be consistent with the order
    // how Constant Pool is updated (see ConstantPoolCache::klass_at_put).
    __ addi_PPC(Rtags, Rtags, Array<u1>::base_offset_in_bytes());
    __ lbzx_PPC(Rtags, Rindex, Rtags);

    __ cmpdi_PPC(CCR0, Rtags, JVM_CONSTANT_Class);
    __ bne_PPC(CCR0, Lslow_case);

    // Get instanceKlass
    __ sldi_PPC(Roffset, Rindex, LogBytesPerWord);
    __ load_resolved_klass_at_offset(Rcpool, Roffset, RinstanceKlass);

    // Make sure klass is fully initialized and get instance_size.
    __ lbz_PPC(Rscratch, in_bytes(InstanceKlass::init_state_offset()), RinstanceKlass);
    __ lwz_PPC(Rinstance_size, in_bytes(Klass::layout_helper_offset()), RinstanceKlass);

    __ cmpdi_PPC(CCR1, Rscratch, InstanceKlass::fully_initialized);
    // Make sure klass does not have has_finalizer, or is abstract, or interface or java/lang/Class.
    __ andi__PPC(R0, Rinstance_size, Klass::_lh_instance_slow_path_bit); // slow path bit equals 0?

    __ crnand_PPC(CCR0, Assembler::equal, CCR1, Assembler::equal); // slow path bit set or not fully initialized?
    __ beq_PPC(CCR0, Lslow_case);

    // --------------------------------------------------------------------------
    // Fast case:
    // Allocate the instance.
    // 1) Try to allocate in the TLAB.
    // 2) If the above fails (or is not applicable), go to a slow case (creates a new TLAB, etc.).

    Register RoldTopValue = RallocatedObject; // Object will be allocated here if it fits.
    Register RnewTopValue = R6_ARG4_PPC;
    Register RendValue    = R7_ARG5_PPC;

    // Check if we can allocate in the TLAB.
    __ ld_PPC(RoldTopValue, in_bytes(JavaThread::tlab_top_offset()), R24_thread);
    __ ld_PPC(RendValue,    in_bytes(JavaThread::tlab_end_offset()), R24_thread);

    __ add_PPC(RnewTopValue, Rinstance_size, RoldTopValue);

    // If there is enough space, we do not CAS and do not clear.
    __ cmpld_PPC(CCR0, RnewTopValue, RendValue);
    __ bgt_PPC(CCR0, Lslow_case);

    __ std_PPC(RnewTopValue, in_bytes(JavaThread::tlab_top_offset()), R24_thread);

    if (!ZeroTLAB) {
      // --------------------------------------------------------------------------
      // Init1: Zero out newly allocated memory.
      // Initialize remaining object fields.
      Register Rbase = Rtags;
      __ addi_PPC(Rinstance_size, Rinstance_size, 7 - (int)sizeof(oopDesc));
      __ addi_PPC(Rbase, RallocatedObject, sizeof(oopDesc));
      __ srdi_PPC(Rinstance_size, Rinstance_size, 3);

      // Clear out object skipping header. Takes also care of the zero length case.
      __ clear_memory_doubleword(Rbase, Rinstance_size);
    }

    // --------------------------------------------------------------------------
    // Init2: Initialize the header: mark, klass
    // Init mark.
    if (UseBiasedLocking) {
      __ ld_PPC(Rscratch, in_bytes(Klass::prototype_header_offset()), RinstanceKlass);
    } else {
      __ load_const_optimized(Rscratch, markOopDesc::prototype(), R0);
    }
    __ std_PPC(Rscratch, oopDesc::mark_offset_in_bytes(), RallocatedObject);

    // Init klass.
    __ store_klass_gap(RallocatedObject);
    __ store_klass(RallocatedObject, RinstanceKlass, Rscratch); // klass (last for cms)

    // Check and trigger dtrace event.
    SkipIfEqualZero::skip_to_label_if_equal_zero(_masm, Rscratch, &DTraceAllocProbes, Ldone);
    __ push(atos);
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::dtrace_object_alloc));
    __ pop(atos);

    __ b_PPC(Ldone);
  }

  // --------------------------------------------------------------------------
  // slow case
  __ bind(Lslow_case);
  call_VM(R25_tos, CAST_FROM_FN_PTR(address, InterpreterRuntime::_new), Rcpool, Rindex);

  // continue
  __ bind(Ldone);

  // Must prevent reordering of stores for object initialization with stores that publish the new object.
  __ Assembler::fence(Assembler::W_OP, Assembler::W_OP);
}

void TemplateTable::newarray() {
  transition(itos, atos);

  __ lbu(R11_ARG1, R22_bcp, 1);
  __ addw(R12_ARG2, R0_ZERO, R25_tos);
  call_VM(R25_tos, CAST_FROM_FN_PTR(address, InterpreterRuntime::newarray), R11_ARG1, R12_ARG2 /* size */);

  // Must prevent reordering of stores for object initialization with stores that publish the new object.
  __ Assembler::fence(Assembler::W_OP, Assembler::W_OP);
}

void TemplateTable::anewarray() {
  transition(itos, atos);

  __ get_constant_pool(R11_ARG1);
  __ get_2_byte_integer_at_bcp(1, R12_ARG2, InterpreterMacroAssembler::Unsigned);
  __ addw(R13_ARG3, R0_ZERO, R25_tos); // size
  call_VM(R25_tos, CAST_FROM_FN_PTR(address, InterpreterRuntime::anewarray), R11_ARG1 /* pool */, R12_ARG2 /* index */, R13_ARG3 /* size */);

  // Must prevent reordering of stores for object initialization with stores that publish the new object.
  __ Assembler::fence(Assembler::W_OP, Assembler::W_OP);
}

// Allocate a multi dimensional array
void TemplateTable::multianewarray() {
  transition(vtos, atos);

  Register Rptr = R31; // Needs to survive C call.

  // Put ndims * wordSize into frame temp slot
  __ lbz_PPC(Rptr, 3, R22_bcp);
  __ sldi_PPC(Rptr, Rptr, Interpreter::logStackElementSize);
  // Esp points past last_dim, so set to R4 to first_dim address.
  __ add_PPC(R4, Rptr, R23_esp);
  call_VM(R25_tos, CAST_FROM_FN_PTR(address, InterpreterRuntime::multianewarray), R4 /* first_size_address */);
  // Pop all dimensions off the stack.
  __ add_PPC(R23_esp, Rptr, R23_esp);

  // Must prevent reordering of stores for object initialization with stores that publish the new object.
  __ membar(Assembler::StoreStore);
}

void TemplateTable::arraylength() {
  transition(atos, itos);

  __ verify_oop(R25_tos);
//  __ null_check_throw(R25_tos, arrayOopDesc::length_offset_in_bytes(), R5_scratch1);
  __ lw(R25_tos, R25_tos, arrayOopDesc::length_offset_in_bytes());
}

// ============================================================================
// Typechecks

void TemplateTable::checkcast() {
  transition(atos, atos);

  Label Ldone, Lis_null, Lquicked, Lresolved;
  Register Roffset         = R10_ARG0,
           RobjKlass       = R11_ARG1,
           RspecifiedKlass = R12_ARG2, // Generate_ClassCastException_verbose_handler will read value from this register.
           Rcpool          = R5_scratch1,
           Rtags           = R6_scratch2;

  // Null does not pass.
  __ beqz(R25_tos, Lis_null);

  // Get constant pool tag to find out if the bytecode has already been "quickened".
  __ get_cpool_and_tags(Rcpool, Rtags);

  __ get_2_byte_integer_at_bcp(1, Roffset, InterpreterMacroAssembler::Unsigned);

  __ addi(Rtags, Rtags, Array<u1>::base_offset_in_bytes());
  __ lbu(Rtags, Rtags, Roffset);

  __ li(R13_ARG3, JVM_CONSTANT_Class);
  __ sub(R13_ARG3, Rtags, R13_ARG3);
  __ beqz(R13_ARG3, Lquicked);

  // Call into the VM to "quicken" instanceof.
  __ push_ptr();  // for GC
  call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::quicken_io_cc));
 
  __ get_vm_result_2(RspecifiedKlass);
  __ pop_ptr();   // Restore receiver.

  __ j(Lresolved);

  // Extract target class from constant pool.
  __ bind(Lquicked);
//    __ unimplemented("unimplemented part of checkcast");

//  __ sldi_PPC(Roffset, Roffset, LogBytesPerWord);
  __ slli(Roffset, Roffset, LogBytesPerWord);

  __ load_resolved_klass_at_offset(Rcpool, Roffset, RspecifiedKlass);

  // Do the checkcast.
  __ bind(Lresolved);
  // Get value klass in RobjKlass.

      printf("checkcast-15: %p\n", __ pc());

  __ load_klass(RobjKlass, R25_tos);
  // Generate a fast subtype check. Branch to cast_ok if no failure. Return 0 if failure.

    printf("checkcast-16: %p\n", __ pc());

  __ gen_subtype_check(RobjKlass, RspecifiedKlass, /*3 temp regs*/ Roffset, Rcpool, Rtags, /*target if subtype*/ Ldone);

  // Not a subtype; so must throw exception
  // Target class oop is in register R6_ARG4_PPC == RspecifiedKlass by convention.
  __ load_dispatch_table(R5_scratch1, (address*)Interpreter::_throw_ClassCastException_entry);
  __ mtctr_PPC(R5_scratch1);
  __ bctr_PPC();

  // Profile the null case.
  __ align(32, 12);
  __ bind(Lis_null);
  __ profile_null_seen(R5_scratch1, Rtags); // Rtags used as scratch.

  __ align(32, 12);
  __ bind(Ldone);
}

// Output:
//   - tos == 0: Obj was null or not an instance of class.
//   - tos == 1: Obj was an instance of class.
void TemplateTable::instanceof() {
  transition(atos, itos);

  Label Ldone, Lis_null, Lquicked, Lresolved;
  Register Roffset         = R10_ARG0,
           RobjKlass       = R11_ARG1,
           RspecifiedKlass = R12_ARG2,
           Rcpool          = R5_scratch1,
           Rtags           = R6_scratch2;

  // Null does not pass.
 // __ cmpdi_PPC(CCR0, R25_tos, 0);
  __ beqz(R25_tos, Lis_null);

  // Get constant pool tag to find out if the bytecode has already been "quickened".
  __ get_cpool_and_tags(Rcpool, Rtags);

  __ get_2_byte_integer_at_bcp(1, Roffset, InterpreterMacroAssembler::Unsigned);

//  __ addi_PPC(Rtags, Rtags, Array<u1>::base_offset_in_bytes());
//  __ lbzx_PPC(Rtags, Rtags, Roffset);
  __ addi(Rtags, Rtags, Array<u1>::base_offset_in_bytes());
  __ lbu(Rtags, Rtags, Roffset);

    printf("instanceof-7: %p\n", __ pc());

//  __ cmpdi_PPC(CCR0, Rtags, JVM_CONSTANT_Class);
//  __ beq_PPC(CCR0, Lquicked);

    __ li(R13_ARG3, JVM_CONSTANT_Class);
    __ beq(R13_ARG3, Rtags, Lquicked);

//    __ unimplemented("instanceof: it is not JVM_CONSTANT_Class");

  // Call into the VM to "quicken" instanceof.
  __ push_ptr();  // for GC
  call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::quicken_io_cc));
  __ get_vm_result_2(RspecifiedKlass);
  __ pop_ptr();   // Restore receiver.
  __ j(Lresolved);

  // Extract target class from constant pool.
  __ bind(Lquicked);
//  __ sldi_PPC(Roffset, Roffset, LogBytesPerWord);
//  __ load_resolved_klass_at_offset(Rcpool, Roffset, RspecifiedKlass);

  __ slli(Roffset, Roffset, LogBytesPerWord);
  __ load_resolved_klass_at_offset(Rcpool, Roffset, RspecifiedKlass);


  // Do the checkcast.
  __ bind(Lresolved);
  // Get value klass in RobjKlass.
  __ load_klass(RobjKlass, R25_tos);

    printf("instanceof-17: %p\n", __ pc());


  // Generate a fast subtype check. Branch to cast_ok if no failure. Return 0 if failure.
//  __ li_PPC(R25_tos, 1);
  __ li(R25_tos, 1);

  // FIXME_RISCV use different registers
    __ gen_subtype_check(RobjKlass, RspecifiedKlass, /*3 temp regs*/ Roffset, Rcpool, Rtags, /*target if subtype*/ Ldone);
  __ li(R25_tos, 0l);

  if (ProfileInterpreter) {
    __ b_PPC(Ldone);
  }

  // Profile the null case.
  __ align(32, 12);
  __ bind(Lis_null);
  __ profile_null_seen(Rcpool, Rtags); // Rcpool and Rtags used as scratch.

  __ align(32, 12);
  __ bind(Ldone);
}

// =============================================================================
// Breakpoints

void TemplateTable::_breakpoint() {
  transition(vtos, vtos);

  // Get the unpatched byte code.
  __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::get_original_bytecode_at), R27_method, R22_bcp);
  __ mr_PPC(R31, R3_RET_PPC);

  // Post the breakpoint event.
  __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::_breakpoint), R27_method, R22_bcp);

  // Complete the execution of original bytecode.
  __ dispatch_Lbyte_code(vtos, R31, Interpreter::normal_table(vtos));
}

// =============================================================================
// Exceptions

void TemplateTable::athrow() {
  transition(atos, vtos);

  // Exception oop is in tos
  __ verify_oop(R25_tos);

  __ null_check_throw(R25_tos, -1, R5_scratch1);

  // Throw exception interpreter entry expects exception oop to be in R3.
  __ mr_PPC(R3_RET_PPC, R25_tos);
  __ load_dispatch_table(R5_scratch1, (address*)Interpreter::throw_exception_entry());
  __ mtctr_PPC(R5_scratch1);
  __ bctr_PPC();
}

// =============================================================================
// Synchronization
// Searches the basic object lock list on the stack for a free slot
// and uses it to lock the obect in tos.
//
// Recursive locking is enabled by exiting the search if the same
// object is already found in the list. Thus, a new basic lock obj lock
// is allocated "higher up" in the stack and thus is found first
// at next monitor exit.
void TemplateTable::monitorenter() {
  transition(atos, vtos);

  __ verify_oop(R25_tos);

  Register Rcurrent_obj      = R6_scratch2,
           Robj_to_lock      = R25_tos,
           Rfree_slot        = R10_ARG0,
           Rscratch1         = R5_scratch1,
           Rscratch2         = R11_ARG1,
           Rscratch3         = R12_ARG2,
           Rcurrent_obj_addr = R13_ARG3;
  const int entry_size = frame::interpreter_frame_monitor_size() * wordSize;

  // ------------------------------------------------------------------------------
  // Null pointer exception.
  //__ null_check_throw(Robj_to_lock, -1, R5_scratch1); FIXME_RISCV

  // Try to acquire a lock on the object.
  // Repeat until succeeded (i.e., until monitorenter returns true).

  // ------------------------------------------------------------------------------
  // Find a free slot in the monitor block.
  Label Lfound, Lexit, Lallocate_new;
  {
    Label Lloop, Lno_free_slot;
    Register Rlimit = Rscratch1;

    __ mv(Rfree_slot, R0_ZERO);
    // Set up search loop - start with topmost monitor.
    __ addi(Rcurrent_obj_addr, R18_monitor, BasicObjectLock::obj_offset_in_bytes());

    __ mv(Rlimit, R8_FP);
    __ addi(Rlimit, Rlimit, -(frame::frame_header_size + entry_size) + BasicObjectLock::obj_offset_in_bytes()); // Monitor base

    // Check if any slot is present => short cut to allocation if not.
    __ bgt(Rcurrent_obj_addr, Rlimit, Lallocate_new);

    // Pre-load topmost slot.
    __ ld(Rcurrent_obj, Rcurrent_obj_addr, 0);

    // The search loop.
    __ bind(Lloop);

    // Found free slot?
    __ bnez(Rcurrent_obj, Lno_free_slot);
    __ addi(Rfree_slot, Rcurrent_obj_addr, -BasicObjectLock::obj_offset_in_bytes());
    __ bind(Lno_free_slot);

    // Is this entry for same obj? If so, stop the search and take the found
    // free slot or allocate a new one to enable recursive locking.
    __ beq(Rcurrent_obj, Robj_to_lock, Lexit);

    // otherwise advance to next entry
    __ addi(Rcurrent_obj_addr, Rcurrent_obj_addr, entry_size);
    __ ld(Rcurrent_obj, Rcurrent_obj_addr, 0);

    // Check if last allocated BasicLockObj reached.
    __ bgt(Rcurrent_obj_addr, Rlimit, Lexit);
    // Next iteration if unchecked BasicObjectLocks exist on the stack.
    __ j(Lloop);
    __ bind(Lexit);
  }

  __ bnez(Rfree_slot, Lfound);

  __ bind(Lallocate_new);
  // We didn't find a free BasicObjLock => allocate one.
  {
    Label Lloop;
    Register Rcurrent_addr = Rscratch1;
    __ addi(R2_SP, R2_SP, -entry_size);
    __ addi(R23_esp, R23_esp, -entry_size);
    __ addi(R18_monitor, R18_monitor, -entry_size);
    __ mv(Rcurrent_addr, R2_SP);
    __ mv(Rfree_slot, R18_monitor);
    __ beq(Rcurrent_addr, Rfree_slot, Lfound);

    __ bind(Lloop);
    __ ld(Rscratch2, Rcurrent_addr, entry_size);
    __ sd(Rscratch2, Rcurrent_addr, 0);
    __ addi(Rcurrent_addr, Rcurrent_addr, wordSize);
    __ bne(Rcurrent_addr, Rfree_slot, Lloop);
  }

  // ------------------------------------------------------------------------------
  // We now have a slot to lock.
  __ bind(Lfound);

  // Increment bcp to point to the next bytecode, so exception handling for async. exceptions work correctly.
  // The object has already been poped from the stack, so the expression stack looks correct.
  __ addi(R22_bcp, R22_bcp, 1);

  __ sd(Robj_to_lock, Rfree_slot, BasicObjectLock::obj_offset_in_bytes());
  __ lock_object(Rfree_slot, Robj_to_lock);

  // Check if there's enough space on the stack for the monitors after locking.
  // This emits a single store.
  //__ generate_stack_overflow_check(0); FIXME_RISCV

  // The bcp has already been incremented. Just need to dispatch to next instruction.
  __ dispatch_next(vtos);
}

void TemplateTable::monitorexit() {
  transition(atos, vtos);
  __ verify_oop(R25_tos);

  Register Rcurrent_monitor  = R5_scratch1,
           Rcurrent_obj      = R6_scratch2,
           Robj_to_lock      = R25_tos,
           Rcurrent_obj_addr = R10_ARG0,
           Rlimit            = R11_ARG1;
  Label Lfound, Lillegal_monitor_state;
  const int entry_size = frame::interpreter_frame_monitor_size() * wordSize;

  // Find the corresponding slot in the monitors stack section.
  {
    Label Lloop;
    __ mv(Rlimit, R8_FP);
    __ addi(Rlimit, Rlimit, -(frame::frame_header_size + entry_size) + BasicObjectLock::obj_offset_in_bytes()); // Monitor base
    // Start with topmost monitor.
    __ addi(Rcurrent_obj_addr, R18_monitor, BasicObjectLock::obj_offset_in_bytes());
    __ ld(Rcurrent_obj, Rcurrent_obj_addr, 0);
    __ bgt(Rcurrent_obj_addr, Rlimit, Lillegal_monitor_state);

    __ bind(Lloop);
    // Is this entry for same obj?
    __ beq(Rcurrent_obj, Robj_to_lock, Lfound);

    // Check if last allocated BasicLockObj reached.
    __ addi(Rcurrent_obj_addr, Rcurrent_obj_addr, entry_size);
    __ ld(Rcurrent_obj, Rcurrent_obj_addr, 0);
    __ bgt(Rcurrent_obj_addr, Rlimit, Lillegal_monitor_state);
    __ j(Lloop);
  }

  // Fell through without finding the basic obj lock => throw up!
  __ bind(Lillegal_monitor_state);

Label Ldone;
__ j(Ldone);

  __ unimplemented("IllegalMonitorStateException");
  //call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::throw_illegal_monitor_state_exception)); FIXME_RISCV
  __ should_not_reach_here();

  __ align(32, 12);
  __ bind(Lfound);
  __ addi(Rcurrent_monitor, Rcurrent_obj_addr, -BasicObjectLock::obj_offset_in_bytes());
  __ unlock_object(Rcurrent_monitor);


__ bind(Ldone);
}

// ============================================================================
// Wide bytecodes

// Wide instructions. Simply redirects to the wide entry point for that instruction.
void TemplateTable::wide() {
  transition(vtos, vtos);

  const Register Rtable = R5_scratch1,
                 Rindex = R6_scratch2,
                 Rtmp   = R7_TMP2;

  __ lbu(Rindex, R22_bcp, 1);

  __ load_dispatch_table(Rtable, Interpreter::_wentry_point);

  __ slli(Rindex, Rindex, LogBytesPerWord);
  __ add(Rtable, Rtable, Rindex);
  __ ld(Rtmp, Rtable, 0);
  __ jr(Rtmp);
  // Note: the bcp increment step is part of the individual wide bytecode implementations.
}
