/**
REQUIRES: powerpc-registered-target
RUN: %clang_cc1 -emit-llvm-only -fdump-record-layouts -triple powerpc-apple-darwin8 -malign-power %s 2> /dev/null | \
RUN: grep -v Comment | \
RUN: FileCheck --check-prefix=CHECK %s
**/

struct _cD {
 char c;
 long double D;
} cD;
/**
CHECK-LABEL: Type: struct _cD
CHECK: ASTRecordLayout
CHECK-NEXT: Size:256
CHECK-NEXT: DataSize:256
CHECK-NEXT: Alignment:128
CHECK-NEXT: FieldOffsets: [0, 128]

CHECK-LABEL: struct _cD definition
CHECK-NEXT: c 'char'
CHECK-NEXT: D 'long double'
CHECK: LLVMType:%struct._cD = type { i8, ppc_fp128 }
**/

struct _Dc {
 long double D;
 char c;
} Dc;
/**
CHECK-LABEL: Type: struct _Dc
CHECK: ASTRecordLayout
CHECK-NEXT: Size:256
CHECK-NEXT: DataSize:256
CHECK-NEXT: Alignment:128
CHECK-NEXT: FieldOffsets: [0, 128]

CHECK-LABEL: struct _Dc definition
CHECK-NEXT: D 'long double'
CHECK-NEXT: c 'char'
CHECK: LLVMType:%struct._Dc = type { ppc_fp128, i8, [15 x i8] }
**/

struct _cScD {
 char c;
 struct _cD ScD;
} cScD;
/**
CHECK-LABEL: Type: struct _cScD
CHECK: ASTRecordLayout
CHECK-NEXT: Size:384
CHECK-NEXT: DataSize:384
CHECK-NEXT: Alignment:128
CHECK-NEXT: FieldOffsets: [0, 128]

CHECK-LABEL: struct _cScD definition
CHECK-NEXT: c 'char'
CHECK-NEXT: ScD 'struct _cD':'struct _cD'
CHECK: LLVMType:%struct._cScD = type { i8, %struct._cD }
**/

struct _cSDc {
 char c;
 struct _Dc SDc;
} cSDc;
/**
CHECK-LABEL: Type: struct _cSDc
CHECK: ASTRecordLayout
CHECK-NEXT: Size:384
CHECK-NEXT: DataSize:384
CHECK-NEXT: Alignment:128
CHECK-NEXT: FieldOffsets: [0, 128]

CHECK-LABEL: struct _cSDc definition
CHECK-NEXT: c 'char'
CHECK-NEXT: SDc 'struct _Dc':'struct _Dc'
CHECK: LLVMType:%struct._cSDc = type { i8, %struct._Dc }
**/

#ifdef MAIN
#include <stdint.h>
#include <stdio.h>

int main (void)
{
  printf ("s-size  : cD %ld Dc %ld cScD %ld cSDc %ld\n",
  	sizeof(cD), sizeof(Dc), sizeof(cScD), sizeof(cSDc));
  printf ("s-align : cD %ld Dc %ld cScD %ld cSDc %ld\n",
  	__alignof__(cD), __alignof__(Dc),
  	__alignof__(cScD), __alignof__(cSDc));
  
  return 0;
}

#endif
