/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <cstdlib>
#include <cstring>

#include "testing/testing.h"
#include "integrator/deep_camera_receipt_writer.h"
#include "integrator/deep_input_receipt_reader.h"
#include "kernel/integrator/deep_optical_event.h"

CCL_NAMESPACE_BEGIN

TEST(DeepCameraReceipt, streaming_reader_preserves_full_sample_and_requires_closed_population)
{
  const char *root = getenv("CYCLES_CAMERA_RECEIPT_TEST_ROOT");
  if (!root) {
    GTEST_SKIP() << "CYCLES_CAMERA_RECEIPT_TEST_ROOT is required";
  }
  const string path = path_join(root, "reader-full-sample.receipts");
  const uint32_t header[32] = {0x44435248, 1, 88, 2, 0xffffffffu, 0x12345678u};
  DeepCameraReceipt records[2] = {};
  records[0].sample = 0xffffffffu;
  records[0].pixel = 0xfffffffeu;
  records[1].sample = 0x01000001u;
  records[1].tmax = 1234.5f;
  DeepCameraReceiptWriter writer;
  ASSERT_TRUE(writer.begin(path, header));
  ASSERT_TRUE(writer.append(records, 2));
  uint32_t read_header[32] = {};
  EXPECT_FALSE(DeepCameraReceiptReader::sealed_header(path, 0x44435248, 88, 2, read_header));
  ASSERT_TRUE(writer.finish(true));
  ASSERT_TRUE(DeepCameraReceiptReader::sealed_header(path, 0x44435248, 88, 2, read_header));
  EXPECT_FALSE(DeepCameraReceiptReader::sealed_header(path, 0x44435248, 88, 1, read_header));
  EXPECT_FALSE(DeepCameraReceiptReader::sealed_header(path, 0x44535248, 96, 2, read_header));
  DeepCameraReceiptReader reader;
  ASSERT_TRUE(reader.begin(path, 2, header));
  DeepCameraReceipt actual[2] = {};
  ASSERT_TRUE(reader.read(actual, 1));
  ASSERT_TRUE(reader.read(actual + 1, 1));
  EXPECT_FALSE(reader.read(actual, 1));
  ASSERT_TRUE(reader.finish(2));
  EXPECT_EQ(memcmp(actual, records, sizeof(records)), 0);
}

TEST(DeepOpticalBacking, event_receipt_identity_and_ledger_are_separate_immutable_records)
{
  const char *root = getenv("CYCLES_CAMERA_RECEIPT_TEST_ROOT");
  if (!root) {
    GTEST_SKIP() << "CYCLES_CAMERA_RECEIPT_TEST_ROOT is required";
  }
  const string path = path_join(root, "optical-events.receipts");
  const uint32_t header[32] = {0x444f4548, 1, 32, 1};
  DeepOpticalEvent event = {};
  event.receipt = 0xffffffffu;
  event.segment = 0x01000001u;
  event.tau[0] = 1.0e-8f;
  DeepInputReceiptWriter<DeepOpticalEvent, 0x444f4548, 0x444f4546> writer;
  ASSERT_TRUE(writer.begin(path, header));
  ASSERT_TRUE(writer.append(&event, 1));
  ASSERT_TRUE(writer.finish(true));
  vector<uint8_t> bytes;
  ASSERT_TRUE(path_read_binary(path, bytes));
  ASSERT_EQ(bytes.size(), 128 + 32 + 32);
  EXPECT_EQ(memcmp(bytes.data() + 128, &event, sizeof(event)), 0);
  const string cancelled = path_join(root, "optical-ledger-cancelled.receipts");
  const uint32_t ledger_header[32] = {0x444f4c48, 1, 16, 1};
  DeepInputReceiptWriter<DeepOpticalLedger, 0x444f4c48, 0x444f4c46> ledger_writer;
  DeepOpticalLedger ledger = {DEEP_OPTICAL_COMPLETE, 1, 1, 0};
  ASSERT_TRUE(ledger_writer.begin(cancelled, ledger_header));
  ASSERT_TRUE(ledger_writer.append(&ledger, 1));
  ASSERT_TRUE(ledger_writer.finish(false));
  EXPECT_FALSE(path_exists(cancelled + ".sealed"));
}

