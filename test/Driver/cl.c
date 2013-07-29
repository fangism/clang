// Note: we have to quote the options with a /, otherwise some shells will try
// to expand it and make test fail.

// Check that clang-cl options are not available by default.
// RUN: %clang -help | FileCheck %s -check-prefix=DEFAULT
// DEFAULT-NOT: CL.EXE COMPATIBILITY OPTIONS
// DEFAULT-NOT: {{/[?]}}
// DEFAULT-NOT: /help
// RUN: not %clang '/?'
// RUN: not %clang -?
// RUN: not %clang '/help'

// Check that '/?' and '/help' are available as clang-cl options.
// RUN: %clang_cl '/?' | FileCheck %s -check-prefix=CL
// RUN: %clang_cl '/help' | FileCheck %s -check-prefix=CL
// RUN: %clang_cl -help | FileCheck %s -check-prefix=CL
// CL: CL.EXE COMPATIBILITY OPTIONS
// CL: {{/[?]}}
// CL: /help

// Options which are not "core" clang options nor cl.exe compatible options
// are not available in clang-cl.
// DEFAULT: -fapple-kext
// CL-NOT: -fapple-kext

// Don't attempt slash switches on msys bash.
// REQUIRES: shell-preserves-root
