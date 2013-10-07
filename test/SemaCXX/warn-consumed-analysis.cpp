// RUN: %clang_cc1 -fsyntax-only -verify -Wconsumed -std=c++11 %s

// TODO: Switch to using macros for the expected warnings.

#define CALLABLE_WHEN(...)      __attribute__ ((callable_when(__VA_ARGS__)))
#define CONSUMABLE(state)       __attribute__ ((consumable(state)))
#define CONSUMES                __attribute__ ((consumes))
#define RETURN_TYPESTATE(state) __attribute__ ((return_typestate(state)))
#define TESTS_UNCONSUMED        __attribute__ ((tests_unconsumed))

typedef decltype(nullptr) nullptr_t;

template <typename T>
class CONSUMABLE(unconsumed) ConsumableClass {
  T var;
  
  public:
  ConsumableClass();
  ConsumableClass(nullptr_t p) RETURN_TYPESTATE(consumed);
  ConsumableClass(T val) RETURN_TYPESTATE(unconsumed);
  ConsumableClass(ConsumableClass<T> &other);
  ConsumableClass(ConsumableClass<T> &&other);
  
  ConsumableClass<T>& operator=(ConsumableClass<T>  &other);
  ConsumableClass<T>& operator=(ConsumableClass<T> &&other);
  ConsumableClass<T>& operator=(nullptr_t) CONSUMES;
  
  template <typename U>
  ConsumableClass<T>& operator=(ConsumableClass<U>  &other);
  
  template <typename U>
  ConsumableClass<T>& operator=(ConsumableClass<U> &&other);
  
  void operator()(int a) CONSUMES;
  void operator*() const CALLABLE_WHEN("unconsumed");
  void unconsumedCall() const CALLABLE_WHEN("unconsumed");
  void callableWhenUnknown() const CALLABLE_WHEN("unconsumed", "unknown");
  
  bool isValid() const TESTS_UNCONSUMED;
  operator bool() const TESTS_UNCONSUMED;
  bool operator!=(nullptr_t) const TESTS_UNCONSUMED;
  
  void constCall() const;
  void nonconstCall();
  
  void consume() CONSUMES;
};

void baf0(const ConsumableClass<int>  var);
void baf1(const ConsumableClass<int> &var);
void baf2(const ConsumableClass<int> *var);

void baf3(ConsumableClass<int>  &var);
void baf4(ConsumableClass<int>  *var);
void baf5(ConsumableClass<int> &&var);

ConsumableClass<int> returnsUnconsumed() {
  return ConsumableClass<int>(); // expected-warning {{return value not in expected state; expected 'unconsumed', observed 'consumed'}}
}

ConsumableClass<int> returnsConsumed() RETURN_TYPESTATE(consumed);
ConsumableClass<int> returnsConsumed() {
  return ConsumableClass<int>();
}

ConsumableClass<int> returnsUnknown() RETURN_TYPESTATE(unknown);

void testInitialization() {
  ConsumableClass<int> var0;
  ConsumableClass<int> var1 = ConsumableClass<int>();
  
  var0 = ConsumableClass<int>();
  
  *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
  *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
  
  if (var0.isValid()) {
    *var0;
    *var1;
    
  } else {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
  }
}

void testTempValue() {
  *ConsumableClass<int>(); // expected-warning {{invalid invocation of method 'operator*' on a temporary object while it is in the 'consumed' state}}
}

void testSimpleRValueRefs() {
  ConsumableClass<int> var0;
  ConsumableClass<int> var1(42);
  
  *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
  *var1;
  
  var0 = static_cast<ConsumableClass<int>&&>(var1);
  
  *var0;
  *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
}

void testIfStmt() {
  ConsumableClass<int> var;
  
  if (var.isValid()) {
    *var;
  } else {
    *var; // expected-warning {{invalid invocation of method 'operator*' on object 'var' while it is in the 'consumed' state}}
  }
  
  if (!var.isValid()) {
    *var; // expected-warning {{invalid invocation of method 'operator*' on object 'var' while it is in the 'consumed' state}}
  } else {
    *var;
  }
  
  if (var) {
    // Empty
  } else {
    *var; // expected-warning {{invalid invocation of method 'operator*' on object 'var' while it is in the 'consumed' state}}
  }
  
  if (var != nullptr) {
    // Empty
  } else {
    *var; // expected-warning {{invalid invocation of method 'operator*' on object 'var' while it is in the 'consumed' state}}
  }
}

