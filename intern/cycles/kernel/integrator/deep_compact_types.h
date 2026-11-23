/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>

enum DeepCompactKind : uint32_t {
  DEEP_COMPACT_VOLUME = 0,
  DEEP_COMPACT_SURFACE = 1,
  DEEP_COMPACT_HOLDOUT = 2,
  DEEP_COMPACT_BLACK = 3,
  DEEP_COMPACT_BACKGROUND = 4,
  DEEP_COMPACT_COLLAPSED_VOLUME = 5,
  DEEP_COMPACT_COLOUR = 6,
};

enum DeepCompactRayFlag : uint32_t {
  DEEP_COMPACT_TERMINAL = 1u,
  DEEP_COMPACT_STOP_SHARING = 2u,
  DEEP_COMPACT_HAS_CANDIDATE = 4u,
  DEEP_COMPACT_PENDING_CELL = 8u,
  DEEP_COMPACT_HAS_SURFACE = 16u,
  DEEP_COMPACT_TRANSFER_PENDING = 32u,
  DEEP_COMPACT_PENDING_POINT = 64u,
  DEEP_COMPACT_FINISH_PENDING = 128u,
  DEEP_COMPACT_EXTINCTION_DONE = 256u,
};

/* Surface continuation needs these two runtime bits alongside native static shader flags.
 * Keep the 64-byte record: native static flags begin at bit 12; low bits are free.
 * Volume records continue to use group as their material grouping identifier. */
enum DeepCompactSurfaceFlag : uint32_t {
  DEEP_COMPACT_SURFACE_BACKFACING = 1u,
  DEEP_COMPACT_SURFACE_RAY_PORTAL = 2u,
};

/* Scalar-only production layout. Native colour is never reserved in this record.
 * q is a fitting proxy; independently inherited log endpoints own the tail. */
struct DeepCompactPrimitive {
  float front, back;
  double q, log_before, log_after;
  uint32_t camera, ordinal, kind, group;
  uint32_t object, prim, shader, normal;
};
static_assert(sizeof(DeepCompactPrimitive) == 64);

struct DeepCompactRayState {
  double log_m, log_q;
  float cursor, last_surface_t;
  uint32_t next_ordinal, flags, last_surface_object, last_surface_prim;
  DeepCompactPrimitive candidate, reference;
  /* One evaluated cell survives page exhaustion; it is not re-shaded. */
  float pending_front, pending_back;
  double pending_sigma, pending_cursor;
  uint32_t pending_phase, reserved;
  float stack_front;
  uint32_t stack_valid;
  DeepCompactPrimitive pending_point;
  double pending_log_m, pending_log_q;
};
static_assert(sizeof(DeepCompactRayState) == 288);

struct DeepCompactStackEntry {
  int32_t object, shader;
};
static_assert(sizeof(DeepCompactStackEntry) == 8);

/* Final scalar camera metadata replaces the native continuation after coverage closes. */
struct DeepCompactCamera {
  int32_t x, y;
  uint32_t sample, disposition;
  double log_tail;
  float projection, depth_origin;
};
static_assert(sizeof(DeepCompactCamera) == 32);

struct DeepCompactEvent {
  double slope, jump;
  float z;
  uint32_t pixel, source, kind;
  int32_t active;
  uint32_t reserved;
};
static_assert(sizeof(DeepCompactEvent) == 40);

/* Point before/after and the next open interval are independently evaluable.
 * The q fields survive subtraction cancellation in nearly transparent packets. */
struct DeepCompactKnot {
  float z, next_z;
  double log_before, log_after, log_next, point_q, interval_q;
  uint32_t kind, source;
};
static_assert(sizeof(DeepCompactKnot) == 56);

struct DeepCompactParameters {
  uint64_t cameras, camera_keys, camera_ids, camera_map, pixels;
  uint64_t primitives, primitive_ids, events, event_keys, event_ids, event_ranges;
  uint64_t areas, knots, tail, projection, status;
  uint32_t camera_count, primitive_count, event_count, pixel_count;
};
static_assert(sizeof(DeepCompactParameters) == 144);

struct DeepCompactRootColour {
  float rgb[3];
  uint32_t flags;
};
static_assert(sizeof(DeepCompactRootColour) == 16);

