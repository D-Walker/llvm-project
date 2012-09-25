// RUN: %clang_cc1 -emit-llvm %s -o - -cxx-abi microsoft -triple=i386-pc-win32 | FileCheck %s

class A {
 public:
  A() { }
  ~A() { }
};

void no_contstructor_destructor_infinite_recursion() {
  A a;

// CHECK:      define linkonce_odr x86_thiscallcc %class.A* @"\01??0A@@QAE@XZ"(%class.A* %this)
// CHECK-NEXT: entry:
// CHECK-NEXT:   %this.addr = alloca %class.A*, align 4
// CHECK-NEXT:   store %class.A* %this, %class.A** %this.addr, align 4
// CHECK-NEXT:   %this1 = load %class.A** %this.addr
// CHECK-NEXT:   ret %class.A* %this1
// CHECK-NEXT: }

// Make sure that the destructor doesn't call itself:
// CHECK: define {{.*}} @"\01??1A@@QAE@XZ"
// CHECK-NOT: call void @"\01??1A@@QAE@XZ"
// CHECK: ret
}

