// RUN: %clang_cc1 -analyze -analyzer-checker=core -analyzer-config suppress-null-return-paths=false -verify %s
// RUN: %clang_cc1 -analyze -analyzer-checker=core -verify -DSUPPRESSED=1 %s

namespace rdar12676053 {
  // Delta-reduced from a preprocessed file.
  template<class T>
  class RefCount {
    T *ref;
  public:
    T *operator->() const {
      return ref ? ref : 0;
    }
  };

  class string {};

  class ParserInputState {
  public:
    string filename;
  };

  class Parser {
    void setFilename(const string& f)  {
      inputState->filename = f;
#ifndef SUPPRESSED
// expected-warning@-2 {{Called C++ object pointer is null}}
#endif
    }
  protected:
    RefCount<ParserInputState> inputState;
  };
}


// This is the standard placement new.
inline void* operator new(__typeof__(sizeof(int)), void* __p) throw()
{
  return __p;
}

extern bool coin();

namespace References {
  class Map {
    int *&getNewBox();
    int *firstBox;

  public:
    int *&getValue(int key) {
      if (coin()) {
        return firstBox;
      } else {
        int *&newBox = getNewBox();
        newBox = 0;
        return newBox;
      }
    }

    int *&getValueIndirectly(int key) {
      int *&valueBox = getValue(key);
      return valueBox;
    }
  };

  void testMap(Map &m, int i) {
    *m.getValue(i) = 1;
#ifndef SUPPRESSED
    // expected-warning@-2 {{Dereference of null pointer}}
#endif

    *m.getValueIndirectly(i) = 1;
#ifndef SUPPRESSED
    // expected-warning@-2 {{Dereference of null pointer}}
#endif

    int *&box = m.getValue(i);
    extern int *getPointer();
    box = getPointer();
    *box = 1; // no-warning

    int *&box2 = m.getValue(i);
    box = 0;
    *box = 1; // expected-warning {{Dereference of null pointer}}
  }

  class SomeClass {
  public:
    void doSomething();
  };

  SomeClass *&getSomeClass() {
    if (coin()) {
      extern SomeClass *&opaqueClass();
      return opaqueClass();
    } else {
      static SomeClass *sharedClass;
      sharedClass = 0;
      return sharedClass;
    }
  }

  void testClass() {
    getSomeClass()->doSomething();
#ifndef SUPPRESSED
    // expected-warning@-2 {{Called C++ object pointer is null}}
#endif

    // Separate the lvalue-to-rvalue conversion from the subsequent dereference.
    SomeClass *object = getSomeClass();
    object->doSomething();
#ifndef SUPPRESSED
    // expected-warning@-2 {{Called C++ object pointer is null}}
#endif
  }
}
