// Copyright 2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>
#include "VKLCommon.h"
#include "logging.h"
#include "openvkl/VKLDataType.h"

namespace openvkl {

  // Function pointer type for creating a concrete instance of a subtype of
  // this class.
  template <typename T>
  using FactoryFcn = T *(*)();

  template <typename T, VKLDataType VKL_TYPE = VKL_UNKNOWN>
  struct ObjectFactory
  {
    T *createInstance(Device *device, const std::string &type);

    void registerType(const std::string &type, FactoryFcn<T> f);

   private:
    std::map<std::string, FactoryFcn<T>> registry;
  };

  template <typename T, VKLDataType VKL_TYPE>
  T *ObjectFactory<T, VKL_TYPE>::createInstance(Device *device,
                                                const std::string &type)
  {
    const auto type_string = stringFor(VKL_TYPE);

    T *object = nullptr;
    if (registry.count(type) != 0) {
      // Create a concrete instance of the requested subtype.
      object = (*registry[type])();
    }

    if (object == nullptr) {
      registry.erase(type);
      throw std::runtime_error(
          "Could not find " + type_string + " of type: " + type +
          ".  Make sure you have the correct VKL libraries linked.");
    }

    return object;
  }

  template <typename T, VKLDataType VKL_TYPE>
  void ObjectFactory<T, VKL_TYPE>::registerType(const std::string &type,
                                                FactoryFcn<T> f)
  {
    registry[type] = f;
  }

}  // namespace openvkl
