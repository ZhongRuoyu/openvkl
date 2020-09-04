// Copyright 2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../../common/export_util.h"
#include "../../iterator/UnstructuredIterator.h"
#include "../../sampler/Sampler.h"
#include "ParticleVolume.h"
#include "ParticleVolume_ispc.h"
#include "Volume_ispc.h"

namespace openvkl {
  namespace ispc_driver {

    template <int W>
    struct ParticleSampler : public SamplerBase<W, ParticleVolume>
    {
      ParticleSampler(ParticleVolume<W> *volume);

      ~ParticleSampler() override = default;

      void commit() override {}

      void computeSampleV(const vintn<W> &valid,
                          const vvec3fn<W> &objectCoordinates,
                          vfloatn<W> &samples,
                          unsigned int attributeIndex) const override final;

      void computeSampleN(unsigned int N,
                          const vvec3fn<1> *objectCoordinates,
                          float *samples,
                          unsigned int attributeIndex) const override final;

      void computeGradientV(const vintn<W> &valid,
                            const vvec3fn<W> &objectCoordinates,
                            vvec3fn<W> &gradients,
                            unsigned int attributeIndex) const override final;

      void computeGradientN(unsigned int N,
                            const vvec3fn<1> *objectCoordinates,
                            vvec3fn<1> *gradients,
                            unsigned int attributeIndex) const override final;

      const IteratorFactory<W, IntervalIterator> &getIntervalIteratorFactory()
          const override final;

      const IteratorFactory<W, HitIterator> &getHitIteratorFactory()
          const override final;

     protected:
      using SamplerBase<W, ParticleVolume>::volume;

     private:
        UnstructuredIntervalIteratorFactory<W> intervalIteratorFactory;
        UnstructuredHitIteratorFactory<W> hitIteratorFactory;
    };

    // Inlined definitions ////////////////////////////////////////////////////

    template <int W>
    inline ParticleSampler<W>::ParticleSampler(ParticleVolume<W> *volume)
        : SamplerBase<W, ParticleVolume>(*volume)
    {
      assert(volume);
    }

    template <int W>
    inline void ParticleSampler<W>::computeSampleV(
        const vintn<W> &valid,
        const vvec3fn<W> &objectCoordinates,
        vfloatn<W> &samples,
        unsigned int attributeIndex) const
    {
      assert(attributeIndex < volume->getNumAttributes());
      CALL_ISPC(VKLParticleVolume_sample_export,
                static_cast<const int *>(valid),
                volume->getISPCEquivalent(),
                &objectCoordinates,
                &samples);
    }

    template <int W>
    inline void ParticleSampler<W>::computeSampleN(
        unsigned int N,
        const vvec3fn<1> *objectCoordinates,
        float *samples,
        unsigned int attributeIndex) const
    {
      assert(attributeIndex < volume->getNumAttributes());
      CALL_ISPC(Volume_sample_N_export,
                volume->getISPCEquivalent(),
                N,
                (ispc::vec3f *)objectCoordinates,
                samples);
    }

    template <int W>
    inline void ParticleSampler<W>::computeGradientV(
        const vintn<W> &valid,
        const vvec3fn<W> &objectCoordinates,
        vvec3fn<W> &gradients,
        unsigned int attributeIndex) const
    {
      assert(attributeIndex < volume->getNumAttributes());
      CALL_ISPC(VKLParticleVolume_gradient_export,
                static_cast<const int *>(valid),
                volume->getISPCEquivalent(),
                &objectCoordinates,
                &gradients);
    }

    template <int W>
    inline void ParticleSampler<W>::computeGradientN(
        unsigned int N,
        const vvec3fn<1> *objectCoordinates,
        vvec3fn<1> *gradients,
        unsigned int attributeIndex) const
    {
      assert(attributeIndex < volume->getNumAttributes());
      CALL_ISPC(Volume_gradient_N_export,
                volume->getISPCEquivalent(),
                N,
                (ispc::vec3f *)objectCoordinates,
                (ispc::vec3f *)gradients);
    }

    template <int W>
    inline const IteratorFactory<W, IntervalIterator>
        &ParticleSampler<W>::getIntervalIteratorFactory() const
    {
      return intervalIteratorFactory;
    }

    template <int W>
    inline const IteratorFactory<W, HitIterator>
        &ParticleSampler<W>::getHitIteratorFactory() const
    {
      return hitIteratorFactory;
    }

  }  // namespace ispc_driver
}  // namespace openvkl
