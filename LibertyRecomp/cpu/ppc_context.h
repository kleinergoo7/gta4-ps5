#pragma once
// RexGlue's PPC types are authoritative. graine SDK 0.7.5 renamed the generated
// project header from `gta4_config.h` → `gta4_init.h` and the image/code-size
// defines from `PPC_*` → `REX_*`. Keep the old PPC_* aliases so Liberty's
// kernel/memory.cpp + main.cpp don't need wholesale renames.
#include "../../glue/rexglue-sdk-main/gta4-recomp/generated/gta4_init.h"
#ifndef PPC_IMAGE_BASE
  #define PPC_IMAGE_BASE REX_IMAGE_BASE
  #define PPC_IMAGE_SIZE REX_IMAGE_SIZE
  #define PPC_CODE_BASE  REX_CODE_BASE
  #define PPC_CODE_SIZE  REX_CODE_SIZE
#endif
#ifndef PPC_MEMORY_SIZE
  #define PPC_MEMORY_SIZE REX_MEMORY_SIZE
#endif
#ifndef PPC_LOOKUP_FUNC
  #define PPC_LOOKUP_FUNC REX_LOOKUP_FUNC
#endif
// Remaining PPC_* helpers map onto their REX_* replacements (Linux builds
// are unaffected: REX_PHYS_HOST_OFFSET is 0 here, and the indirect-call
// macro only gains a bounds check plus profiler hook).
#ifndef PPC_FUNC
  #define PPC_FUNC REX_FUNC
#endif
#ifndef PPC_FUNC_IMPL
  #define PPC_FUNC_IMPL(x) REX_EXTERN(x)
#endif
#ifndef PPC_FUNC_PROLOGUE
  #define PPC_FUNC_PROLOGUE REX_FUNC_PROLOGUE
#endif
#ifndef PPC_CALL_INDIRECT_FUNC
  #define PPC_CALL_INDIRECT_FUNC REX_CALL_INDIRECT_FUNC
#endif
#ifndef PPC_LOAD_U8
  #define PPC_LOAD_U8 REX_LOAD_U8
#endif
#ifndef PPC_LOAD_U16
  #define PPC_LOAD_U16 REX_LOAD_U16
#endif
#ifndef PPC_LOAD_U32
  #define PPC_LOAD_U32 REX_LOAD_U32
#endif
#ifndef PPC_LOAD_U64
  #define PPC_LOAD_U64 REX_LOAD_U64
#endif
#ifndef PPC_STORE_U8
  #define PPC_STORE_U8 REX_STORE_U8
#endif
#ifndef PPC_STORE_U16
  #define PPC_STORE_U16 REX_STORE_U16
#endif
#ifndef PPC_STORE_U32
  #define PPC_STORE_U32 REX_STORE_U32
#endif
#ifndef PPC_STORE_U64
  #define PPC_STORE_U64 REX_STORE_U64
#endif
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>

// SDK v0.7.4: Thread-local PPC context accessed via rex::runtime::current_ppc_context()
#include <rex/system/thread_state.h>

inline PPCContext* GetPPCContext()
{
    return rex::runtime::current_ppc_context();
}

inline void SetPPCContext(PPCContext& /* ctx */)
{
    // In 0.7.4, the context is per-thread via ThreadState::Bind().
    // SetPPCContext is a no-op — the context is managed by the runtime.
}
