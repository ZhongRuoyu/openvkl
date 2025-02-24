// Copyright 2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "api/CPUDevice.h"
#include "../common/ManagedObject.h"
#include "../common/VKLCommon.h"

// For Windows in particular, we need registered object creation functions
// available in the top level module library, as exports from dependencies will
// not be visible.

#define VKL_WRAP_MODULE_REGISTRATION(module_name) \
  extern "C" OPENVKL_DLLEXPORT void openvkl_init_module_##module_name();

#if VKL_TARGET_WIDTH_ENABLED_4
VKL_WRAP_MODULE_REGISTRATION(cpu_device_4)
#endif

#if VKL_TARGET_WIDTH_ENABLED_8
VKL_WRAP_MODULE_REGISTRATION(cpu_device_8)
#endif

#if VKL_TARGET_WIDTH_ENABLED_16
VKL_WRAP_MODULE_REGISTRATION(cpu_device_16)
#endif

// calls init functions in [4, 8, 16] devices to ensure proper linkage
extern "C" OPENVKL_DLLEXPORT void openvkl_init_module_cpu_device()
{
#if VKL_TARGET_WIDTH_ENABLED_4
  try {
    openvkl::loadLocalModule("cpu_device_4");
  } catch (const std::exception &e) {
    openvkl::postLogMessage(nullptr, e.what(), VKL_LOG_ERROR);
  }
#endif

#if VKL_TARGET_WIDTH_ENABLED_8
  try {
    openvkl::loadLocalModule("cpu_device_8");
  } catch (const std::exception &e) {
    openvkl::postLogMessage(nullptr, e.what(), VKL_LOG_ERROR);
  }
#endif

#if VKL_TARGET_WIDTH_ENABLED_16
  try {
    openvkl::loadLocalModule("cpu_device_16");
  } catch (const std::exception &e) {
    openvkl::postLogMessage(nullptr, e.what(), VKL_LOG_ERROR);
  }
#endif
}
