/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "dalvik_system_ZygoteHooks.h"

#include <stdlib.h>

#include <cutils/process_name.h>

#include "arch/instruction_set.h"
#include "debugger.h"
#include "java_vm_ext.h"
#include "jit/jit.h"
#include "jni_internal.h"
#include "JNIHelp.h"
#include "scoped_thread_state_change.h"
#include "ScopedUtfChars.h"
#include "thread-inl.h"
#include "trace.h"

#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include <sys/resource.h>

namespace art {

static void EnableDebugger() {
#if defined(__linux__)
  // To let a non-privileged gdbserver attach to this
  // process, we must set our dumpable flag.
  if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == -1) {
    PLOG(ERROR) << "prctl(PR_SET_DUMPABLE) failed for pid " << getpid();
  }
#endif
  // We don't want core dumps, though, so set the core dump size to 0.
  rlimit rl;
  rl.rlim_cur = 0;
  rl.rlim_max = RLIM_INFINITY;
  if (setrlimit(RLIMIT_CORE, &rl) == -1) {
    PLOG(ERROR) << "setrlimit(RLIMIT_CORE) failed for pid " << getpid();
  }
}

static void EnableDebugFeatures(uint32_t debug_flags) {
  // Must match values in com.android.internal.os.Zygote.
  enum {
    DEBUG_ENABLE_DEBUGGER           = 1,
    DEBUG_ENABLE_CHECKJNI           = 1 << 1,
    DEBUG_ENABLE_ASSERT             = 1 << 2,
    DEBUG_ENABLE_SAFEMODE           = 1 << 3,
    DEBUG_ENABLE_JNI_LOGGING        = 1 << 4,
    DEBUG_ENABLE_JIT                = 1 << 5,
    DEBUG_GENERATE_DEBUG_INFO       = 1 << 6,
  };

  Runtime* const runtime = Runtime::Current();
  if ((debug_flags & DEBUG_ENABLE_CHECKJNI) != 0) {
    JavaVMExt* vm = runtime->GetJavaVM();
    if (!vm->IsCheckJniEnabled()) {
      LOG(INFO) << "Late-enabling -Xcheck:jni";
      vm->SetCheckJniEnabled(true);
      // There's only one thread running at this point, so only one JNIEnv to fix up.
      Thread::Current()->GetJniEnv()->SetCheckJniEnabled(true);
    } else {
      LOG(INFO) << "Not late-enabling -Xcheck:jni (already on)";
    }
    debug_flags &= ~DEBUG_ENABLE_CHECKJNI;
  }

  if ((debug_flags & DEBUG_ENABLE_JNI_LOGGING) != 0) {
    gLogVerbosity.third_party_jni = true;
    debug_flags &= ~DEBUG_ENABLE_JNI_LOGGING;
  }

  Dbg::SetJdwpAllowed((debug_flags & DEBUG_ENABLE_DEBUGGER) != 0);
  if ((debug_flags & DEBUG_ENABLE_DEBUGGER) != 0) {
    EnableDebugger();
  }
  debug_flags &= ~DEBUG_ENABLE_DEBUGGER;

  const bool safe_mode = (debug_flags & DEBUG_ENABLE_SAFEMODE) != 0;
  if (safe_mode) {
    // Ensure that any (secondary) oat files will be interpreted.
    runtime->AddCompilerOption("--compiler-filter=interpret-only");
    debug_flags &= ~DEBUG_ENABLE_SAFEMODE;
  }

  bool use_jit = false;
  if ((debug_flags & DEBUG_ENABLE_JIT) != 0) {
    if (safe_mode) {
      LOG(INFO) << "Not enabling JIT due to safe mode";
    } else {
      use_jit = true;
      LOG(INFO) << "Late-enabling JIT";
    }
    debug_flags &= ~DEBUG_ENABLE_JIT;
  }
  runtime->GetJITOptions()->SetUseJIT(use_jit);

  const bool generate_debug_info = (debug_flags & DEBUG_GENERATE_DEBUG_INFO) != 0;
  if (generate_debug_info) {
    runtime->AddCompilerOption("--generate-debug-info");
    debug_flags &= ~DEBUG_GENERATE_DEBUG_INFO;
  }

  // This is for backwards compatibility with Dalvik.
  debug_flags &= ~DEBUG_ENABLE_ASSERT;

  if (debug_flags != 0) {
    LOG(ERROR) << StringPrintf("Unknown bits set in debug_flags: %#x", debug_flags);
  }
}

