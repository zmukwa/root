//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.
//------------------------------------------------------------------------------

// RUN: cat %s | %cling -I%p | FileCheck %s

// This file should be used as regression test for the meta processing subsystem
// Reproducers of fixed bugs should be put here

// PR #96277
#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/StoredValueRef.h"
#include <stdio.h>
gCling->declare("int print() { printf(\"print is run.\\n\"); return 1; }");
cling::StoredValueRef V;
gCling->process("int a = print();",&V);
//CHECK: print is run.
gCling->process("a", &V);
//CHECK: (int) 1
gCling->process("a;", &V);
//CHECK-NOT: print is run.
// End PR #96277
// PR #98146
gCling->process("\"Root\"", &V);
// CHECK: (const char [5]) "Root"
V
// CHECK: (cling::StoredValueRef) boxes [(const char *) "Root"]
// End PR #98146
typedef enum {k1,k2} enumName;
enumName var
// CHECK: (enumName) (::k1) : (int) 0
const enumName constVar = (enumName) 1 // k2 is invisible!
// CHECK: (const enumName) (::k2) : (int) 1
