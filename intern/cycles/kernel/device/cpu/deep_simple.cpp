/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#define __DEEP_BOUNDED_POLICY__ 2
#define DEEP_INIT_ENTRY deep_simple_init_from_camera
#define DEEP_MEGA_ENTRY deep_simple_megakernel
#include "kernel/device/cpu/deep_kernel_impl.h"