void testComplexConditionals0() {
  ConsumableClass<int> var0, var1, var2;
  
  if (var0 && var1) {
    *var0;
    *var1;
    
  } else {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
  }
  
  if (var0 || var1) {
    *var0;
    *var1;
    
  } else {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
  }
  
  if (var0 && !var1) {
    *var0;
    *var1;
    
  } else {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
  }
  
  if (var0 || !var1) {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
    
  } else {
    *var0;
    *var1;
  }
  
  if (!var0 && !var1) {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
    
  } else {
    *var0;
    *var1;
  }
  
  if (!var0 || !var1) {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
    
  } else {
    *var0;
    *var1;
  }
  
  if (!(var0 && var1)) {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
    
  } else {
    *var0;
    *var1;
  }
  
  if (!(var0 || var1)) {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
    
  } else {
    *var0;
    *var1;
  }
  
  if (var0 && var1 && var2) {
    *var0;
    *var1;
    *var2;
    
  } else {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
    *var2; // expected-warning {{invalid invocation of method 'operator*' on object 'var2' while it is in the 'consumed' state}}
  }
  
#if 0
  // FIXME: Get this test to pass.
  if (var0 || var1 || var2) {
    *var0;
    *var1;
    *var2;
    
  } else {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
    *var2; // expected-warning {{invalid invocation of method 'operator*' on object 'var2' while it is in the 'consumed' state}}
  }
#endif
}

void testComplexConditionals1() {
  ConsumableClass<int> var0, var1, var2;
  
  // Coerce all variables into the unknown state.
  baf3(var0);
  baf3(var1);
  baf3(var2);
  
  if (var0 && var1) {
    *var0;
    *var1;
    
  } else {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'unknown' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'unknown' state}}
  }
  
  if (var0 || var1) {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'unknown' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'unknown' state}}
    
  } else {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
  }
  
  if (var0 && !var1) {
    *var0;
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
    
  } else {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'unknown' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'unknown' state}}
  }
  
  if (var0 || !var1) {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'unknown' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'unknown' state}}
    
  } else {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
    *var1;
  }
  
  if (!var0 && !var1) {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
    
  } else {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'unknown' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'unknown' state}}
  }
  
  if (!(var0 || var1)) {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
    
  } else {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'unknown' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'unknown' state}}
  }
  
  if (!var0 || !var1) {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'unknown' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'unknown' state}}
    
  } else {
    *var0;
    *var1;
  }
  
  if (!(var0 && var1)) {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'unknown' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'unknown' state}}
    
  } else {
    *var0;
    *var1;
  }
  
  if (var0 && var1 && var2) {
    *var0;
    *var1;
    *var2;
    
  } else {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'unknown' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'unknown' state}}
    *var2; // expected-warning {{invalid invocation of method 'operator*' on object 'var2' while it is in the 'unknown' state}}
  }
  
#if 0
  // FIXME: Get this test to pass.
  if (var0 || var1 || var2) {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'unknown' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'unknown' state}}
    *var2; // expected-warning {{invalid invocation of method 'operator*' on object 'var2' while it is in the 'unknown' state}}
    
  } else {
    *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
    *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
    *var2; // expected-warning {{invalid invocation of method 'operator*' on object 'var2' while it is in the 'consumed' state}}
  }
#endif
}

void testStateChangeInBranch() {
  ConsumableClass<int> var;
  
  // Make var enter the 'unknown' state.
  baf3(var);
  
  if (!var) {
    var = ConsumableClass<int>(42);
  }
  
  *var;
}

void testFunctionParam(ConsumableClass<int> param) {
  
  if (param.isValid()) {
    *param;
  } else {
    *param;
  }
  
  param = nullptr;
  *param; // expected-warning {{invocation of method 'operator*' on object 'param' while it is in the 'consumed' state}}
}

