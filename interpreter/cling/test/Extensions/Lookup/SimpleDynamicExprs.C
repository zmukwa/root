//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.
//------------------------------------------------------------------------------

// RUN: cat %s | %cling -I%p | FileCheck %s

#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/InterpreterCallbacks.h"

.dynamicExtensions

gCling->setCallbacks(new cling::test::SymbolResolverCallback(gCling));
jksghdgsjdf->getVersion() // CHECK: {{.*Interpreter.*}}
hsdghfjagsp->Draw() // CHECK: (int) 12

h->PrintString(std::string("test")); // CHECK: test
int a[5] = {1,2,3,4,5};
h->PrintArray(a, 5); // CHECK: 12345
.q
