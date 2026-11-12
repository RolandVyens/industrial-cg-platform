/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#define __DEEP_BOUNDED_POLICY__ 1
#define DEEP_INIT_ENTRY deep_alpha_init_from_camera
#define DEEP_MEGA_ENTRY deep_alpha_megakernel
#define DEEP_CPU_AVX2
#include "kernel/device/cpu/deep_kernel_impl.h"
