// Don't attempt slash switches on msys bash.
// REQUIRES: shell-preserves-root

// Note: %s must be preceded by -- or bound to another option, otherwise it may
// be interpreted as a command-line option, e.g. on Mac where %s is commonly
// under /Users.

// RUN: %clang_cl /Tc%s -### /link foo bar baz 2>&1 | FileCheck --check-prefix=LINK %s
// LINK: link.exe
// LINK: "foo"
// LINK: "bar"
// LINK: "baz"

// RUN: %clang_cl /Tc%s -### -fsanitize=address 2>&1 | FileCheck --check-prefix=ASAN %s
// ASAN: link.exe
// ASAN: "-debug"
// ASAN: "-incremental:no"
// ASAN: "{{.*}}clang_rt.asan-i386.lib"
// ASAN: "{{.*}}cl-link{{.*}}.obj"
