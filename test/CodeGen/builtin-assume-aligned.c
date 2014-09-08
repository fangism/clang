// RUN: %clang_cc1 -triple x86_64-unknown-unknown -emit-llvm -o - %s | FileCheck %s

// CHECK-LABEL: @test1
int test1(int *a) {
// CHECK: [[PTRINT1:%.+]] = ptrtoint
// CHECK: [[MASKEDPTR1:%.+]] = and i64 [[PTRINT1]], 31
// CHECK: [[MASKCOND1:%.+]] = icmp eq i64 [[MASKEDPTR1]], 0
// CHECK: call void @llvm.assume(i1 [[MASKCOND1]])
  a = __builtin_assume_aligned(a, 32, 0ull);
  return a[0];
}

// CHECK-LABEL: @test2
int test2(int *a) {
// CHECK: [[PTRINT2:%.+]] = ptrtoint
// CHECK: [[MASKEDPTR2:%.+]] = and i64 [[PTRINT2]], 31
// CHECK: [[MASKCOND2:%.+]] = icmp eq i64 [[MASKEDPTR2]], 0
// CHECK: call void @llvm.assume(i1 [[MASKCOND2]])
  a = __builtin_assume_aligned(a, 32, 0);
  return a[0];
}

// CHECK-LABEL: @test3
int test3(int *a) {
// CHECK: [[PTRINT3:%.+]] = ptrtoint
// CHECK: [[MASKEDPTR3:%.+]] = and i64 [[PTRINT3]], 31
// CHECK: [[MASKCOND3:%.+]] = icmp eq i64 [[MASKEDPTR3]], 0
// CHECK: call void @llvm.assume(i1 [[MASKCOND3]])
  a = __builtin_assume_aligned(a, 32);
  return a[0];
}

// CHECK-LABEL: @test4
int test4(int *a, int b) {
// CHECK-DAG: [[PTRINT4:%.+]] = ptrtoint
// CHECK-DAG: [[CONV4:%.+]] = sext i32
// CHECK: [[OFFSETPTR4:%.+]] = sub i64 [[PTRINT4]], [[CONV4]]
// CHECK: [[MASKEDPTR4:%.+]] = and i64 [[OFFSETPTR4]], 31
// CHECK: [[MASKCOND4:%.+]] = icmp eq i64 [[MASKEDPTR4]], 0
// CHECK: call void @llvm.assume(i1 [[MASKCOND4]])
  a = __builtin_assume_aligned(a, 32, b);
  return a[0];
}

