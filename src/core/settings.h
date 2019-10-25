#pragma once
#include "types.h"
#include <array>
#include <utility>

enum class GPURenderer
{
  HardwareOpenGL,
  Software
};

struct Settings
{
  Settings();

  GPURenderer gpu_renderer = GPURenderer::Software;
  u32 gpu_resolution_scale = 1;
  u32 max_gpu_resolution_scale = 1;
  bool gpu_vsync = true;

  // TODO: Controllers, memory cards, etc.

  static constexpr std::array<std::pair<GPURenderer, const char*>, 2> GPU_RENDERERS = {
    {{GPURenderer::HardwareOpenGL, "Hardware (OpenGL)"}, {GPURenderer::Software, "Software"}}};
};