TEST(DeepCameraReceipt, rejects_truncated_surface_with_matching_terminal_and_seal)
{
  const char *root = getenv("CYCLES_CAMERA_RECEIPT_TEST_ROOT");
  if (!root) {
    GTEST_SKIP() << "CYCLES_CAMERA_RECEIPT_TEST_ROOT is required";
  }
  const string path = path_join(root, "truncated-surface.receipts");
  const uint32_t header[32] = {0x44535248, 1, 96, 2};
  const uint32_t footer[8] = {0x44535246, 1, 1, 0, 2, 0, 1, 0};
  const uint32_t marker[8] = {0x4449534c, 0x44535248, 96, 1, 2, 0, 1, 0};
  DeepSurfaceReceipt only_one = {};
  FILE *file = path_fopen(path, "wbx");
  ASSERT_NE(file, nullptr);
  ASSERT_EQ(fwrite(header, 1, sizeof(header), file), sizeof(header));
  ASSERT_EQ(fwrite(&only_one, 1, sizeof(only_one), file), sizeof(only_one));
  ASSERT_EQ(fwrite(footer, 1, sizeof(footer), file), sizeof(footer));
  ASSERT_EQ(fclose(file), 0);
  FILE *seal = path_fopen(path + ".sealed", "wbx");
  ASSERT_NE(seal, nullptr);
  ASSERT_EQ(fwrite(marker, 1, sizeof(marker), seal), sizeof(marker));
  ASSERT_EQ(fclose(seal), 0);
  uint32_t read_header[32];
  EXPECT_FALSE(DeepCameraReceiptReader::sealed_header(path, 0x44535248, 96, 2, read_header));
}

/* Evidence files are retained in an explicitly supplied root. These tests never remove files. */
TEST(DeepCameraReceipt, lossless_batches_and_immutable_complete_domain)
{
  const char *root = getenv("CYCLES_CAMERA_RECEIPT_TEST_ROOT");
  if (!root) {
    GTEST_SKIP() << "CYCLES_CAMERA_RECEIPT_TEST_ROOT is required for retained backing evidence";
  }
  const string path = path_join(root, "complete.receipts");
  const uint32_t header[32] = {0x44435248, 1, 88};
  DeepCameraReceipt a = {};
  a.pixel = 11;
  a.sample = 4095;
  a.flags = DEEP_CAMERA_RECEIPT_ZERO_THROUGHPUT;
  a.population_weight = 1;
  DeepCameraReceipt b = a;
  b.sample = 0xffffffffu;
  b.flags = DEEP_CAMERA_RECEIPT_RAY_VALID;
  b.origin[0] = -1.25f;
  b.direction[2] = 1;
  b.tmax = 1e30f;
  b.throughput[0] = 0.375f;
  DeepCameraReceiptWriter writer;
  ASSERT_TRUE(writer.begin(path, header));
  ASSERT_TRUE(writer.append(&a, 1));
  ASSERT_TRUE(writer.append(&b, 1));
  EXPECT_EQ(writer.count(), 2u);
  ASSERT_TRUE(writer.finish(true));
  vector<uint8_t> seal;
  ASSERT_TRUE(path_read_binary(path + ".sealed", seal));
  ASSERT_EQ(seal.size(), 32u);
  uint32_t marker[8];
  memcpy(marker, seal.data(), sizeof(marker));
  EXPECT_EQ(marker[0], 0x4449534cu);
  EXPECT_EQ(marker[1], 0x44435248u);
  EXPECT_EQ(marker[2], 88u);
  EXPECT_EQ(marker[4], 2u);
  DeepCameraReceiptWriter duplicate;
  EXPECT_FALSE(duplicate.begin(path, header));
  vector<uint8_t> bytes;
  ASSERT_TRUE(path_read_binary(path, bytes));
  ASSERT_EQ(bytes.size(), 128 + 2 * 88 + 32);
  EXPECT_EQ(memcmp(bytes.data() + 128, &a, sizeof(a)), 0);
  EXPECT_EQ(memcmp(bytes.data() + 128 + 88, &b, sizeof(b)), 0);
  uint32_t footer[8];
  memcpy(footer, bytes.data() + 128 + 2 * 88, sizeof(footer));
  EXPECT_EQ(footer[0], 0x44435246u);
  EXPECT_EQ(footer[2], 1u);
  EXPECT_EQ(footer[4], 2u);
  EXPECT_EQ(footer[6], 2u);
}

