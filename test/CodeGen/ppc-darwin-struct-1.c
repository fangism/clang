/**
REQUIRES: powerpc-registered-target
RUN: %clang_cc1 -emit-llvm-only -fdump-record-layouts -triple powerpc-apple-darwin8 -malign-power %s 2> /dev/null | \
RUN: grep -v Comment | \
RUN: FileCheck --check-prefix=CHECK %s
**/

struct _cd {
 char c;
 double d;
} cd;
/**
CHECK-LABEL: Type: struct _cd
CHECK: ASTRecordLayout
CHECK-NEXT: Size:96
CHECK-NEXT: DataSize:96
CHECK-NEXT: Alignment:32
CHECK-NEXT: FieldOffsets: [0, 32]

CHECK-LABEL: struct _cd definition
CHECK-NEXT: c 'char'
CHECK-NEXT: d 'double'
CHECK: LLVMType:%struct._cd = type { i8, double }
**/

struct _dc {
 double d;
 char c;
} dc;
/**
CHECK-LABEL: Type: struct _dc
CHECK: ASTRecordLayout
CHECK-NEXT: Size:128
CHECK-NEXT: DataSize:128
CHECK-NEXT: Alignment:64
CHECK-NEXT: FieldOffsets: [0, 64]

CHECK-LABEL: struct _dc definition
CHECK-NEXT: d 'double'
CHECK-NEXT: c 'char'
CHECK: LLVMType:%struct._dc = type { double, i8, [7 x i8] }
**/

struct _cL {
 char c;
 long long L;
} cL;
/**
CHECK-LABEL: Type: struct _cL
CHECK: ASTRecordLayout
CHECK-NEXT: Size:96
CHECK-NEXT: DataSize:96
CHECK-NEXT: Alignment:32
CHECK-NEXT: FieldOffsets: [0, 32]

CHECK-LABEL: struct _cL definition
CHECK-NEXT: c 'char'
CHECK-NEXT: L 'long long'
CHECK: LLVMType:%struct._cL = type { i8, i64 }
**/

struct _Lc {
 long long L;
 char c;
} Lc;
/**
CHECK-LABEL: Type: struct _Lc
CHECK: ASTRecordLayout
CHECK-NEXT: Size:128
CHECK-NEXT: DataSize:128
CHECK-NEXT: Alignment:64
CHECK-NEXT: FieldOffsets: [0, 64]

CHECK-LABEL: struct _Lc definition
CHECK-NEXT: L 'long long'
CHECK-NEXT: c 'char'
CHECK: LLVMType:%struct._Lc = type { i64, i8, [7 x i8] }
**/

struct _cScd {
 char c;
 struct _cd Scd;
} cScd;
/**
CHECK-LABEL: Type: struct _cScd
CHECK: ASTRecordLayout
CHECK-NEXT: Size:128
CHECK-NEXT: DataSize:128
CHECK-NEXT: Alignment:32
CHECK-NEXT: FieldOffsets: [0, 32]

CHECK-LABEL: struct _cScd definition
CHECK-NEXT: c 'char'
CHECK-NEXT: Scd 'struct _cd':'struct _cd'
CHECK: LLVMType:%struct._cScd = type { i8, %struct._cd }
**/

struct _cSdc {
 char c;
 struct _dc Sdc;
} cSdc;
/**
CHECK-LABEL: Type: struct _cSdc
CHECK: ASTRecordLayout
CHECK-NEXT: Size:160
CHECK-NEXT: DataSize:160
CHECK-NEXT: Alignment:32
CHECK-NEXT: FieldOffsets: [0, 32]

CHECK-LABEL: struct _cSdc definition
CHECK-NEXT: c 'char'
CHECK-NEXT: Sdc 'struct _dc':'struct _dc'
CHECK: LLVMType:%struct._cSdc = type { i8, %struct._dc }
**/

struct _cScL {
 char c;
 struct _cL ScL;
} cScL;
/**
CHECK-LABEL: Type: struct _cScL
CHECK: ASTRecordLayout
CHECK-NEXT: Size:128
CHECK-NEXT: DataSize:128
CHECK-NEXT: Alignment:32
CHECK-NEXT: FieldOffsets: [0, 32]

CHECK-LABEL: struct _cScL definition
CHECK-NEXT: c 'char'
CHECK-NEXT: ScL 'struct _cL':'struct _cL'
CHECK: LLVMType:%struct._cScL = type { i8, %struct._cL }
**/

struct _cSLc {
 char c;
 struct _Lc SLc;
} cSLc;
/**
CHECK-LABEL: Type: struct _cSLc
CHECK: ASTRecordLayout
CHECK-NEXT: Size:160
CHECK-NEXT: DataSize:160
CHECK-NEXT: Alignment:32
CHECK-NEXT: FieldOffsets: [0, 32]

CHECK-LABEL: struct _cSLc definition
CHECK-NEXT: c 'char'
CHECK-NEXT: SLc 'struct _Lc':'struct _Lc'
CHECK: LLVMType:%struct._cSLc = type { i8, %struct._Lc }
**/

#ifdef MAIN
#include <stdint.h>
#include <stdio.h>

int main (void)
{
  printf ("s-size  : cd %ld dc %ld cL %ld Lc %ld cScd %ld cSdc %ld cScL %ld cSLc %ld\n",
  	sizeof(cd), sizeof(dc), sizeof(cL), sizeof(Lc),
  	sizeof(cScd), sizeof(cSdc), sizeof(cScL), sizeof(cSLc));
  printf ("s-align : cd %ld dc %ld cL %ld Lc %ld cScd %ld cSdc %ld cScL %ld cSLc %ld\n",
  	__alignof__(cd), __alignof__(dc), __alignof__(cL), __alignof__(Lc),
  	__alignof__(cScd), __alignof__(cSdc), __alignof__(cScL), __alignof__(cSLc));
  
  return 0;
}

#endif