static jlong ZygoteHooks_nativePreFork(JNIEnv* env, jclass) {
  Runtime* runtime = Runtime::Current();
  CHECK(runtime->IsZygote()) << "runtime instance not started with -Xzygote";

  runtime->PreZygoteFork();

  if (Trace::GetMethodTracingMode() != TracingMode::kTracingInactive) {
    // Tracing active, pause it.
    Trace::Pause();
  }

  // Grab thread before fork potentially makes Thread::pthread_key_self_ unusable.
  return reinterpret_cast<jlong>(ThreadForEnv(env));
}

static void ZygoteHooks_nativePostForkChild(JNIEnv* env, jclass, jlong token, jint debug_flags,
                                            jstring instruction_set) {
  Thread* thread = reinterpret_cast<Thread*>(token);
  // Our system thread ID, etc, has changed so reset Thread state.
  thread->InitAfterFork();
  EnableDebugFeatures(debug_flags);

  // Update tracing.
  if (Trace::GetMethodTracingMode() != TracingMode::kTracingInactive) {
    Trace::TraceOutputMode output_mode = Trace::GetOutputMode();
    Trace::TraceMode trace_mode = Trace::GetMode();
    size_t buffer_size = Trace::GetBufferSize();

    // Just drop it.
    Trace::Abort();

    // Only restart if it was streaming mode.
    // TODO: Expose buffer size, so we can also do file mode.
    if (output_mode == Trace::TraceOutputMode::kStreaming) {
      const char* proc_name_cutils = get_process_name();
      std::string proc_name;
      if (proc_name_cutils != nullptr) {
        proc_name = proc_name_cutils;
      }
      if (proc_name_cutils == nullptr || proc_name == "zygote" || proc_name == "zygote64") {
        // Either no process name, or the name hasn't been changed, yet. Just use pid.
        pid_t pid = getpid();
        proc_name = StringPrintf("%u", static_cast<uint32_t>(pid));
      }

      std::string profiles_dir(GetDalvikCache("profiles", false /* create_if_absent */));
      if (!profiles_dir.empty()) {
        std::string trace_file = StringPrintf("%s/%s.trace.bin", profiles_dir.c_str(),
                                              proc_name.c_str());
        Trace::Start(trace_file.c_str(),
                     -1,
                     buffer_size,
                     0,   // TODO: Expose flags.
                     output_mode,
                     trace_mode,
                     0);  // TODO: Expose interval.
        if (thread->IsExceptionPending()) {
          ScopedObjectAccess soa(env);
          thread->ClearException();
        }
      } else {
        LOG(ERROR) << "Profiles dir is empty?!?!";
      }
    }
  }

  if (instruction_set != nullptr) {
    ScopedUtfChars isa_string(env, instruction_set);
    InstructionSet isa = GetInstructionSetFromString(isa_string.c_str());
    Runtime::NativeBridgeAction action = Runtime::NativeBridgeAction::kUnload;
    if (isa != kNone && isa != kRuntimeISA) {
      action = Runtime::NativeBridgeAction::kInitialize;
    }
    Runtime::Current()->DidForkFromZygote(env, action, isa_string.c_str());
  } else {
    Runtime::Current()->DidForkFromZygote(env, Runtime::NativeBridgeAction::kUnload, nullptr);
  }
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(ZygoteHooks, nativePreFork, "()J"),
  NATIVE_METHOD(ZygoteHooks, nativePostForkChild, "(JILjava/lang/String;)V"),
};

void register_dalvik_system_ZygoteHooks(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("dalvik/system/ZygoteHooks");
}

}  // namespace art