TEST(DeepCameraReceipt, cancel_does_not_seal_population)
{
  const char *root = getenv("CYCLES_CAMERA_RECEIPT_TEST_ROOT");
  if (!root) {
    GTEST_SKIP() << "CYCLES_CAMERA_RECEIPT_TEST_ROOT is required";
  }
  const string path = path_join(root, "cancelled.receipts");
  const uint32_t header[32] = {0x44435248, 1, 88};
  DeepCameraReceipt receipt = {};
  DeepCameraReceiptWriter writer;
  ASSERT_TRUE(writer.begin(path, header));
  ASSERT_TRUE(writer.append(&receipt, 1));
  ASSERT_TRUE(writer.finish(false));
  EXPECT_FALSE(path_exists(path + ".sealed"));
  vector<uint8_t> bytes;
  ASSERT_TRUE(path_read_binary(path, bytes));
  ASSERT_EQ(bytes.size(), 128 + 88 + 32);
  uint32_t complete;
  memcpy(&complete, bytes.data() + 128 + 88 + 8, sizeof(complete));
  EXPECT_EQ(complete, 0u);
}

TEST(DeepCameraReceipt, failed_append_and_destructor_cannot_seal)
{
  const char *root = getenv("CYCLES_CAMERA_RECEIPT_TEST_ROOT");
  if (!root) {
    GTEST_SKIP() << "CYCLES_CAMERA_RECEIPT_TEST_ROOT is required";
  }
  const string path = path_join(root, "failed.receipts");
  const uint32_t header[32] = {0x44435248, 1, 88};
  {
    DeepCameraReceiptWriter writer;
    ASSERT_TRUE(writer.begin(path, header));
    EXPECT_FALSE(writer.append(nullptr, 1));
    EXPECT_FALSE(writer.finish(true));
  }
  EXPECT_EQ(path_file_size(path), 128u);
  EXPECT_FALSE(path_exists(path + ".sealed"));
  const string incomplete = path_join(root, "interrupted.receipts");
  {
    DeepCameraReceiptWriter writer;
    ASSERT_TRUE(writer.begin(incomplete, header));
  }
  EXPECT_EQ(path_file_size(incomplete), 128u);
  EXPECT_FALSE(path_exists(incomplete + ".sealed"));
}

TEST(DeepSurfaceReceipt, full_sample_identity_and_late_rgb_are_retained)
{
  const char *root = getenv("CYCLES_CAMERA_RECEIPT_TEST_ROOT");
  if (!root) {
    GTEST_SKIP() << "CYCLES_CAMERA_RECEIPT_TEST_ROOT is required";
  }
  const string path = path_join(root, "surface-complete.receipts");
  const uint32_t header[32] = {0x44535248, 1, 96};
  DeepSurfaceReceipt receipts[2] = {};
  receipts[0].sample = 0xffffffffu;
  receipts[0].pixel = 17;
  receipts[0].ordinal = 41;
  receipts[0].rgba[3] = 0.25f;
  receipts[1] = receipts[0];
  receipts[1].ordinal = 42;
  /* A shadow keeps record 0 while its main path now refers to record 1. */
  receipts[0].rgba[0] = 1.75f;
  receipts[1].rgba[0] = -0.125f;
  DeepSurfaceReceiptWriter writer;
  ASSERT_TRUE(writer.begin(path, header));
  ASSERT_TRUE(writer.append(receipts, 2));
  ASSERT_TRUE(writer.finish(true));
  vector<uint8_t> bytes;
  ASSERT_TRUE(path_read_binary(path, bytes));
  ASSERT_EQ(bytes.size(), 128 + 2 * 96 + 32);
  EXPECT_EQ(memcmp(bytes.data() + 128, receipts, sizeof(receipts)), 0);
  EXPECT_EQ(path_file_size(path + ".sealed"), 32u);
}

TEST(DeepSurfaceReceipt, empty_domain_and_cancel_have_distinct_completion)
{
  const char *root = getenv("CYCLES_CAMERA_RECEIPT_TEST_ROOT");
  if (!root) {
    GTEST_SKIP() << "CYCLES_CAMERA_RECEIPT_TEST_ROOT is required";
  }
  const uint32_t header[32] = {0x44535248, 1, 96};
  const string path = path_join(root, "surface-empty.receipts");
  DeepSurfaceReceiptWriter empty;
  ASSERT_TRUE(empty.begin(path, header));
  ASSERT_TRUE(empty.finish(true));
  EXPECT_EQ(path_file_size(path), 160u);
  EXPECT_EQ(path_file_size(path + ".sealed"), 32u);
  const string cancelled_path = path_join(root, "surface-cancelled.receipts");
  DeepSurfaceReceiptWriter cancelled;
  ASSERT_TRUE(cancelled.begin(cancelled_path, header));
  ASSERT_TRUE(cancelled.finish(false));
  EXPECT_FALSE(path_exists(cancelled_path + ".sealed"));
}

