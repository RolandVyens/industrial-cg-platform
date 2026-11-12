/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#include "integrator/deep_gpu_frame_work.h"
#include "device/device.h"
#include "kernel/integrator/state.h"
#include "session/buffers.h"
#include "util/path.h"
#include "util/string.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

CCL_NAMESPACE_BEGIN

namespace {
uint32_t witness_blocks(const uint64_t n) { return uint32_t((n + 255) / 256); }
bool witness_json_string(FILE *file, const string &value)
{
  if (std::fputc('"', file) == EOF) return false;
  for (const unsigned char c : value) {
    if (c == '"' || c == '\\') { if (std::fputc('\\', file) == EOF) return false; }
    if (c < 32) { if (std::fprintf(file, "\\u%04x", unsigned(c)) < 0) return false; }
    else if (std::fputc(c, file) == EOF) return false;
  }
  return std::fputc('"', file) != EOF;
}
}  // namespace

bool DeepGPUFrameWork::begin_witness()
{
  if (!witness_configured_) {
    witness_configured_ = true;
    const char *values = std::getenv("CYCLES_EXPERIMENTAL_DEEP_WITNESS_CUTS");
    const char *root = std::getenv("CYCLES_EXPERIMENTAL_DEEP_WITNESS_ROOT");
    if (!values && !root) return true;
    if (!values || !*values || !root || !*root || !path_exists(root))
      return fail("Deep witness requires frozen cuts and an existing unique output directory");
    witness_root_ = root;
    for (const char *cursor = values; *cursor;) {
      char *end = nullptr; errno = 0;
      const double depth = std::strtod(cursor, &end);
      if (errno || end == cursor || !std::isfinite(depth) ||
          (*end && *end != ',') || (!witness_cuts_.empty() && depth <= witness_cuts_.back()))
        return fail("Deep witness cuts must be finite, strictly ordered decimal values");
      witness_cuts_.push_back(depth);
      if (!*end) break;
      cursor = end + 1;
      if (!*cursor) return fail("Deep witness cut list has an empty trailing value");
    }
    if (witness_cuts_.empty() || witness_cuts_.size() > UINT32_MAX)
      return fail("Deep witness cut population is empty or overflows metadata");
    witness_.cut_count = uint32_t(witness_cuts_.size());
    if (!witness_cuts_device_->append_raw(*queue_, witness_cuts_.data(), witness_cuts_.size() * sizeof(double)))
      return false;
    witness_.cuts = witness_cuts_device_->pointer();
    if (const char *probes = std::getenv("CYCLES_EXPERIMENTAL_DEEP_TRACE_PIXELS")) {
      for (const char *cursor = probes; *cursor;) {
        char *end = nullptr; errno = 0;
        const long x = std::strtol(cursor, &end, 10);
        if (errno || end == cursor || *end != ':' || x < INT32_MIN || x > INT32_MAX)
          return fail("Deep trace probes require signed integer x:y pairs");
        cursor = end + 1; errno = 0;
        const long y = std::strtol(cursor, &end, 10);
        if (errno || end == cursor || (*end && *end != ',') || y < INT32_MIN || y > INT32_MAX)
          return fail("Deep trace probe y coordinate is invalid");
        trace_probes_.push_back((uint64_t(uint32_t(x)) << 32) | uint32_t(y));
        if (!*end) break;
        cursor = end + 1;
        if (!*cursor) return fail("Deep trace probe list has an empty trailing value");
      }
      if (trace_probes_.empty() || trace_probes_.size() > UINT32_MAX)
        return fail("Deep trace probe population is invalid");
      trace_.probe_count = uint32_t(trace_probes_.size()); trace_.capacity = 65536;
      if (!trace_probes_device_->append_raw(*queue_, trace_probes_.data(), trace_probes_.size() * sizeof(uint64_t)))
        return false;
      trace_.probes = trace_probes_device_->pointer();
    }
  }
  if (!witness_.cut_count) return true;
  const uint32_t width = uint32_t(domain_.core_width ? domain_.core_width : domain_.width);
  const uint32_t height = uint32_t(domain_.core_height ? domain_.core_height : domain_.height);
  const uint64_t count = uint64_t(width) * height * witness_.cut_count;
  if (!width || !height || count > UINT32_MAX || count > SIZE_MAX / (7 * sizeof(double)))
    return fail("Deep witness tile storage exceeds its bounded range");
  if (!witness_log_->used()) {
    witness_.width = width; witness_.height = height;
    witness_.x = domain_.core_width ? domain_.core_x : domain_.x;
    witness_.y = domain_.core_height ? domain_.core_y : domain_.y;
    if (!witness_log_->reserve(*queue_, size_t(count) * sizeof(double), false)) return false;
    witness_log_->set_used(size_t(count) * sizeof(double));
    DeepGPUFrameDispatch d; d.a = witness_log_->pointer(); d.n = uint32_t(count);
    if (!operation(DeepGPUFrameOperation::CompactWitnessInit, d, witness_blocks(count))) return false;
    if (domain_.demand.capture_rgb() && !witness_rgb_->zero(*queue_, size_t(count) * 6 * sizeof(double))) return false;
    if (trace_.probe_count) {
      if (!trace_records_->reserve(*queue_, size_t(trace_.capacity) * sizeof(DeepCompactTraceRecord), false) ||
          !trace_counter_->zero(*queue_, 8) ||
          !trace_raw_->reserve(*queue_, size_t(trace_.capacity) * sizeof(DeepCompactRawTrace), false) ||
          !trace_raw_counter_->zero(*queue_, 8)) return false;
      trace_.records = trace_records_->pointer(); trace_.counter = trace_counter_->pointer();
      trace_.raw_records = trace_raw_->pointer(); trace_.raw_counter = trace_raw_counter_->pointer();
      if (!capture_case_->used() && !capture_case_->zero(*queue_, sizeof(DeepCompactCaptureCase) +
                                                                       sizeof(DeepCompactRayState))) return false;
      trace_.capture_case = capture_case_->pointer();
      if (domain_.demand.capture_rgb()) {
        if (!trace_colour_->reserve(*queue_, size_t(trace_.capacity) * sizeof(DeepCompactColourTrace), false) ||
            !trace_colour_counter_->zero(*queue_, 8)) return false;
        trace_.colour_records = trace_colour_->pointer(); trace_.colour_counter = trace_colour_counter_->pointer();
      }
    }
  }
  else if (witness_.width != width || witness_.height != height ||
           witness_.x != (domain_.core_width ? domain_.core_x : domain_.x) ||
           witness_.y != (domain_.core_height ? domain_.core_y : domain_.y))
    return fail("Deep witness source tile changed before its population closed");
  if (domain_.scheduled_admissions > UINT32_MAX ||
      !witness_rays_->reserve(*queue_, size_t(domain_.scheduled_admissions) * sizeof(DeepCompactWitnessRay), false))
    return false;
  witness_.log_t = witness_log_->pointer(); witness_.rgb = witness_rgb_->pointer();
  witness_.rays = witness_rays_->pointer(); witness_.status = controls_.device_pointer;
  witness_.camera_count = uint32_t(domain_.scheduled_admissions);
  witness_.has_volume = domain_.has_volume;
  return true;
}

