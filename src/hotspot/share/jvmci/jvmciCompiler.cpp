/*
 * Copyright (c) 2011, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "precompiled.hpp"
#include "compiler/compileBroker.hpp"
#include "classfile/moduleEntry.hpp"
#include "jvmci/jvmciEnv.hpp"
#include "jvmci/jvmciRuntime.hpp"
#include "oops/objArrayOop.inline.hpp"
#include "runtime/compilationPolicy.hpp"
#include "runtime/handles.inline.hpp"

JVMCICompiler* JVMCICompiler::_instance = NULL;
elapsedTimer JVMCICompiler::_codeInstallTimer;
elapsedTimer JVMCICompiler::_hostedCodeInstallTimer;

JVMCICompiler::JVMCICompiler() : AbstractCompiler(compiler_jvmci) {
  _bootstrapping = false;
  _bootstrap_compilation_request_handled = false;
  _methods_compiled = 0;
  _global_compilation_ticks = 0;
  assert(_instance == NULL, "only one instance allowed");
  _instance = this;
}

// Initialization
void JVMCICompiler::initialize() {
  if (!UseCompiler || !EnableJVMCI || !UseJVMCICompiler || !should_perform_init()) {
    return;
  }

  set_state(initialized);

  // JVMCI is considered as application code so we need to
  // stop the VM deferring compilation now.
  CompilationPolicy::completed_vm_startup();
}

void JVMCICompiler::bootstrap(TRAPS) {
  if (Arguments::mode() == Arguments::_int) {
    // Nothing to do in -Xint mode
    return;
  }
#ifndef PRODUCT
  // We turn off CompileTheWorld so that compilation requests are not
  // ignored during bootstrap or that JVMCI can be compiled by C1/C2.
  FlagSetting ctwOff(CompileTheWorld, false);
#endif

  _bootstrapping = true;
  ResourceMark rm;
  HandleMark hm;
  if (PrintBootstrap) {
    tty->print("Bootstrapping JVMCI");
  }
  jlong start = os::javaTimeMillis();

  Array<Method*>* objectMethods = SystemDictionary::Object_klass()->methods();
  // Initialize compile queue with a selected set of methods.
  int len = objectMethods->length();
  for (int i = 0; i < len; i++) {
    methodHandle mh = objectMethods->at(i);
    if (!mh->is_native() && !mh->is_static() && !mh->is_initializer()) {
      ResourceMark rm;
      int hot_count = 10; // TODO: what's the appropriate value?
      CompileBroker::compile_method(mh, InvocationEntryBci, CompLevel_full_optimization, mh, hot_count, CompileTask::Reason_Bootstrap, THREAD);
    }
  }

  int qsize;
  bool first_round = true;
  int z = 0;
  do {
    // Loop until there is something in the queue.
    do {
      os::sleep(THREAD, 100, true);
      qsize = CompileBroker::queue_size(CompLevel_full_optimization);
    } while (!_bootstrap_compilation_request_handled && first_round && qsize == 0);
    first_round = false;
    if (PrintBootstrap) {
      while (z < (_methods_compiled / 100)) {
        ++z;
        tty->print_raw(".");
      }
    }
  } while (qsize != 0);

  if (PrintBootstrap) {
    tty->print_cr(" in " JLONG_FORMAT " ms (compiled %d methods)", os::javaTimeMillis() - start, _methods_compiled);
  }
  _bootstrapping = false;
  JVMCI::java_runtime()->bootstrap_finished(CHECK);
}

bool JVMCICompiler::force_comp_at_level_simple(Method *method) {
  if (_bootstrapping) {
    // When bootstrapping, the JVMCI compiler can compile its own methods.
    return false;
  }
  if (UseJVMCINativeLibrary) {
    // This mechanism exists to force compilation of a JVMCI compiler by C1
    // to reduce the compilation time spent on the JVMCI compiler itself. In
    // +UseJVMCINativeLibrary mode, the JVMCI compiler is AOT compiled.
    return false;
  } else {
    JVMCIRuntime* runtime = JVMCI::java_runtime();
    if (runtime != NULL) {
      JVMCIObject receiver = runtime->probe_HotSpotJVMCIRuntime();
      if (receiver.is_null()) {
        return false;
      }
      JVMCIEnv* ignored_env = NULL;
      objArrayHandle excludeModules(JavaThread::current(), HotSpotJVMCI::HotSpotJVMCIRuntime::excludeFromJVMCICompilation(ignored_env, HotSpotJVMCI::resolve(receiver)));
      if (excludeModules.not_null()) {
        ModuleEntry* moduleEntry = method->method_holder()->module();
        for (int i = 0; i < excludeModules->length(); i++) {
          if (excludeModules->obj_at(i) == moduleEntry->module()) {
            return true;
          }
        }
      }
    }
    return false;
  }
}

// Compilation entry point for methods
void JVMCICompiler::compile_method(ciEnv* env, ciMethod* target, int entry_bci, DirectiveSet* directive) {
  ShouldNotReachHere();
}

// Print CompileBroker compilation timers
void JVMCICompiler::print_timers() {
  double code_install_time = _codeInstallTimer.seconds();
  tty->print_cr("    JVMCI CompileBroker Time:");
  tty->print_cr("       Compile:        %7.3f s", stats()->total_time());
  tty->print_cr("       Install Code:   %7.3f s", code_install_time);
}

// Print non-CompileBroker compilation timers
void JVMCICompiler::print_hosted_timers() {
  double code_install_time = _hostedCodeInstallTimer.seconds();
  tty->print_cr("    JVMCI Hosted Time:");
  tty->print_cr("       Install Code:   %7.3f s", code_install_time);
}

void JVMCICompiler::inc_methods_compiled() {
  Atomic::inc(&_methods_compiled);
  Atomic::inc(&_global_compilation_ticks);
}

void JVMCICompiler::inc_global_compilation_ticks() {
  Atomic::inc(&_global_compilation_ticks);
}