TEST(DeepSurfaceReceipt, marker_collision_and_wrong_schema_cannot_publish)
{
  const char *root = getenv("CYCLES_CAMERA_RECEIPT_TEST_ROOT");
  if (!root) {
    GTEST_SKIP() << "CYCLES_CAMERA_RECEIPT_TEST_ROOT is required";
  }
  const uint32_t header[32] = {0x44535248, 1, 96};
  const string path = path_join(root, "surface-marker-collision.receipts");
  DeepSurfaceReceiptWriter writer;
  ASSERT_TRUE(writer.begin(path, header));
  FILE *collision = path_fopen(path + ".sealed", "wbx");
  ASSERT_NE(collision, nullptr);
  const char sentinel = 'x';
  ASSERT_EQ(fwrite(&sentinel, 1, 1, collision), 1u);
  ASSERT_EQ(fclose(collision), 0);
  EXPECT_FALSE(writer.finish(true));
  EXPECT_EQ(path_file_size(path + ".sealed"), 1u);
  DeepSurfaceReceiptWriter retry;
  EXPECT_FALSE(retry.begin(path, header));
  const uint32_t wrong[32] = {0x44435248, 1, 88};
  const string wrong_path = path_join(root, "surface-wrong-schema.receipts");
  DeepSurfaceReceiptWriter wrong_writer;
  EXPECT_FALSE(wrong_writer.begin(wrong_path, wrong));
  EXPECT_FALSE(path_exists(wrong_path));
}

TEST(DeepVolumeRadianceReceipt, late_shadow_full_identity_and_failed_domain)
{
  const char *root = getenv("CYCLES_CAMERA_RECEIPT_TEST_ROOT");
  if (!root) {
    GTEST_SKIP() << "CYCLES_CAMERA_RECEIPT_TEST_ROOT is required";
  }
  const uint32_t header[32] = {0x44565248, 1, 96};
  DeepVolumeRadianceReceipt receipts[2] = {};
  receipts[0].pixel = 17;
  receipts[0].sample = 0xffffffffu;
  receipts[0].rng_pixel = 31;
  receipts[0].tmin = 0.125f;
  receipts[0].tmax = 3.0f;
  receipts[1] = receipts[0];
  receipts[1].sample = 41;
  /* Distinct shadow ownership stays with sample UINT32_MAX after main storage advances. */
  receipts[0].unowned_shadow[0] = 1.75f;
  receipts[0].emission[1] = 0.25f;
  receipts[1].unowned_path[0] = -0.125f;
  const string path = path_join(root, "volume-radiance-complete.receipts");
  DeepVolumeRadianceReceiptWriter writer;
  ASSERT_TRUE(writer.begin(path, header));
  ASSERT_TRUE(writer.append(receipts, 2));
  ASSERT_TRUE(writer.finish(true));
  uint32_t actual[32] = {};
  ASSERT_TRUE(DeepCameraReceiptReader::sealed_header(path, 0x44565248, 96, 2, actual));
  EXPECT_FALSE(DeepCameraReceiptReader::sealed_header(path, 0x44565248, 96, 1, actual));
  vector<uint8_t> bytes;
  ASSERT_TRUE(path_read_binary(path, bytes));
  ASSERT_EQ(bytes.size(), 128 + sizeof(receipts) + 32);
  EXPECT_EQ(memcmp(bytes.data() + 128, receipts, sizeof(receipts)), 0);
  const string failed = path_join(root, "volume-radiance-failed.receipts");
  DeepVolumeRadianceReceiptWriter failed_writer;
  ASSERT_TRUE(failed_writer.begin(failed, header));
  ASSERT_TRUE(failed_writer.append(receipts, 1));
  ASSERT_TRUE(failed_writer.finish(false));
  EXPECT_FALSE(DeepCameraReceiptReader::sealed_header(failed, 0x44565248, 96, 1, actual));
  EXPECT_FALSE(path_exists(failed + ".sealed"));
}

CCL_NAMESPACE_END
