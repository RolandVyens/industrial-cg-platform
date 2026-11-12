/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <limits>

#include "kernel/integrator/deep_camera_receipt.h"
#include "kernel/integrator/deep_surface_receipt.h"
#include "kernel/integrator/deep_volume_radiance_receipt.h"
#include "kernel/integrator/deep_volume_emission_span.h"
#include "util/path.h"

CCL_NAMESPACE_BEGIN

/* M1 diagnostic backing. Consumption requires the footer, a close-success sidecar and
 * the caller's successful process/frame manifest.
 * No destructor can seal cancelled/failed work, and an existing file is never overwritten.
 * Scene identity is bound by the caller's exclusive evidence root, not inferred from a pointer. */
template<typename Record, uint32_t HeaderMagic, uint32_t FooterMagic>
class DeepInputReceiptWriter {
 public:
  ~DeepInputReceiptWriter()
  {
    if (file_) {
      fclose(file_);
    }
  }
  DeepInputReceiptWriter() = default;
  DeepInputReceiptWriter(const DeepInputReceiptWriter &) = delete;
  DeepInputReceiptWriter &operator=(const DeepInputReceiptWriter &) = delete;

  bool begin(const string &path, const uint32_t (&header)[32])
  {
    if (file_ || header[0] != HeaderMagic || header[1] != 1 ||
        header[2] != sizeof(Record) || path_exists(path + ".sealed"))
    {
      return false;
    }
    path_ = path;
    file_ = path_fopen(path, "wbx");
    count_ = 0;
    batches_ = 0;
    failed_ = !file_;
    return file_ && write(header, sizeof(header));
  }

  bool append(const Record *receipts, const size_t count)
  {
    if (!file_ || failed_ || !receipts || count == 0 ||
        count > std::numeric_limits<size_t>::max() / sizeof(Record) ||
        count > std::numeric_limits<uint64_t>::max() - count_)
    {
      failed_ = true;
      return false;
    }
    /* All records in this batch must be committed before the device buffer may be reused. */
    if (!write(receipts, count * sizeof(Record))) {
      return false;
    }
    count_ += count;
    ++batches_;
    return true;
  }

  bool finish(const bool complete)
  {
    if (!file_) {
      return false;
    }
    const uint32_t footer[8] = {FooterMagic,
                                1,
                                uint32_t(complete && !failed_),
                                0,
                                uint32_t(count_),
                                uint32_t(count_ >> 32),
                                uint32_t(batches_),
                                uint32_t(batches_ >> 32)};
    const bool written = !failed_ && write(footer, sizeof(footer));
    const bool closed = fclose(file_) == 0;
    file_ = nullptr;
    if (!written || !closed) {
      return false;
    }
    if (!complete) {
      return true;
    }
    /* Publish only after backing fclose succeeds. The external manifest is still required:
     * neither this diagnostic marker nor a per-render_samples domain is a SceneEpoch. */
    FILE *seal = path_fopen(path_ + ".sealed", "wbx");
    if (!seal) {
      return false;
    }
    const uint32_t marker[8] = {0x4449534c, HeaderMagic, sizeof(Record), 1,
                                uint32_t(count_), uint32_t(count_ >> 32),
                                uint32_t(batches_), uint32_t(batches_ >> 32)};
    const bool published = fwrite(marker, 1, sizeof(marker), seal) == sizeof(marker);
    const bool seal_closed = fclose(seal) == 0;
    return published && seal_closed;
  }

  uint64_t count() const
  {
    return count_;
  }

  const string &path() const
  {
    return path_;
  }

 private:
  bool write(const void *data, const size_t bytes)
  {
    failed_ |= fwrite(data, 1, bytes, file_) != bytes;
    return !failed_;
  }
  FILE *file_ = nullptr;
  string path_;
  uint64_t count_ = 0;
  uint64_t batches_ = 0;
  bool failed_ = false;
};

using DeepCameraReceiptWriter =
    DeepInputReceiptWriter<DeepCameraReceipt, 0x44435248, 0x44435246>;
using DeepSurfaceReceiptWriter =
    DeepInputReceiptWriter<DeepSurfaceReceipt, 0x44535248, 0x44535246>;
using DeepVolumeRadianceReceiptWriter =
    DeepInputReceiptWriter<DeepVolumeRadianceReceipt, 0x44565248, 0x44565246>;
using DeepVolumeEmissionSpanWriter =
    DeepInputReceiptWriter<DeepVolumeEmissionSpan, 0x44455248, 0x44455246>;

CCL_NAMESPACE_END
