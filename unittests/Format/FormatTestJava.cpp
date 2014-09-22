//===- unittest/Format/FormatTestJava.cpp - Formatting tests for Java -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "FormatTestUtils.h"
#include "clang/Format/Format.h"
#include "llvm/Support/Debug.h"
#include "gtest/gtest.h"

#define DEBUG_TYPE "format-test"

namespace clang {
namespace format {

class FormatTestJava : public ::testing::Test {
protected:
  static std::string format(llvm::StringRef Code, unsigned Offset,
                            unsigned Length, const FormatStyle &Style) {
    DEBUG(llvm::errs() << "---\n");
    DEBUG(llvm::errs() << Code << "\n\n");
    std::vector<tooling::Range> Ranges(1, tooling::Range(Offset, Length));
    tooling::Replacements Replaces = reformat(Style, Code, Ranges);
    std::string Result = applyAllReplacements(Code, Replaces);
    EXPECT_NE("", Result);
    DEBUG(llvm::errs() << "\n" << Result << "\n\n");
    return Result;
  }

  static std::string format(
      llvm::StringRef Code,
      const FormatStyle &Style = getGoogleStyle(FormatStyle::LK_Java)) {
    return format(Code, 0, Code.size(), Style);
  }

  static FormatStyle getGoogleJSStyleWithColumns(unsigned ColumnLimit) {
    FormatStyle Style = getGoogleStyle(FormatStyle::LK_Java);
    Style.ColumnLimit = ColumnLimit;
    return Style;
  }

  static void verifyFormat(
      llvm::StringRef Code,
      const FormatStyle &Style = getGoogleStyle(FormatStyle::LK_Java)) {
    EXPECT_EQ(Code.str(), format(test::messUp(Code), Style));
  }
};

TEST_F(FormatTestJava, ClassDeclarations) {
  verifyFormat("public class SomeClass {\n"
               "  private int a;\n"
               "  private int b;\n"
               "}");
  verifyFormat("public class A {\n"
               "  class B {\n"
               "    int i;\n"
               "  }\n"
               "  class C {\n"
               "    int j;\n"
               "  }\n"
               "}");
}

} // end namespace tooling
} // end namespace clang