struct DeepCompactColourSource {
  float z, z_back, physical_front, physical_back;
  float sigma[3], rgb[3];
  uint32_t camera, kind, ordinal, flags;
  double map_a, map_b;
  uint32_t object, prim, shader, target;
};
static_assert(sizeof(DeepCompactColourSource) == 88);

struct DeepCompactEmissionState {
  float front, back, sigma[3];
  uint32_t flags;
};
static_assert(sizeof(DeepCompactEmissionState) == 24);

struct DeepCompactColourCursor {
  double physical;
  uint32_t done, reserved;
};
static_assert(sizeof(DeepCompactColourCursor) == 16);

struct DeepCompactColourPiece {
  float front, back;
  double rgb[3];
  uint32_t pixel, target, camera, kind;
};
static_assert(sizeof(DeepCompactColourPiece) == 48);

/* Colour-only endpoints refine consumers, never the already frozen scalar AA
 * base. Attached point colour instead addresses its original scalar packet. */
struct DeepCompactJoinEvent {
  double slope[3], jump[3];
  float z;
  uint32_t pixel, base, kind;
};
static_assert(sizeof(DeepCompactJoinEvent) == 64);

struct DeepCompactColourParameters {
  uint64_t base, sources, cursors, pieces, hash, point_rgb, controls;
  uint32_t source_count, page_capacity, hash_capacity, reserved;
  float exposure;
  uint32_t alpha_half;
};
static_assert(sizeof(DeepCompactColourParameters) == 80);

struct DeepCompactOutputSummary {
  double log_denominator, log_tail;
  uint32_t surface, reserved;
};
static_assert(sizeof(DeepCompactOutputSummary) == 24);

struct DeepCompactEncodeParameters {
  uint64_t base, joined, input_ranges, policy, states, samples, keys, controls, summaries;
  uint64_t trace;
  uint32_t pixel_count, page_capacity, physical, simple;
};
static_assert(sizeof(DeepCompactEncodeParameters) == 96);

struct DeepCompactSortItem {
  uint64_t key;
  uint32_t id, reserved;
};
static_assert(sizeof(DeepCompactSortItem) == 16);

struct DeepCompactWitnessRay {
  double log_m, log_q;
  float cursor;
  uint32_t next_cut, done, reserved;
};
static_assert(sizeof(DeepCompactWitnessRay) == 32);

/* Optional qualification only. No witness allocation or arithmetic is enabled
 * in the production timing lane. Device inputs are original evaluated cells. */
struct DeepCompactWitnessParameters {
  uint64_t cuts, log_t, rgb, rays, cameras, status, output, film;
  uint32_t cut_count, width, height, camera_count;
  int32_t x, y;
  uint32_t camera_base, has_volume, film_stride, pass_stride;
  uint32_t sample_pass, window_x, window_y, flags;
  float exposure;
  uint32_t padding;
};
static_assert(sizeof(DeepCompactWitnessParameters) == 128);

struct DeepCompactTraceParameters {
  uint64_t records, counter, probes;
  uint64_t raw_records, raw_counter, colour_records, colour_counter;
  uint64_t capture_case;
  uint32_t capacity, probe_count;
  int32_t x, y;
  uint32_t width, height;
};
static_assert(sizeof(DeepCompactTraceParameters) == 88);

struct DeepCompactTraceRecord {
  DeepCompactPrimitive left, reference, right, trial;
  double alpha, depth, rgb_absolute, rgb_relative, footprint;
  double rgb[4][3], epsilon_left[3], epsilon_right[3], epsilon_trial[3];
  int32_t x, y;
  uint32_t flags, reserved;
};
static_assert(sizeof(DeepCompactTraceRecord) == 480);

struct DeepCompactRawTrace {
  double front, back, sigma, log_m, log_q, z_front, z_back;
  double origin[3], direction[3], camera_origin[3];
  double affine_origin, affine_direction, transmission, holdout;
  int32_t x, y;
  uint32_t sample, flags, ordinal, reserved;
};
static_assert(sizeof(DeepCompactRawTrace) == 184);

struct DeepCompactColourTrace {
  DeepCompactColourSource source;
  int32_t x, y;
  uint32_t sample, reserved;
};
static_assert(sizeof(DeepCompactColourTrace) == 104);

struct DeepCompactCaptureCase {
  DeepCompactPrimitive primitives[4];
  uint32_t count, claimed, status, reserved;
};
static_assert(sizeof(DeepCompactCaptureCase) == 272);