void testCallingConventions() {
  ConsumableClass<int> var(42);
  
  baf0(var);  
  *var;
  
  baf1(var);  
  *var;
  
  baf2(&var);  
  *var;
  
  baf3(var);  
  *var; // expected-warning {{invalid invocation of method 'operator*' on object 'var' while it is in the 'unknown' state}}
  
  var = ConsumableClass<int>(42);
  baf4(&var);  
  *var; // expected-warning {{invalid invocation of method 'operator*' on object 'var' while it is in the 'unknown' state}}
  
  var = ConsumableClass<int>(42);
  baf5(static_cast<ConsumableClass<int>&&>(var));  
  *var; // expected-warning {{invalid invocation of method 'operator*' on object 'var' while it is in the 'consumed' state}}
}

void testConstAndNonConstMemberFunctions() {
  ConsumableClass<int> var(42);
  
  var.constCall();
  *var;
  
  var.nonconstCall();
  *var;
}

void testFunctionParam0(ConsumableClass<int> param) {
  *param;
}

void testFunctionParam1(ConsumableClass<int> &param) {
  *param; // expected-warning {{invalid invocation of method 'operator*' on object 'param' while it is in the 'unknown' state}}
}

void testReturnStates() {
  ConsumableClass<int> var;
  
  var = returnsUnconsumed();
  *var;
  
  var = returnsConsumed();
  *var; // expected-warning {{invalid invocation of method 'operator*' on object 'var' while it is in the 'consumed' state}}
}

void testCallableWhen() {
  ConsumableClass<int> var(42);
  
  *var;
  
  baf3(var);
  
  var.callableWhenUnknown();
}

void testMoveAsignmentish() {
  ConsumableClass<int>  var0;
  ConsumableClass<long> var1(42);
  
  *var0; // expected-warning {{invalid invocation of method 'operator*' on object 'var0' while it is in the 'consumed' state}}
  *var1;
  
  var0 = static_cast<ConsumableClass<long>&&>(var1);
  
  *var0;
  *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
  
  var1 = ConsumableClass<long>(42);
  var1 = nullptr;
  *var1; // expected-warning {{invalid invocation of method 'operator*' on object 'var1' while it is in the 'consumed' state}}
}

void testConditionalMerge() {
  ConsumableClass<int> var;
  
  if (var.isValid()) {
    // Empty
  }
  
  *var; // expected-warning {{invalid invocation of method 'operator*' on object 'var' while it is in the 'consumed' state}}
  
  if (var.isValid()) {
    // Empty
  } else {
    // Empty
  }
  
  *var; // expected-warning {{invalid invocation of method 'operator*' on object 'var' while it is in the 'consumed' state}}
}

void testConsumes0() {
  ConsumableClass<int> var(42);
  
  *var;
  
  var.consume();
  
  *var; // expected-warning {{invalid invocation of method 'operator*' on object 'var' while it is in the 'consumed' state}}
}

void testConsumes1() {
  ConsumableClass<int> var(nullptr);
  
  *var; // expected-warning {{invalid invocation of method 'operator*' on object 'var' while it is in the 'consumed' state}}
}

void testConsumes2() {
  ConsumableClass<int> var(42);
  
  var.unconsumedCall();
  var(6);
  
  var.unconsumedCall(); // expected-warning {{invalid invocation of method 'unconsumedCall' on object 'var' while it is in the 'consumed' state}}
}

void testUnreachableBlock() {
  ConsumableClass<int> var(42);
  
  if (var) {
    *var;
  } else {
    *var;
  }
  
  *var;
}

void testSimpleForLoop() {
  ConsumableClass<int> var;
  
  for (int i = 0; i < 10; ++i) {
    *var; // expected-warning {{invalid invocation of method 'operator*' on object 'var' while it is in the 'unknown' state}}
  }
  
  *var; // expected-warning {{invalid invocation of method 'operator*' on object 'var' while it is in the 'unknown' state}}
}

void testSimpleWhileLoop() {
  int i = 0;
  
  ConsumableClass<int> var;
  
  while (i < 10) {
    *var; // expected-warning {{invalid invocation of method 'operator*' on object 'var' while it is in the 'unknown' state}}
    ++i;
  }
  
  *var; // expected-warning {{invalid invocation of method 'operator*' on object 'var' while it is in the 'unknown' state}}
}