bool DeepGPUFrameWork::bind_witness(IntegratorStateGPU &state)
{
  state.deep_compact_witness = nullptr;
  state.deep_compact_trace = nullptr;
  if (!witness_.cut_count) return true;
  witness_.cameras = reinterpret_cast<device_ptr>(state.deep_compact_cameras);
  if (!upload_metadata(&witness_, sizeof(witness_), witness_parameters_)) return false;
  state.deep_compact_witness = reinterpret_cast<const DeepCompactWitnessParameters *>(witness_parameters_.device_pointer);
  if (trace_.probe_count) {
    trace_.x = witness_.x; trace_.y = witness_.y; trace_.width = witness_.width; trace_.height = witness_.height;
    if (!upload_metadata(&trace_, sizeof(trace_), trace_parameters_)) return false;
    state.deep_compact_trace = reinterpret_cast<const DeepCompactTraceParameters *>(trace_parameters_.device_pointer);
  }
  return true;
}

bool DeepGPUFrameWork::finish_witness_native()
{
  if (!witness_.cut_count) return true;
  witness_.cameras = native_pointer(DeepGPUResidentKind::Camera);
  witness_.camera_count = native_count(DeepGPUResidentKind::Camera);
  if (!upload_metadata(&witness_, sizeof(witness_), witness_parameters_)) return false;
  DeepGPUFrameDispatch d; d.a = witness_parameters_.device_pointer;
  if (!operation(DeepGPUFrameOperation::CompactWitnessSeal, d, witness_blocks(witness_.camera_count))) return false;
  d.b = native_pointer(DeepGPUResidentKind::ColourSource);
  d.c = trace_.probe_count ? trace_parameters_.device_pointer : 0;
  d.n = native_count(DeepGPUResidentKind::ColourSource);
  if (!operation(DeepGPUFrameOperation::CompactWitnessSources, d, witness_blocks(d.n)) || !good()) return false;
  witness_rays_->clear(); witness_.rays = 0;
  return true;
}

