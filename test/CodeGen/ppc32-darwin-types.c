// REQUIRES: ppc32-registered-target
// RUN: %clang_cc1 -triple powerpc-apple-darwin8 -dM -E - %s | FileCheck %s
// CHECK: #define __PTRDIFF_TYPE__ int
