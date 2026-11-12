/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <cstdio>
#include <cstring>
#include "util/path.h"
#include "kernel/integrator/deep_camera_receipt.h"

CCL_NAMESPACE_BEGIN

/* Streaming immutable diagnostic input. Requires a trusted successful joint producer in-process;
 * this does not replace the outer successful frame/process or production SceneEpoch contract. */
class DeepCameraReceiptReader {
 public:
  ~DeepCameraReceiptReader()
  {
    if (file_) {
      fclose(file_);
    }
  }

  static bool sealed_header(const string &path,
                            uint32_t magic,
                            uint32_t record_bytes,
                            uint64_t count,
                            uint32_t (&header)[32])
  {
    if (!record_bytes || count > (SIZE_MAX - sizeof(header) - 32) / record_bytes ||
        path_file_size(path) != sizeof(header) + size_t(count) * record_bytes + 32)
    {
      return false;
    }
    uint32_t marker[8] = {};
    FILE *seal = path_fopen(path + ".sealed", "rb");
    if (!seal) {
      return false;
    }
    const bool seal_ok = fread(marker, 1, sizeof(marker), seal) == sizeof(marker) &&
                         fgetc(seal) == EOF && !ferror(seal);
    const bool seal_closed = fclose(seal) == 0;
    FILE *input = path_fopen(path, "rb");
    if (!input) {
      return false;
    }
    uint32_t footer[8] = {};
    const bool input_ok = fread(header, 1, sizeof(header), input) == sizeof(header) &&
                          fseek(input, -long(sizeof(footer)), SEEK_END) == 0 &&
                          fread(footer, 1, sizeof(footer), input) == sizeof(footer) &&
                          fgetc(input) == EOF && !ferror(input);
    const bool input_closed = fclose(input) == 0;
    return seal_ok && seal_closed && input_ok && input_closed &&
           marker[0] == 0x4449534c && marker[1] == magic && marker[2] == record_bytes &&
           marker[3] == 1 && (uint64_t(marker[4]) | (uint64_t(marker[5]) << 32)) == count &&
           header[0] == magic && header[1] == 1 && header[2] == record_bytes &&
           footer[0] == magic - 2 && footer[1] == 1 && footer[2] == 1 && footer[3] == 0 &&
           memcmp(footer + 4, marker + 4, 4 * sizeof(uint32_t)) == 0;
  }

  bool begin(const string &path, uint64_t count, const uint32_t (&header)[32])
  {
    if (file_) {
      return false;
    }
    file_ = path_fopen(path, "rb");
    remaining_ = count;
    uint32_t actual[32] = {};
    return file_ && fread(actual, 1, sizeof(actual), file_) == sizeof(actual) &&
           memcmp(actual, header, sizeof(actual)) == 0;
  }

  bool read(DeepCameraReceipt *destination, size_t count)
  {
    if (!file_ || !destination || count == 0 || count > remaining_ ||
        count > SIZE_MAX / sizeof(DeepCameraReceipt))
    {
      return false;
    }
    if (fread(destination, sizeof(DeepCameraReceipt), count, file_) != count) {
      return false;
    }
    remaining_ -= count;
    return true;
  }

  bool finish(uint64_t count)
  {
    uint32_t footer[8] = {};
    const bool ok = file_ && remaining_ == 0 &&
                    fread(footer, 1, sizeof(footer), file_) == sizeof(footer) &&
                    footer[0] == 0x44435246 && footer[1] == 1 && footer[2] == 1 &&
                    footer[3] == 0 &&
                    (uint64_t(footer[4]) | (uint64_t(footer[5]) << 32)) == count &&
                    fgetc(file_) == EOF && !ferror(file_);
    const bool closed = file_ && fclose(file_) == 0;
    file_ = nullptr;
    return ok && closed;
  }

 private:
  FILE *file_ = nullptr;
  uint64_t remaining_ = 0;
};

CCL_NAMESPACE_END