bool DeepGPUFrameWork::dump_witness(RenderBuffers &buffers)
{
  if (!witness_.cut_count || !witness_log_->used()) return true;
  const auto &film = buffers.params;
  const int sample_pass = film.get_pass_offset(PASS_SAMPLE_COUNT);
  if (sample_pass == PASS_UNUSED || sample_pass < 0 || sample_pass >= film.pass_stride ||
      witness_.x != film.full_x + film.window_x || witness_.y != film.full_y + film.window_y ||
      witness_.width != uint32_t(film.window_width) || witness_.height != uint32_t(film.window_height))
    return fail("Deep witness Film population or window binding is invalid");
  const uint64_t count = uint64_t(witness_.width) * witness_.height * witness_.cut_count;
  auto output = make_buffer();
  const uint32_t channels = witness_.rgb ? 7 : 1;
  if (!output->reserve(*queue_, size_t(count) * channels * sizeof(double), false)) return false;
  witness_.output = output->pointer(); witness_.film = buffers.buffer.device_pointer;
  witness_.film_stride = film.stride; witness_.pass_stride = film.pass_stride;
  witness_.sample_pass = uint32_t(sample_pass); witness_.window_x = film.window_x; witness_.window_y = film.window_y;
  witness_.exposure = film.exposure;
  if (!upload_metadata(&witness_, sizeof(witness_), witness_parameters_)) return false;
  DeepGPUFrameDispatch d; d.a = witness_parameters_.device_pointer;
  if (!operation(DeepGPUFrameOperation::CompactWitnessOutput, d, witness_blocks(count)) || !good()) return false;
  RawBytes raw(host_budget_); raw.bytes.resize(size_t(count) * channels * sizeof(double));
  if (!queue_->read_device_region(output->memory(), 0, raw.bytes.data(), raw.bytes.size())) return false;
  const string stem = string_printf("tile-%llu-%d-%d-%u-%u", static_cast<unsigned long long>(domain_.id),
                                    witness_.x, witness_.y, witness_.width, witness_.height);
  const string binary = path_join(witness_root_, stem + ".bin");
  const string metadata = path_join(witness_root_, stem + ".json");
  FILE *file = std::fopen(binary.c_str(), "wbx");
  if (!file) return fail("Deep witness cannot exclusively create its raw output");
  bool ok = std::fwrite(raw.bytes.data(), 1, raw.bytes.size(), file) == raw.bytes.size();
  if (std::fclose(file) != 0) ok = false;
  if (!ok) return fail("Deep witness raw output write or checked close failed");
  file = std::fopen(metadata.c_str(), "wbx");
  if (!file) return fail("Deep witness cannot exclusively create its metadata");
  ok = std::fprintf(file, "{\"schema\":1,\"exact_output_root\":") > 0;
  ok &= witness_json_string(file, witness_root_);
  ok &= std::fprintf(file, ",\"binary\":") > 0;
  ok &= witness_json_string(file, binary);
  ok &= std::fprintf(file, ",\"x\":%d,\"y\":%d,\"width\":%u,\"height\":%u,\"cut_count\":%u,"
                          "\"physical_rgb\":%s,\"exposure\":%.17g,\"dtype\":\"little-endian-float64\","
                          "\"fields\":%s,\"cuts\":[",
                       witness_.x, witness_.y, witness_.width, witness_.height, witness_.cut_count,
                       witness_.rgb ? "true" : "false", double(witness_.exposure),
                       witness_.rgb ? "[\"logT\",\"half.R\",\"half.G\",\"half.B\",\"float.R\",\"float.G\",\"float.B\"]" : "[\"logT\"]") > 0;
  for (size_t i = 0; i < witness_cuts_.size(); i++)
    ok &= std::fprintf(file, "%s%.17g", i ? "," : "", witness_cuts_[i]) > 0;
  ok &= std::fprintf(file, "]}\n") > 0;
  if (std::fclose(file) != 0) ok = false;
  if (!ok) return fail("Deep witness metadata write or checked close failed");
  std::fprintf(stderr, "DEEP_WITNESS_OUTPUT exact_output_root=%s binary=%s metadata=%s bytes=%llu\n",
               witness_root_.c_str(), binary.c_str(), metadata.c_str(), static_cast<unsigned long long>(raw.bytes.size()));
  witness_log_->clear(); witness_rgb_->clear(); witness_rays_->clear();
  witness_.log_t = witness_.rgb = witness_.rays = witness_.output = 0;
  return true;
}

