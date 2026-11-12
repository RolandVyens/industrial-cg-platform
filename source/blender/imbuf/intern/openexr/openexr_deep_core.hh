/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>

#include "IMB_openexr.hh"

namespace blender {

/* Private synchronous I/O seam. Implementations serialize offset writes and
 * explicitly check flush/close. A destructor is never a success acknowledgement. */
class DeepExrCoreStream {
 public:
  virtual ~DeepExrCoreStream() = default;
  virtual bool write(uint64_t offset, const void *data, size_t bytes) = 0;
  virtual bool finish_checked() = 0;
};

bool imb_exr_save_deep_rows_core(DeepExrRowReader reader,
                                 const DeepExrWriteParams &params,
                                 const char *filepath,
                                 const DeepExrMemoryCallbacks *memory,
                                 DeepExrWriteReport *report,
                                 DeepExrCoreStream *stream = nullptr);

bool imb_exr_save_deep_packed_rows_core(DeepExrPackedRowReader reader,
                                        const DeepExrWriteParams &params,
                                        const char *filepath,
                                        const DeepExrMemoryCallbacks *memory,
                                        DeepExrWriteReport *report);

}  // namespace blender
