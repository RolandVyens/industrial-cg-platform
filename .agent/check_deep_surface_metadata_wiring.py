#!/usr/bin/env python3
"""Check that the Deep EXR hard-surface metadata wiring is active in source."""

from __future__ import annotations

import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def read_text(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def has_path_state_member(state_template: str) -> bool:
    return "KERNEL_STRUCT_MEMBER(path, uint32_t, deep_surface_sample_idx" in state_template


def has_path_state_init(path_state: str) -> bool:
    pattern = re.compile(
        r"INTEGRATOR_STATE_WRITE\(state,\s*path,\s*deep_surface_sample_idx\)\s*=\s*"
        r"(?:0xffffffffu|DEEP_INVALID_SAMPLE_INDEX);"
    )
    return bool(pattern.search(path_state))


def has_surface_writer_call(shade_surface: str) -> bool:
    return "film_write_deep_surface_sample_transparent(" in shade_surface


def stores_surface_sample_index(shade_surface: str) -> bool:
    pattern = re.compile(
        r"INTEGRATOR_STATE_WRITE\(state,\s*path,\s*deep_surface_sample_idx\)\s*="
    )
    return bool(pattern.search(shade_surface))


def has_surface_rgb_accumulation(light_passes: str) -> bool:
    return "film_accumulate_deep_surface_rgb(" in light_passes


def main() -> int:
    state_template = read_text("intern/cycles/kernel/integrator/state_template.h")
    path_state = read_text("intern/cycles/kernel/integrator/path_state.h")
    shade_surface = read_text("intern/cycles/kernel/integrator/shade_surface.h")
    light_passes = read_text("intern/cycles/kernel/film/light_passes.h")

    failures: list[str] = []

    if not has_path_state_member(state_template):
      failures.append("Missing path state member: path.deep_surface_sample_idx")
    if not has_path_state_init(path_state):
      failures.append("Missing path-state initialization/reset for deep_surface_sample_idx")
    if not has_surface_writer_call(shade_surface):
      failures.append(
          "Missing metadata-aware writer call in shade_surface.h: "
          "film_write_deep_surface_sample_transparent(...)"
      )
    if not stores_surface_sample_index(shade_surface):
      failures.append(
          "Missing storage of the returned deep surface sample index into path state"
      )
    if not has_surface_rgb_accumulation(light_passes):
      failures.append(
          "Missing RGB accumulation call in light_passes.h: "
          "film_accumulate_deep_surface_rgb(...)"
      )

    if failures:
      for failure in failures:
        print(f"FAIL: {failure}")
      print(f"metadata_wiring_failures={len(failures)}")
      return 1

    print("PASS: Deep EXR surface metadata wiring is active.")
    print("metadata_wiring_failures=0")
    return 0


if __name__ == "__main__":
    try:
      raise SystemExit(main())
    except Exception as exc:
      print(f"ERROR: {exc}")
      sys.exit(1)