bool DeepGPUFrameWork::dump_trace()
{
  if (!trace_.probe_count || !trace_counter_->used()) return true;
  uint32_t counters[2] = {};
  if (!queue_->read_device_region(trace_counter_->memory(), 0, counters, sizeof(counters))) return false;
  const size_t count = std::min(counters[0], trace_.capacity);
  RawBytes raw(host_budget_); raw.bytes.resize(count * sizeof(DeepCompactTraceRecord));
  if (count && !queue_->read_device_region(trace_records_->memory(), 0, raw.bytes.data(), raw.bytes.size())) return false;
  const string stem = string_printf("fit-trace-%llu", static_cast<unsigned long long>(domain_.id));
  const string binary = path_join(witness_root_, stem + ".bin");
  const string metadata = path_join(witness_root_, stem + ".json");
  FILE *file = std::fopen(binary.c_str(), "wbx");
  if (!file) return fail("Deep trace cannot exclusively create its raw output");
  bool ok = !raw.bytes.size() || std::fwrite(raw.bytes.data(), 1, raw.bytes.size(), file) == raw.bytes.size();
  if (std::fclose(file) != 0) ok = false;
  if (!ok) return fail("Deep trace raw write or checked close failed");
  file = std::fopen(metadata.c_str(), "wbx");
  if (!file) return fail("Deep trace cannot exclusively create its metadata");
  ok = std::fprintf(file, "{\"schema\":1,\"exact_output_root\":") > 0;
  ok &= witness_json_string(file, witness_root_);
  ok &= std::fprintf(file, ",\"binary\":") > 0;
  ok &= witness_json_string(file, binary);
  ok &= std::fprintf(file, ",\"record_stride\":%zu,\"attempts\":%u,\"records\":%zu,\"dropped\":%u,"
                          "\"scope\":\"frozen selected pixels, capture and consumer local decisions\"",
                       sizeof(DeepCompactTraceRecord), counters[0], count, counters[1]) > 0;
  for (const bool colours : {false, true}) {
    const auto counter = colours ? trace_colour_counter_ : trace_raw_counter_;
    if (!counter->used()) continue;
    const auto records = colours ? trace_colour_ : trace_raw_;
    uint32_t scope_counts[2] = {};
    if (!queue_->read_device_region(counter->memory(), 0, scope_counts, sizeof(scope_counts))) {
      std::fclose(file); return false;
    }
    const size_t retained = std::min(scope_counts[0], trace_.capacity);
    const size_t stride = colours ? sizeof(DeepCompactColourTrace) : sizeof(DeepCompactRawTrace);
    RawBytes scope(host_budget_); scope.bytes.resize(retained * stride);
    if (retained && !queue_->read_device_region(records->memory(), 0, scope.bytes.data(), scope.bytes.size())) {
      std::fclose(file); return false;
    }
    const char *name = colours ? "colour" : "raw";
    const string scope_path = path_join(witness_root_, stem + "-" + name + ".bin");
    FILE *raw_file = std::fopen(scope_path.c_str(), "wbx");
    if (!raw_file) { std::fclose(file); return fail("Deep trace scope cannot exclusively create its output"); }
    bool written = !scope.bytes.size() ||
        std::fwrite(scope.bytes.data(), 1, scope.bytes.size(), raw_file) == scope.bytes.size();
    if (std::fclose(raw_file) != 0) written = false;
    if (!written) { std::fclose(file); return fail("Deep trace scope write or checked close failed"); }
    ok &= std::fprintf(file, ",\"%s\":{\"binary\":", name) > 0;
    ok &= witness_json_string(file, scope_path);
    ok &= std::fprintf(file, ",\"stride\":%zu,\"records\":%zu,\"attempts\":%u,\"dropped\":%u}",
                         stride, retained, scope_counts[0], scope_counts[1]) > 0;
  }
  if (capture_case_->used()) {
    auto cases = make_buffer();
    if (!cases->zero(*queue_, 8 * sizeof(DeepGPUFrameQualificationCase))) {
      std::fclose(file); return false;
    }
    DeepGPUFrameDispatch dispatch; dispatch.a = cases->pointer(); dispatch.b = capture_case_->pointer();
    if (!operation(DeepGPUFrameOperation::CompactQualificationCases, dispatch, 1, 32)) {
      std::fclose(file); return false;
    }
    for (const bool capture : {true, false}) {
      const size_t stride = capture ? sizeof(DeepCompactCaptureCase) : sizeof(DeepGPUFrameQualificationCase);
      const size_t population = capture ? 1 : 8;
      RawBytes payload(host_budget_); payload.bytes.resize(stride * population);
      auto source = capture ? capture_case_ : cases;
      if (!queue_->read_device_region(source->memory(), 0, payload.bytes.data(), payload.bytes.size())) {
        std::fclose(file); return false;
      }
      const char *name = capture ? "capture_case" : "encoding_cases";
      const string path = path_join(witness_root_, stem + "-" + name + ".bin");
      FILE *destination = std::fopen(path.c_str(), "wbx");
      if (!destination) { std::fclose(file); return fail("Deep case output exclusive create failed"); }
      bool written = std::fwrite(payload.bytes.data(), 1, payload.bytes.size(), destination) == payload.bytes.size();
      if (std::fclose(destination) != 0) written = false;
      if (!written) { std::fclose(file); return fail("Deep case output write or checked close failed"); }
      ok &= std::fprintf(file, ",\"%s\":{\"binary\":", name) > 0;
      ok &= witness_json_string(file, path);
      ok &= std::fprintf(file, ",\"stride\":%zu,\"records\":%zu}", stride, population) > 0;
    }
    capture_case_->clear(); trace_.capture_case = 0;
  }
  ok &= std::fprintf(file, "}\n") > 0;
  if (std::fclose(file) != 0) ok = false;
  if (!ok) return fail("Deep trace metadata write or checked close failed");
  std::fprintf(stderr, "DEEP_TRACE_OUTPUT exact_output_root=%s binary=%s records=%zu dropped=%u\n",
               witness_root_.c_str(), binary.c_str(), count, counters[1]);
  trace_records_->clear(); trace_counter_->clear();
  trace_raw_->clear(); trace_raw_counter_->clear(); trace_colour_->clear(); trace_colour_counter_->clear();
  trace_.records = trace_.counter = 0;
  trace_.raw_records = trace_.raw_counter = trace_.colour_records = trace_.colour_counter = 0;
  return true;
}

CCL_NAMESPACE_END
