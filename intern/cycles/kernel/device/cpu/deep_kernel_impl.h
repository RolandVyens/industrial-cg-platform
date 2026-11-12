/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* The CPU capture adapter uses an explicit platform discriminator; ordinary
 * CPU kernels retain their existing feature configuration. */
#define __KERNEL_CPU__

#ifdef DEEP_CPU_AVX2
#  include "util/optimization.h"

#  ifndef WITH_CYCLES_OPTIMIZED_KERNEL_AVX2
#    define KERNEL_STUB
#  else
/* SSE optimization disabled for now on 32 bit, see bug #36316. */
#    if !(defined(__GNUC__) && (defined(i386) || defined(_M_IX86)))
#      define __KERNEL_SSE__
#      define __KERNEL_SSE2__
#      define __KERNEL_SSE3__
#      define __KERNEL_SSSE3__
#      define __KERNEL_SSE42__
#      define __KERNEL_AVX__
#      define __KERNEL_AVX2__
#    endif
#  endif /* WITH_CYCLES_OPTIMIZED_KERNEL_AVX2 */

#else
#  if defined(__x86_64__) || defined(_M_X64)
#    define __KERNEL_SSE__
#    define __KERNEL_SSE2__
#    define __KERNEL_SSE3__
#    define __KERNEL_SSSE3__
#    define __KERNEL_SSE42__
#  endif

/* When building kernel for native machine detect kernel features from the flags
 * set by compiler.
 */
#  ifdef WITH_KERNEL_NATIVE
#    ifdef __SSE4_2__
#      ifndef __KERNEL_SSE42__
#        define __KERNEL_SSE42__
#      endif
#    endif
#    ifdef __AVX__
#      ifndef __KERNEL_SSE__
#        define __KERNEL_SSE__
#      endif
#      define __KERNEL_AVX__
#    endif
#    ifdef __AVX2__
#      ifndef __KERNEL_SSE__
#        define __KERNEL_SSE__
#      endif
#      define __KERNEL_AVX2__
#    endif
#  endif

/* quiet unused define warnings */
#  if defined(__KERNEL_SSE2__)
/* do nothing */
#  endif

#endif

#include <cmath>

#include "kernel/device/cpu/compat.h"
#include "kernel/device/cpu/globals.h"
#include "kernel/device/cpu/kernel.h"

/* The declaration header visits both ISAs and leaves KERNEL_ARCH set to the
 * last one. Select the actual translation unit after those declarations. */
#undef KERNEL_ARCH
#ifdef DEEP_CPU_AVX2
#  define KERNEL_ARCH cpu_avx2
#else
#  define KERNEL_ARCH cpu
#endif

#ifndef KERNEL_STUB
#  include "kernel/device/cpu/image.h"
#  include "kernel/globals.h"
#  include "kernel/integrator/state.h"

/* Arithmetic is shared verbatim; its implementation types are private to
 * this policy/ISA translation unit, so ISA COMDATs cannot be interchanged. */
namespace {
using std::isfinite;
#  define __device__
#  define __forceinline__ ccl_always_inline
#  define __noinline__ ccl_never_inline
inline uint32_t atomicOr(uint32_t *p, const uint32_t v)
{
  const uint32_t before = *p;
  *p |= v;
  return before;
}
inline void atomicAdd(float *p, const float v)
{
  /* Main, shadow and AO all belong to this synchronous CPU camera owner. */
  *p += v;
}
#  include "kernel/integrator/deep_bounded_records.h"
}  // namespace

// clang-format off
#  include "kernel/integrator/state_flow.h"
#  include "kernel/integrator/state_util.h"
#  include "kernel/integrator/init_from_camera.h"
#  include "kernel/integrator/megakernel.h"
// clang-format on
#  undef __noinline__
#  undef __forceinline__
#  undef __device__
#endif

CCL_NAMESPACE_BEGIN

bool KERNEL_NAME_EVAL(KERNEL_ARCH, DEEP_INIT_ENTRY)(const ThreadKernelGlobalsCPU *kg,
                                                    IntegratorStateCPU *state,
                                                    KernelWorkTile *tile,
                                                    float *render_buffer)
{
#ifdef KERNEL_STUB
  (void)kg;
  (void)state;
  (void)tile;
  (void)render_buffer;
  assert(false && "Unavailable bounded CPU kernel ISA");
  return false;
#else
  return integrator_init_from_camera(
      kg, state, tile, render_buffer, tile->x, tile->y, tile->start_sample);
#endif
}

void KERNEL_NAME_EVAL(KERNEL_ARCH, DEEP_MEGA_ENTRY)(const ThreadKernelGlobalsCPU *kg,
                                                    IntegratorStateCPU *state,
                                                    float *render_buffer)
{
#ifdef KERNEL_STUB
  (void)kg;
  (void)state;
  (void)render_buffer;
  assert(false && "Unavailable bounded CPU kernel ISA");
#else
  integrator_megakernel(kg, state, render_buffer);
#endif
}

CCL_NAMESPACE_END
