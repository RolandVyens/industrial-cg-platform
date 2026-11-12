/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#define __DEEP_BOUNDED_POLICY__ 2
/* This module only exports the two native volume entries. Keep the common
 * shader configuration unchanged while testing more resident short waves. */
#ifdef __CUDA_ARCH__
#  include "kernel/device/cuda/compat.h"
#  include "kernel/device/cuda/config.h"
#  undef GPU_KERNEL_BLOCK_NUM_THREADS
#  undef GPU_KERNEL_MAX_REGISTERS
#  define GPU_KERNEL_BLOCK_NUM_THREADS 64
#  define GPU_KERNEL_MAX_REGISTERS 64
#  include "kernel/device/cuda/globals.h"
#  include "kernel/device/gpu/image.h"
#  include "kernel/device/gpu/kernel.h"
#endif
