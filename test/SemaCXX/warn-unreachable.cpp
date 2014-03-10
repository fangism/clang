// RUN: %clang_cc1 %s -fcxx-exceptions -fexceptions -fsyntax-only -verify -fblocks -Wunreachable-code -Wno-unused-value

int &halt() __attribute__((noreturn));
int &live();
int dead();
int liveti() throw(int);
int (*livetip)() throw(int);

int test1() {
  try {
    live();
  } catch (int i) {
    live();
  }
  return 1;
}

void test2() {
  try {
    live();
  } catch (int i) {
    live();
  }
  try {
    liveti();
  } catch (int i) {
    live();
  }
  try {
    livetip();
  } catch (int i) {
    live();
  }
  throw 1;
  dead();       // expected-warning {{will never be executed}}
}


void test3() {
  halt()
    --;         // expected-warning {{will never be executed}}
  // FIXME: The unreachable part is just the '?', but really all of this
  // code is unreachable and shouldn't be separately reported.
  halt()        // expected-warning {{will never be executed}}
    ? 
    dead() : dead();
  live(),
    float       
      (halt()); // expected-warning {{will never be executed}}
}

void test4() {
  struct S {
    int mem;
  } s;
  S &foor();
  halt(), foor()// expected-warning {{will never be executed}}
    .mem;       
}

void test5() {
  struct S {
    int mem;
  } s;
  S &foonr() __attribute__((noreturn));
  foonr()
    .mem;       // expected-warning {{will never be executed}}
}

void test6() {
  struct S {
    ~S() { }
    S(int i) { }
  };
  live(),
    S
      (halt());  // expected-warning {{will never be executed}}
}

// Don't warn about unreachable code in template instantiations, as
// they may only be unreachable in that specific instantiation.
void isUnreachable();

template <typename T> void test_unreachable_templates() {
  T::foo();
  isUnreachable();  // no-warning
}

struct TestUnreachableA {
  static void foo() __attribute__((noreturn));
};
struct TestUnreachableB {
  static void foo();
};

void test_unreachable_templates_harness() {
  test_unreachable_templates<TestUnreachableA>();
  test_unreachable_templates<TestUnreachableB>(); 
}

// Do warn about explict template specializations, as they represent
// actual concrete functions that somebody wrote.

template <typename T> void funcToSpecialize() {}
template <> void funcToSpecialize<int>() {
  halt();
  dead(); // expected-warning {{will never be executed}}
}

// Handle 'try' code dominating a dead return.
enum PR19040_test_return_t
{ PR19040_TEST_FAILURE };
namespace PR19040_libtest
{
  class A {
  public:
    ~A ();
  };
}
PR19040_test_return_t PR19040_fn1 ()
{
    try
    {
        throw PR19040_libtest::A ();
    } catch (...)
    {
        return PR19040_TEST_FAILURE;
    }
    return PR19040_TEST_FAILURE; // expected-warning {{will never be executed}}
}

__attribute__((noreturn))
void raze();

namespace std {
template<typename T> struct basic_string {
  basic_string(const T* x) {}
  ~basic_string() {};
};
typedef basic_string<char> string;
}

std::string testStr() {
  raze();
  return ""; // no-warning
}

std::string testStrWarn(const char *s) {
  raze();
  return s; // expected-warning {{will never be executed}}
}

bool testBool() {
  raze();
  return true; // no-warning
}

static const bool ConditionVar = 1;
int test_global_as_conditionVariable() {
  if (ConditionVar)
    return 1;
  return 0; // no-warning
}

// Handle unreachable temporary destructors.
class A {
public:
  A();
  ~A();
};

__attribute__((noreturn))
void raze(const A& x);

void test_with_unreachable_tmp_dtors(int x) {
  raze(x ? A() : A()); // no-warning
}

// Test sizeof - sizeof in enum declaration.
enum { BrownCow = sizeof(long) - sizeof(char) };
enum { CowBrown = 8 - 1 };


int test_enum_sizeof_arithmetic() {
  if (BrownCow)
    return 1;
  return 2;
}

int test_enum_arithmetic() {
  if (CowBrown)
    return 1;
  return 2; // expected-warning {{never be executed}}
}

int test_arithmetic() {
  if (8 -1)
    return 1;
  return 2; // expected-warning {{never be executed}}
}

