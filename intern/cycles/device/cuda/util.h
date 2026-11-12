/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_CUDA

#  ifdef WITH_CUDA_DYNLOAD
#    include <cuew.h>
#  else
#    include <cuda.h>
#  endif

CCL_NAMESPACE_BEGIN

class CUDADevice;

/* Utility to push/pop CUDA context. */
class CUDAContextScope {
 public:
  CUDAContextScope(CUDADevice *device, bool enable_nested_queries = false);
  CUDAContextScope(const CUDAContextScope &) = delete;
  CUDAContextScope &operator=(const CUDAContextScope &) = delete;
  ~CUDAContextScope();

 private:
  CUDADevice *device;
  bool query_scope_ = false;
  bool pushed_ = false;
};

/* Utility for checking return values of CUDA function calls. */
#  define cuda_device_assert(cuda_device, stmt) \
    { \
      const CUresult cuda_assert_status_ = (stmt); \
      if (cuda_assert_status_ != CUDA_SUCCESS) { \
        const char *name = cuewErrorString(cuda_assert_status_); \
        cuda_device->set_error( \
            string_printf("%s in %s (%s:%d)", name, #stmt, __FILE__, __LINE__)); \
      } \
    } \
    (void)0

#  define cuda_assert(stmt) cuda_device_assert(this, stmt)

#  ifndef WITH_CUDA_DYNLOAD
/* Transparently implement some functions, so majority of the file does not need
 * to worry about difference between dynamically loaded and linked CUDA at all. */
const char *cuewErrorString(CUresult result);
const char *cuewCompilerPath();
int cuewCompilerVersion();
#  endif /* WITH_CUDA_DYNLOAD */

static inline bool cudaSupportsDevice(const int cudaDevID)
{
  int major;
  cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, cudaDevID);
  if (major >= 5) {
    return true;
  }
  return false;
}

CCL_NAMESPACE_END

#endif /* WITH_CUDA */
