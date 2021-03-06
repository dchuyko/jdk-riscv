/*
 * Copyright (c) 1997, 2019, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012, 2014 SAP SE. All rights reserved.
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

#ifndef OS_CPU_LINUX_RISCV_ORDERACCESS_LINUX_RISCV_HPP
#define OS_CPU_LINUX_RISCV_ORDERACCESS_LINUX_RISCV_HPP

// Included in orderAccess.hpp header file.

//#ifndef RISCV64
//#error "OrderAccess currently only implemented for RISCV64"
//#endif

// Compiler version last used for testing: gcc 4.1.2
// Please update this information when this file changes

// Implementation of class OrderAccess.

//
// Machine barrier instructions:
//
// - sync            Two-way memory barrier, aka fence.
// - lwsync          orders  Store|Store,
//                            Load|Store,
//                            Load|Load,
//                   but not Store|Load
// - eieio           orders  Store|Store
// - isync           Invalidates speculatively executed instructions,
//                   but isync may complete before storage accesses
//                   associated with instructions preceding isync have
//                   been performed.
//
// Semantic barrier instructions:
// (as defined in orderAccess.hpp)
//
// - release         orders Store|Store,       (maps to lwsync)
//                           Load|Store
// - acquire         orders  Load|Store,       (maps to lwsync)
//                           Load|Load
// - fence           orders Store|Store,       (maps to sync)
//                           Load|Store,
//                           Load|Load,
//                          Store|Load
//

#define inlasm_fence(p, s)  __asm__ __volatile__ ("fence " #p "," #s : : : "memory");
#define inlasm_fencei()     __asm__ __volatile__ ("fence.i" : : : "memory");
// Use twi-isync for load_acquire (faster than lwsync).
//#define inlasm_acquire_reg(X) __asm__ __volatile__ ("twi 0,%0,0\n isync\n" : : "r" (X) : "memory");

inline void   OrderAccess::loadload()   { inlasm_fence(r, r); }
inline void   OrderAccess::storestore() { inlasm_fence(w, w); }
inline void   OrderAccess::loadstore()  { inlasm_fence(r, w); }
inline void   OrderAccess::storeload()  { inlasm_fence(w, r); }

inline void   OrderAccess::acquire()    { inlasm_fence(r, rw); }
inline void   OrderAccess::release()    { inlasm_fence(rw, w); }
inline void   OrderAccess::fence()      { inlasm_fence(rw, rw); }
inline void   OrderAccess::cross_modify_fence() {
  inlasm_fencei();
  inlasm_fence(r, r);
}

template<size_t byte_size>
struct OrderAccess::PlatformOrderedLoad<byte_size, X_ACQUIRE>
{
  template <typename T>
  T operator()(const volatile T* p) const {
    T t = Atomic::load(p);
    //inlasm_acquire_reg(t);
    acquire();
    return t;
  }
};

#undef inlasm_fence
#undef inlasm_fencei

#endif // OS_CPU_LINUX_RISCV_ORDERACCESS_LINUX_RISCV_HPP
