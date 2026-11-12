/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_CUDA

#  include "device/cuda/util.h"
#  include "device/cuda/device_impl.h"

CCL_NAMESPACE_BEGIN

namespace {
thread_local unsigned nested_context_query_scopes = 0;
}

CUDAContextScope::CUDAContextScope(CUDADevice *device, const bool enable_nested_queries)
    : device(device), query_scope_(enable_nested_queries)
{
  /* The bounded owner always pushes once to retain its caller's context.
   * Within it, inspect the actual Driver state rather than guessing a cached
   * Device: mixed-device calls and balanced external pushes remain valid. */
  if (!query_scope_ && nested_context_query_scopes) {
    CUcontext current = nullptr;
    const CUresult queried = cuCtxGetCurrent(&current);
    cuda_device_assert(device, queried);
    if (queried != CUDA_SUCCESS || current == device->cuContext) return;
  }
  const CUresult result = cuCtxPushCurrent(device->cuContext);
  cuda_device_assert(device, result);
  pushed_ = result == CUDA_SUCCESS;
  if (query_scope_ && pushed_) ++nested_context_query_scopes;
  else query_scope_ = false;
}

CUDAContextScope::~CUDAContextScope()
{
  if (query_scope_) --nested_context_query_scopes;
  if (pushed_) cuda_device_assert(device, cuCtxPopCurrent(nullptr));
}

#  ifndef WITH_CUDA_DYNLOAD
const char *cuewErrorString(CUresult result)
{
  /* We can only give error code here without major code duplication, that
   * should be enough since dynamic loading is only being disabled by folks
   * who knows what they're doing anyway.
   *
   * NOTE: Avoid call from several threads.
   */
  static string error;
  error = string_printf("%d", result);
  return error.c_str();
}

const char *cuewCompilerPath()
{
  return CYCLES_CUDA_NVCC_EXECUTABLE;
}

int cuewCompilerVersion()
{
  return (CUDA_VERSION / 100) + (CUDA_VERSION % 100 / 10);
}
#  endif

CCL_NAMESPACE_END

#endif /* WITH_CUDA */
