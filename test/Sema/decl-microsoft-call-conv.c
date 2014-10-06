// RUN: %clang_cc1 -triple i686-pc-win32 -verify %s

// It's important that this is a .c file.

// This is fine, as CrcGenerateTable*() has a prototype.
void __fastcall CrcGenerateTableFastcall(void);
void __fastcall CrcGenerateTableFastcall() {}
void __stdcall CrcGenerateTableStdcall(void);
void __stdcall CrcGenerateTableStdcall() {}
void __thiscall CrcGenerateTableThiscall(void);
void __thiscall CrcGenerateTableThiscall() {}
void __pascal CrcGenerateTablePascal(void);
void __pascal CrcGenerateTablePascal() {}

void __fastcall CrcGenerateTableNoProtoFastcall() {} // expected-error{{function with no prototype cannot use the callee-cleanup fastcall calling convention}}
void __stdcall CrcGenerateTableNoProtoStdcall() {} // expected-warning{{function with no prototype cannot use the callee-cleanup stdcall calling convention}}
void __thiscall CrcGenerateTableNoProtoThiscall() {} // expected-error{{function with no prototype cannot use the callee-cleanup thiscall calling convention}}
void __pascal CrcGenerateTableNoProtoPascal() {} // expected-error{{function with no prototype cannot use the callee-cleanup pascal calling convention}}

// Regular calling convention is fine.
void CrcGenerateTableNoProto() {}
