/**
REQUIRES: powerpc-registered-target
RUN: %clang -### -target powerpc-apple-darwin8 -c %s 2>&1 | FileCheck --check-prefix=CHECK-CC1 %s
RUN: %clang -### -target powerpc-apple-darwin9 -c %s 2>&1 | FileCheck --check-prefix=CHECK-CC1 %s

RUN: %clang_cc1 -emit-llvm-only -fdump-record-layouts -triple powerpc-apple-darwin8 -malign-power %s 2> /dev/null | \
RUN: grep -v Comment | \
RUN: FileCheck --check-prefix=CHECK %s
**/
#include <stdbool.h>

/**
CHECK-CC1: "-malign-power"
**/

struct _L {
 long long L;
} L;
/**
CHECK-LABEL: Type: struct _L
CHECK: ASTRecordLayout
CHECK-NEXT: Size:64
CHECK-NEXT: DataSize:64
CHECK-NEXT: Alignment:64

CHECK-LABEL: struct _L definition
CHECK-NEXT: L 'long long'
CHECK: LLVMType:%struct._L = type { i64 }
**/

struct _b {
 _Bool b;
} b;
/**
CHECK-LABEL: Type: struct _b
CHECK: ASTRecordLayout
CHECK-NEXT: Size:32
CHECK-NEXT: DataSize:32
CHECK-NEXT: Alignment:32

CHECK-LABEL: struct _b definition
CHECK-NEXT: b '_Bool'
CHECK: LLVMType:%struct._b = type { i32 }
**/

struct _c {
 char c;
} c;
/**
CHECK-LABEL: Type: struct _c
CHECK: ASTRecordLayout
CHECK-NEXT: Size:8
CHECK-NEXT: DataSize:8
CHECK-NEXT: Alignment:8

CHECK-LABEL: struct _c definition
CHECK-NEXT: c 'char'
CHECK: LLVMType:%struct._c = type { i8 }
**/

struct _s {
 short s;
} s;
/**
CHECK-LABEL: Type: struct _s
CHECK: ASTRecordLayout
CHECK-NEXT: Size:16
CHECK-NEXT: DataSize:16
CHECK-NEXT: Alignment:16

CHECK-LABEL: struct _s definition
CHECK-NEXT: s 'short'
CHECK: LLVMType:%struct._s = type { i16 }
**/

struct _i {
 int i;
} i;
/**
CHECK-LABEL: Type: struct _i
CHECK: ASTRecordLayout
CHECK-NEXT: Size:32
CHECK-NEXT: DataSize:32
CHECK-NEXT: Alignment:32

CHECK-LABEL: struct _i definition
CHECK-NEXT: i 'int'
CHECK: LLVMType:%struct._i = type { i32 }
**/

struct _l {
 long l;
} l;
/**
CHECK-LABEL: Type: struct _l
CHECK: ASTRecordLayout
CHECK-NEXT: Size:32
CHECK-NEXT: DataSize:32
CHECK-NEXT: Alignment:32

CHECK-LABEL: struct _l definition
CHECK-NEXT: l 'long'
CHECK: LLVMType:%struct._l = type { i32 }
**/

struct _f {
 float f;
} f;
/**
CHECK-LABEL: Type: struct _f
CHECK: ASTRecordLayout
CHECK-NEXT: Size:32
CHECK-NEXT: DataSize:32
CHECK-NEXT: Alignment:32

CHECK-LABEL: struct _f definition
CHECK-NEXT: f 'float'
CHECK: LLVMType:%struct._f = type { float }
**/

struct _d {
 double d;
} d;
/**
CHECK-LABEL: Type: struct _d
CHECK: ASTRecordLayout
CHECK-NEXT: Size:64
CHECK-NEXT: DataSize:64
CHECK-NEXT: Alignment:64

CHECK-LABEL: struct _d definition
CHECK-NEXT: d 'double'
CHECK: LLVMType:%struct._d = type { double }
**/

struct _D {
 long double D;
} D;
/**
CHECK-LABEL: Type: struct _D
CHECK: ASTRecordLayout
CHECK-NEXT: Size:128
CHECK-NEXT: DataSize:128
CHECK-NEXT: Alignment:128

CHECK-LABEL: struct _D definition
CHECK-NEXT: D 'long double'
CHECK: LLVMType:%struct._D = type { ppc_fp128 }
**/

#ifdef MAIN
#include <stdint.h>
#include <stdio.h>

int main (void)
{
  printf ("sizes   : b %ld c %ld s %ld i %ld l %ld ll %ld f %ld d %ld ld %ld\n",
  	sizeof(_Bool), sizeof(char), sizeof(short), sizeof(int), sizeof(long), 
  	sizeof(long long), sizeof(float), sizeof(double), sizeof(long double));
  printf ("aligns  : b %ld c %ld s %ld i %ld l %ld ll %ld f %ld d %ld ld %ld\n",
  	__alignof__(_Bool), __alignof__(char), __alignof__(short),
  	__alignof__(int), __alignof__(long), __alignof__(long long),
  	__alignof__(float), __alignof__(double), __alignof__(long double));

  printf ("s-size  : b %ld c %ld s %ld i %ld l %ld ll %ld f %ld d %ld ld %ld\n",
  	sizeof(b), sizeof(c), sizeof(s),
  	sizeof(i), sizeof(l), sizeof(L),
  	sizeof(f), sizeof(d), sizeof(D));
  printf ("s-align : b %ld c %ld s %ld i %ld l %ld ll %ld f %ld d %ld ld %ld\n",
  	__alignof__(b), __alignof__(c), __alignof__(s),
  	__alignof__(i), __alignof__(l), __alignof__(L),
  	__alignof__(f), __alignof__(d), __alignof__(D));
  
  return 0;
}

#endif
