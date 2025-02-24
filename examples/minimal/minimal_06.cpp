// Copyright 2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "create_voxels.h"
#include "framebuffer.h"

#include <openvkl/openvkl.h>

#if defined(_MSC_VER)
#include <malloc.h>
#else
#include <alloca.h>
#endif

int main(int argc, char **argv)
{
  vklLoadModule("cpu_device");
  VKLDevice device = vklNewDevice("cpu");
  vklCommitDevice(device);

  constexpr size_t res      = 128;
  std::vector<float> voxels = createVoxels(res);

  VKLVolume volume = vklNewVolume(device, "structuredRegular");
  vklSetVec3i(volume, "dimensions", res, res, res);
  const float spacing = 1.f / static_cast<float>(res);
  vklSetVec3f(volume, "gridSpacing", spacing, spacing, spacing);
  VKLData voxelData = vklNewData(
      device, voxels.size(), VKL_FLOAT, voxels.data(), VKL_DATA_SHARED_BUFFER);
  vklSetData(volume, "data", voxelData);
  vklRelease(voxelData);
  vklCommit(volume);

  VKLSampler sampler = vklNewSampler(volume);
  vklCommit(sampler);

  const float isovalues[]       = { -.6f, -.1f, .4f, .9f };
  VKLHitIteratorContext context = vklNewHitIteratorContext(sampler);
  VKLData isovaluesData         = vklNewData(device, 4, VKL_FLOAT, isovalues);
  vklSetData(context, "values", isovaluesData);
  vklRelease(isovaluesData);
  vklCommit(context);

  Framebuffer fb(64, 32);

  // We will create iterators below, and we will need to know how much memory
  // to allocate.
  const size_t iteratorSize = vklGetHitIteratorSize(context);

  fb.generate([&](float fx, float fy) {
    // Set up the ray, as iterators work on rays.
    const vkl_vec3f rayOrigin    = {fx, fy, 0.f};
    const vkl_vec3f rayDirection = {0.f, 0.f, 1.f};
    const vkl_range1f rayTRange  = {0.f, 1.f};

    // Create a buffer for the iterator.
#if defined(_MSC_VER)
    char *buffer = static_cast<char *>(_malloca(iteratorSize));
#else
    char *buffer = static_cast<char *>(alloca(iteratorSize));
#endif
    // Initialize iterator into the buffer we just created.
    VKLHitIterator hitIterator = vklInitHitIterator(
        context, &rayOrigin, &rayDirection, &rayTRange, 0.f, buffer);

    // Loop over all ray-isosurface intersections along our ray.
    // vklIterateHit will return false when there
    // is no more hit left.
    VKLHit hit;
    Color color = {0.f};
    while (vklIterateHit(hitIterator, &hit))
    {
        const Color c = transferFunction(hit.sample);
        color = over(color, c);
    }
    return color;
  });

  fb.drawToTerminal();

  vklRelease(context);
  vklRelease(sampler);
  vklRelease(volume);
  vklReleaseDevice(device);

  return 0;
}
