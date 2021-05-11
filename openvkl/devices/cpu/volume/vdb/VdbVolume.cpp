// Copyright 2019-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "VdbVolume.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <set>
#include "../../common/export_util.h"
#include "../../common/runtime_error.h"
#include "../../common/temporal_data_verification.h"
#include "../common/logging.h"
#include "VdbInnerNodeObserver.h"
#include "VdbSampler.h"
#include "VdbSampler_ispc.h"
#include "openvkl/vdb.h"
#include "rkcommon/math/AffineSpace.h"
#include "rkcommon/memory/malloc.h"
#include "rkcommon/tasking/AsyncTask.h"
#include "rkcommon/tasking/parallel_for.h"

namespace openvkl {
  namespace cpu_device {

    // -------------------------------------------------------------------------

    template <int W>
    VdbVolume<W>::VdbVolume()
    {
      this->ispcEquivalent = CALL_ISPC(VdbVolume_create);
    }

    template <int W>
    VdbVolume<W>::~VdbVolume()
    {
      cleanup();
      CALL_ISPC(VdbVolume_destroy, this->ispcEquivalent);
    }

    template <int W>
    void VdbVolume<W>::cleanup()
    {
      if (grid) {
        // Note: There are VKL_VDB_NUM_LEVELS-1 slots for the
        //       level buffers! Leaves are not stored in the hierarchy!
        for (uint32_t l = 0; (l + 1) < vklVdbNumLevels(); ++l) {
          VdbLevel &level = grid->levels[l];
          allocator.deallocate(level.origin);
          allocator.deallocate(level.voxels);
          allocator.deallocate(level.valueRange);
        }
        allocator.deallocate(grid->attributeTypes);
        allocator.deallocate(grid->leafUnstructuredIndices);
        allocator.deallocate(grid->leafUnstructuredTimes);
        allocator.deallocate(grid->leafData);
        allocator.deallocate(grid);
      }

      leafData           = nullptr;
      leafFormat         = nullptr;
      leafTemporalFormat = nullptr;
    }

    template <int W>
    std::string VdbVolume<W>::toString() const
    {
      return "openvkl::VdbVolume";
    }

    /*
     * Compute the grid bounding box and count the number of leaves per level.
     */
    box3i computeBbox(uint64_t numLeaves,
                      const DataT<uint32_t> &leafLevel,
                      const DataT<vec3i> &leafOrigin)
    {
      box3i bbox = box3i();
      for (uint64_t i = 0; i < numLeaves; ++i) {
        bbox.extend(leafOrigin[i]);
        bbox.extend(leafOrigin[i] + vec3ui(vklVdbLevelRes(leafLevel[i])));
      }
      return bbox;
    }

    /*
     * Bin leaves per level (returns indices into the input leaf array).
     */
    std::vector<std::vector<uint64_t>> binLeavesPerLevel(
        uint64_t numLeaves, const DataT<uint32_t> &leafLevel)
    {
      std::vector<uint64_t> numLeavesPerLevel(vklVdbNumLevels(), 0);
      for (uint64_t i = 0; i < numLeaves; ++i) {
        if (leafLevel[i] == 0)
          runtimeError("there must not be any leaf nodes on level 0");
        numLeavesPerLevel[leafLevel[i]]++;
      }

      // Sort leaves by level.
      std::vector<std::vector<uint64_t>> binnedLeaves(vklVdbNumLevels());
      ;
      for (uint32_t l = 1; l < vklVdbNumLevels();
           ++l)  // level 0 has no leaves!
        binnedLeaves[l].reserve(numLeavesPerLevel[l]);
      for (uint64_t i = 0; i < numLeaves; ++i)
        binnedLeaves[leafLevel[i]].push_back(i);
      return binnedLeaves;
    }

    /*
     * Compute the root node origin from the bounding box.
     */
    vec3i computeRootOrigin(const box3i &bbox)
    {
      const vec3ui bboxRes = bbox.upper - bbox.lower;
      if (bboxRes.x > vklVdbLevelRes(0) || bboxRes.y > vklVdbLevelRes(0) ||
          bboxRes.z > vklVdbLevelRes(0)) {
        runtimeError("input leaves do not fit into a single root level node");
      }
      return vec3i(
          vklVdbLevelRes(1) *
              (int)std::floor(bbox.lower.x / (float)vklVdbLevelRes(1)),
          vklVdbLevelRes(1) *
              (int)std::floor(bbox.lower.y / (float)vklVdbLevelRes(1)),
          vklVdbLevelRes(1) *
              (int)std::floor(bbox.lower.z / (float)vklVdbLevelRes(1)));
    }

    /*
     * We don't want to deal with the complexity of negative
     * indices in our tree, so only consider offsets relative to the root node
     * origin.
     * This function computes these offsets.
     */
    inline std::vector<vec3ui> computeLeafOffsets(
        uint64_t numLeaves,
        const DataT<vec3i> &leafOrigin,
        const vec3ui &rootOrigin)
    {
      std::vector<vec3ui> leafOffsets(numLeaves);
      for (uint64_t i = 0; i < numLeaves; ++i)
        leafOffsets[i] = static_cast<vec3ui>(leafOrigin[i] - rootOrigin);
      return leafOffsets;
    }

    inline vec3ui offsetToNodeOrigin(
        const vec3ui &offset,  // offset from the root origin.
        uint32_t level)        // the level the node is on.
    {
      // We get the inner node origin from a given (leaf) voxel offset
      // by masking out lower bits.
      const uint32_t mask = ~(vklVdbLevelRes(level) - 1);
      return vec3ui(offset.x & mask, offset.y & mask, offset.z & mask);
    }

    inline vec3ui offsetToVoxelIndex(const vec3ui &offset, uint32_t level)
    {
      // The lower bits contain the offset from the node origin. We then
      // shift by the log child resolution to obtain the voxel index.
      const uint32_t mask = vklVdbLevelRes(level) - 1;
      return vec3ui((offset.x & mask) >> vklVdbLevelTotalLogRes(level + 1),
                    (offset.y & mask) >> vklVdbLevelTotalLogRes(level + 1),
                    (offset.z & mask) >> vklVdbLevelTotalLogRes(level + 1));
    }

    inline uint64_t offsetToLinearVoxelIndex(const vec3ui &offset,
                                             uint32_t level)
    {
      // The lower bits contain the offset from the node origin. We then
      // shift by the log child resolution to obtain the voxel index.
      const vec3ui vi = offsetToVoxelIndex(offset, level);
      return (((uint64_t)vi.x) << (2 * vklVdbLevelResShift(level))) +
             (((uint64_t)vi.y) << vklVdbLevelResShift(level)) +
             ((uint64_t)vi.z);
    }

    /*
     * Initialize all (inner) levels. To do this, we must
     * count the number of inner nodes per level, and allocate buffers for
     * voxels and auxiliary data.
     */
    void allocateInnerLevels(
        const std::vector<vec3ui> &leafOffsets,
        const std::vector<std::vector<uint64_t>> &binnedLeaves,
        std::vector<uint64_t> &capacity,
        VdbGrid *grid,
        Allocator &allocator)
    {
      // Comparator for voxel coordinates. We use this for sorting.
      const auto isLess = [](const vec3i &a, const vec3i &b) {
        return (a.x < b.x) || ((a.x == b.x) && (a.y < b.y)) ||
               ((a.x == b.x) && (a.y == b.y) && (a.z < b.z));
      };

      // Origins on the previous level. These are offsets from grid->rootOrigin.
      std::vector<vec3ui> oldInnerOrigins;

      // From the leaf level, go upwards quantizing leaf origins
      // to the respective level storage resolution, and count all
      // active nodes.
      for (int i = 0; i < vklVdbNumLevels() - 1; ++i) {
        // We traverse bottom-to-top, starting at the leaf level (we will update
        // the parent level!).
        const int l = vklVdbNumLevels() - i - 1;

        // Quantize all of this level's leaf origins to the node size, mapping
        // offsets to inner node origins. We can do this using simple masking
        // because node resolutions are powers of two.
        std::vector<vec3ui> innerOrigins;
        innerOrigins.reserve(oldInnerOrigins.size() + binnedLeaves[l].size());
        for (uint64_t leaf : binnedLeaves[l])
          innerOrigins.push_back(offsetToNodeOrigin(leafOffsets[leaf], l - 1));

        // Also quanitize the child level's inner node origins.
        for (const vec3ui &org : oldInnerOrigins) {
          innerOrigins.push_back(offsetToNodeOrigin(org, l - 1));
        }

        // We now have a list of inner node origins on level l-1, but it
        // contains duplicates. Sort and remove duplicates, and store for next
        // iterations.
        std::sort(innerOrigins.begin(), innerOrigins.end(), isLess);
        const auto end = std::unique(innerOrigins.begin(), innerOrigins.end());
        const uint64_t levelNumInner = end - innerOrigins.begin();
        innerOrigins.resize(levelNumInner);
        oldInnerOrigins = std::move(innerOrigins);

        if (levelNumInner > 0) {
          assert(l > 1 ||
                 levelNumInner ==
                     1);  // This should be true at this point, but make sure...
          VdbLevel &level = grid->levels[l - 1];
          capacity[l - 1] = levelNumInner;
          level.origin    = allocator.allocate<vec3ui>(levelNumInner);

          const size_t totalNumVoxels =
              levelNumInner * vklVdbLevelNumVoxels(l - 1);
          level.voxels = allocator.allocate<uint64_t>(totalNumVoxels);
          level.valueRange =
              allocator.allocate<range1f>(totalNumVoxels * grid->numAttributes);
          range1f empty;
          std::fill(level.valueRange,
                    level.valueRange + totalNumVoxels * grid->numAttributes,
                    empty);
        }
      }
    }

    /*
     * Compute the value range for a leaf.
     */
    range1f computeValueRange(const VdbGrid *grid,
                              VKLFormat format,
                              uint32_t level,
                              const vec3ui &offset,
                              unsigned int attributeIndex)
    {
      range1f range;

      CALL_ISPC(VdbSampler_computeValueRange,
                grid,
                reinterpret_cast<const ispc::vec3ui *>(&offset),
                level,
                attributeIndex,
                reinterpret_cast<ispc::box1f *>(&range));

      return range;
    }

    /*
     * Insert leaf nodes into the tree, creating inner nodes as needed.
     * This function does not allocate anything; allocateInnerLevels() has done
     * this already.
     */
    void insertLeaves(const std::vector<vec3ui> &leafOffsets,
                      const DataT<uint32_t> &leafFormat,
                      const DataT<uint32_t> &leafTemporalFormat,
                      const DataT<Data *> &leafData,
                      const std::vector<std::vector<uint64_t>> &binnedLeaves,
                      const std::vector<uint64_t> &capacity,
                      VdbGrid *grid)
    {
      assert(capacity[0] == 1);
      grid->levels[0].numNodes = 1;

      for (size_t leafLevel = 0; leafLevel < binnedLeaves.size(); ++leafLevel) {
        const auto &leaves = binnedLeaves[leafLevel];
        for (uint64_t idx : leaves) {
          const auto format = static_cast<VKLFormat>(leafFormat[idx]);
          const auto temporalFormat =
              static_cast<VKLTemporalFormat>(leafTemporalFormat[idx]);

          const vec3ui &offset = leafOffsets[idx];
          uint64_t nodeIndex   = 0;
          for (size_t l = 0; l < leafLevel; ++l) {
            VdbLevel &level = grid->levels[l];
            // PRECOND: nodeIndex is valid.
            assert(nodeIndex < level.numNodes);

            const uint64_t voxelIndex = offsetToLinearVoxelIndex(offset, l);
            // NOTE: If this is every greater than 2^32-1 then we will have to
            // use 64 bit addressing.
            const uint64_t v = nodeIndex * vklVdbLevelNumVoxels(l) + voxelIndex;
            assert(v < ((uint64_t)1) << 32);

            uint64_t &voxel = level.voxels[v];
            if (vklVdbVoxelIsLeafPtr(voxel)) {
              assert(false);
              runtimeError(
                  "Attempted to insert a leaf node into a leaf node (level ",
                  l + 1,
                  ", origin ",
                  offsetToNodeOrigin(offset, l),
                  ")");

            } else if (vklVdbVoxelIsEmpty(voxel)) {
              const size_t nl = l + 1;
              if (nl < leafLevel) {
                nodeIndex = grid->levels[nl].numNodes++;
                assert(grid->levels[nl].numNodes <= capacity[nl]);
                voxel = vklVdbVoxelMakeChildPtr(nodeIndex);
                grid->levels[nl].origin[nodeIndex] =
                    offsetToNodeOrigin(offset, nl);
              } else {
                if (format == VKL_FORMAT_TILE ||
                    format == VKL_FORMAT_DENSE_ZYX) {
                  voxel = vklVdbVoxelMakeLeafPtr(idx, format, temporalFormat);
                } else {
                  assert(false);
                }
              }
            } else {
              nodeIndex = vklVdbVoxelChildGetIndex(voxel);
              assert(nodeIndex < grid->levels[l + 1].numNodes);
            }
          }
        }
      }
    }

    /*
     * Compute the value range for the given nodes.
     * The tree must be fully initialized before calling this!
     * This function takes into account filter radius.
     */
    void computeValueRanges(const std::vector<vec3ui> &leafOffsets,
                            const DataT<uint32_t> &leafLevel,
                            const DataT<uint32_t> &leafFormat,
                            VdbGrid *grid)
    {
      const uint64_t numLeaves = leafOffsets.size();

      // The value range computation is a big part of commit() cost. We
      // do it in parallel to make up for that as much as possible.
      std::vector<std::vector<range1f>> valueRanges(
          numLeaves, std::vector<range1f>(grid->numAttributes));

      tasking::parallel_for(numLeaves, [&](uint64_t idx) {
        const auto format    = static_cast<VKLFormat>(leafFormat[idx]);
        const vec3ui &offset = leafOffsets[idx];

        for (unsigned int j = 0; j < grid->numAttributes; j++) {
          valueRanges[idx][j] =
              computeValueRange(grid, format, leafLevel[idx], offset, j);
        }
      });

      for (uint64_t idx = 0; idx < numLeaves; ++idx) {
        const vec3ui &offset = leafOffsets[idx];

        uint64_t nodeIndex = 0;
        for (size_t l = 0; l < leafLevel[idx]; ++l) {
          VdbLevel &level = grid->levels[l];
          // PRECOND: nodeIndex is valid.
          assert(nodeIndex < level.numNodes);

          const uint64_t voxelIndex = offsetToLinearVoxelIndex(offset, l);
          // NOTE: If this is ever greater than 2^32-1 then we will have to
          // use 64 bit addressing.
          const uint64_t v = nodeIndex * vklVdbLevelNumVoxels(l) + voxelIndex;
          assert(v < ((uint64_t)1) << 32);

          for (unsigned int j = 0; j < grid->numAttributes; j++) {
            level.valueRange[v * grid->numAttributes + j].extend(
                valueRanges[idx][j]);
          }

          uint64_t &voxel = level.voxels[v];
          assert(!vklVdbVoxelIsEmpty(voxel));

          if (vklVdbVoxelIsLeafPtr(voxel)) {
            break;
          }

          nodeIndex = vklVdbVoxelChildGetIndex(voxel);
          assert(nodeIndex < grid->levels[l + 1].numNodes);
        }
      }
    }

    /*
     * Load an affine 4x3 matrix from the given object.
     */
    inline AffineSpace3f getParamAffineSpace3f(ManagedObject *obj,
                                               const char *name)
    {
      Ref<const DataT<float>> dataIndexToObject =
          obj->template getParamDataT<float>(name, nullptr);
      AffineSpace3f a(one);
      if (dataIndexToObject && dataIndexToObject->size() >= 12) {
        const DataT<float> &i2w = *dataIndexToObject;
        a.l                     = LinearSpace3f(vec3f(i2w[0], i2w[1], i2w[2]),
                                                vec3f(i2w[3], i2w[4], i2w[5]),
                                                vec3f(i2w[6], i2w[7], i2w[8]));
        a.p                     = vec3f(i2w[9], i2w[10], i2w[11]);
      }
      return a;
    }

    /*
     * Store the given transformation in a format that our ISPC implementation
     * can work with.
     */
    inline void writeTransform(const AffineSpace3f &a, float *buffer)
    {
      assert(buffer);
      buffer[0]  = a.l.row0().x;
      buffer[1]  = a.l.row0().y;
      buffer[2]  = a.l.row0().z;
      buffer[3]  = a.l.row1().x;
      buffer[4]  = a.l.row1().y;
      buffer[5]  = a.l.row1().z;
      buffer[6]  = a.l.row2().x;
      buffer[7]  = a.l.row2().y;
      buffer[8]  = a.l.row2().z;
      buffer[9]  = a.p.x;
      buffer[10] = a.p.y;
      buffer[11] = a.p.z;
    }

    inline void initIndexSpaceTransforms(ManagedObject *obj,
                                         float *indexToObject,
                                         float *objectToIndex)
    {
      const AffineSpace3f i2o = getParamAffineSpace3f(obj, "indexToObject");
      writeTransform(i2o, indexToObject);

      AffineSpace3f o2i;
      o2i.l = i2o.l.inverse();
      o2i.p = -(o2i.l * i2o.p);
      writeTransform(o2i, objectToIndex);
    }

    /*
     * Extract the main node data array, and verify that there are nodes.
     */
    inline Ref<const DataT<Data *>> getLeafNodeData(ManagedObject *obj)
    {
      Ref<const DataT<Data *>> leafData =
          obj->template getParamDataT<Data *>("node.data");
      if (leafData->size() == 0) {
        runtimeError("Vdb volumes must have at least one leaf node.");
      }
      return leafData;
    }

    /*
     * Extract the lef node data type, and verify that it is valid for all
     * nodes.
     */
    inline VKLDataType getLeafDataType(const Ref<const DataT<Data *>> &leafData)
    {
      assert(leafData->size() > 0);
      const VKLDataType dataType = (*leafData)[0]->dataType;
      for (size_t i = 1; i < leafData->size(); ++i) {
        const VKLDataType curDataType = (*leafData)[i]->dataType;
        if (curDataType != dataType) {
          runtimeError("All nodes must have the same VKLDataType ",
                       "in vdb volumes.");
        }
      }

      if (dataType != VKL_HALF && dataType != VKL_FLOAT &&
          dataType != VKL_DATA) {
        runtimeError("node.data arrays have data type ",
                     dataType,
                     " but only ",
                     VKL_HALF,
                     " (VKL_HALF), ",
                     VKL_FLOAT,
                     " (VKL_FLOAT), or ",
                     VKL_DATA,
                     " (VKL_DATA) is supported for vdb volumes.");
      }

      return dataType;
    }

    /*
     * Initialize a single node, and verify attribute types in the process.
     *
     * nodeData: an array with numAttributes entries.
     * expectedNumDataElements: The number of elements expected for each
     *                          attribute buffer.
     * attributeTypes: an array with numAttributes entries.
     * data: an array of numAttributes Data1D objects.
     *
     * Returns true if all buffers are compact, and false if at least one is
     * strided.
     */
    inline bool initNode(Data *const *nodeData,
                         uint64_t expectedNumDataElements,
                         const uint32_t *attributeTypes,
                         uint32_t numAttributes,
                         ispc::Data1D *data)  // numAttributes values.
    {
      bool allCompact = true;
      for (uint32_t a = 0; a < numAttributes; ++a) {
        allCompact &= nodeData[a]->compact();
        if (nodeData[a]->size() < expectedNumDataElements) {
          runtimeError("Node data too small: found ",
                       nodeData[a]->size(),
                       " elements, but expected ",
                       expectedNumDataElements);
        }
        if (nodeData[a]->size() > expectedNumDataElements) {
          runtimeError("Node data too big: found ",
                       nodeData[a]->size(),
                       " elements, but expected ",
                       expectedNumDataElements);
        }
        if (attributeTypes[a] == VKL_HALF) {
          data[a] = nodeData[a]->ispc;
          // Manual error checking because Data does not support half directly.
          if (nodeData[a]->dataType != VKL_HALF) {
            runtimeError("inconsistent leaf attribute data type ",
                         "(expected VKL_HALF)");
          }
        } else if (attributeTypes[a] == VKL_FLOAT) {
          data[a] = nodeData[a]->template as<float>().ispc;
        }
      }
      return allCompact;
    }

    inline void verifyLevel(uint32_t level)
    {
      if (level >= vklVdbNumLevels()) {
        runtimeError(
            "invalid node level ", level, " for this vdb configuration");
      }
    }

    inline void verifyNodeDataFormat(VKLFormat format, uint32_t level)
    {
      switch (format) {
      case VKL_FORMAT_TILE:
        break;
      case VKL_FORMAT_DENSE_ZYX:
        if (level + 1 < VKL_VDB_NUM_LEVELS) {
          runtimeError("leaf nodes are only supported on the lowest level.");
        }
        break;
      default:
        runtimeError("invalid format specified");
      }
    }

    inline uint64_t getExpectedNumVoxels(VKLFormat format, uint32_t level)
    {
      return (format == VKL_FORMAT_TILE) ? 1 : vklVdbLevelNumVoxels(level);
    }

    template <int W>
    void VdbVolume<W>::commit()
    {
      cleanup();

      filter = (VKLFilter)this->template getParam<int>("filter", filter);
      gradientFilter =
          (VKLFilter)this->template getParam<int>("gradientFilter", filter);
      maxSamplingDepth =
          this->template getParam<int>("maxSamplingDepth", maxSamplingDepth);
      maxSamplingDepth = std::min(maxSamplingDepth, VKL_VDB_NUM_LEVELS - 1u);
      maxIteratorDepth = this->template getParam<int>("maxIteratorDepth",
                                                      VKL_VDB_NUM_LEVELS - 2u);

      // Set up the grid data structure.
      // We use exceptions for error reporting, so make sure to release
      // memory in catch()!
      try {
        grid = allocator.allocate<VdbGrid>(1);

        initIndexSpaceTransforms(
            this, grid->indexToObject, grid->objectToIndex);

        // As a first step, we must find out how many leaves and attributes we
        // have. We do this based on the first node, and then simply enforce
        // that all nodes must share this configuration.

        leafData = getLeafNodeData(this);

        grid->numLeaves                = leafData->size();
        const VKLDataType leafDataType = getLeafDataType(leafData);
        const bool multiAttrib         = (leafDataType == VKL_DATA);
        grid->numAttributes = multiAttrib ? (*leafData)[0]->size() : 1;

        const uint64_t numLeafDataPointers =
            grid->numLeaves * static_cast<uint64_t>(grid->numAttributes);
        if (numLeafDataPointers > VKL_VDB_MAX_NUM_LEAF_DATA) {
          runtimeError(
              "numLeaves * numAttributes in vdb volumes must be less than ",
              VKL_VDB_MAX_NUM_LEAF_DATA);
        }

        // Initialize the attribute type vector. Note that we again use the
        // first node as a template.
        grid->attributeTypes =
            allocator.allocate<uint32_t>(grid->numAttributes);
        if (multiAttrib) {
          for (uint32_t i = 0; i < grid->numAttributes; ++i) {
            grid->attributeTypes[i] =
                (*leafData)[0]->template as<Data *>()[i]->dataType;
          }
        } else {
          grid->attributeTypes[0] = leafDataType;
        }

        Ref<const DataT<uint32_t>> leafLevel =
            this->template getParamDataT<uint32_t>("node.level");
        Ref<const DataT<vec3i>> leafOrigin =
            this->template getParamDataT<vec3i>("node.origin");

        leafFormat = this->template getParamDataT<uint32_t>("node.format");
        grid->leafFormat =
            reinterpret_cast<const VKLFormat *>(leafFormat->data());

        leafTemporalFormat = this->template getParamDataT<uint32_t>(
            "node.temporalFormat", nullptr);
        if (!leafTemporalFormat) {
          leafTemporalFormat = new DataT<uint32_t>(
              grid->numLeaves,
              static_cast<uint32_t>(VKL_TEMPORAL_FORMAT_CONSTANT));
          leafTemporalFormat->refDec();
        }
        grid->leafTemporalFormat = reinterpret_cast<const VKLTemporalFormat *>(
            leafTemporalFormat->data());

        leafStructuredTimesteps = this->template getParamDataT<int>(
            "node.temporallyStructuredNumTimesteps", nullptr);
        leafUnstructuredIndices = this->template getParamDataT<Data *>(
            "node.temporallyUnstructuredIndices", nullptr);
        leafUnstructuredTimes = this->template getParamDataT<Data *>(
            "node.temporallyUnstructuredTimes", nullptr);

        if (leafLevel->size() != grid->numLeaves ||
            leafOrigin->size() != grid->numLeaves ||
            leafFormat->size() != grid->numLeaves ||
            leafTemporalFormat->size() != grid->numLeaves) {
          runtimeError(
              "node.level, node.origin, node.format, "
              "node.temporalFormat, and node.data "
              "must all have the same size");
        }

        if (leafStructuredTimesteps) {
          if (leafStructuredTimesteps->size() != grid->numLeaves) {
            runtimeError(
                "If node.temporallyStructuredNumTimesteps is set, it "
                "must have the same size as node.data");
          }
          grid->leafStructuredTimesteps = leafStructuredTimesteps->data();
        }

        if (leafUnstructuredIndices) {
          if (leafUnstructuredIndices->size() != grid->numLeaves) {
            runtimeError(
                "If node.temporallyUnstructuredIndices is set, it "
                "must have the same size as node.data");
          }
          grid->leafUnstructuredIndices =
              allocator.allocate<ispc::Data1D>(grid->numLeaves);
        }

        if (leafUnstructuredTimes) {
          if (leafUnstructuredTimes->size() != grid->numLeaves) {
            runtimeError(
                "If node.temporallyUnstructuredTimes is set, it "
                "must have the same size as node.data");
          }
          grid->leafUnstructuredTimes =
              allocator.allocate<ispc::Data1D>(grid->numLeaves);
        }

        const box3i bbox =
            computeBbox(grid->numLeaves, *leafLevel, *leafOrigin);
        grid->rootOrigin = computeRootOrigin(bbox);
        grid->activeSize = bbox.upper - grid->rootOrigin;

        // VKL requires a float bbox.
        bounds = empty;

        for (int i = 0; i < 8; ++i) {
          const vec3f v = vec3f((i & 1) ? bbox.upper.x : bbox.lower.x,
                                (i & 2) ? bbox.upper.y : bbox.lower.y,
                                (i & 4) ? bbox.upper.z : bbox.lower.z);

          bounds.extend(xfmPoint(grid->indexToObject, v));
        }

        // Initialize and verify all nodes.
        std::atomic_int allLeavesCompact(true);
        grid->leafData = allocator.allocate<ispc::Data1D>(grid->numLeaves *
                                                          grid->numAttributes);

        tasking::parallel_for(grid->numLeaves, [&](uint64_t i) {
          const uint32_t level = (*leafLevel)[i];
          verifyLevel(level);

          const VKLFormat dataFormat = static_cast<VKLFormat>((*leafFormat)[i]);
          verifyNodeDataFormat(dataFormat, level);

          const uint64_t expectedNumVoxels =
              getExpectedNumVoxels(dataFormat, level);

          const VKLTemporalFormat temporalFormat =
              static_cast<VKLTemporalFormat>((*leafTemporalFormat)[i]);

          const int structuredTimesteps =
              leafStructuredTimesteps ? (*leafStructuredTimesteps)[i] : 0;
          const Data *unstructuredIndices =
              leafUnstructuredIndices ? (*leafUnstructuredIndices)[i] : nullptr;
          const Data *unstructuredTimes =
              leafUnstructuredTimes ? (*leafUnstructuredTimes)[i] : nullptr;

          const uint64_t expectedNumDataElements =
              verifyTemporalData(this->device.ptr,
                                 expectedNumVoxels,
                                 temporalFormat,
                                 structuredTimesteps,
                                 unstructuredIndices,
                                 unstructuredTimes);

          Data *const ld = (*leafData)[i];
          allLeavesCompact &= static_cast<int>(
              initNode(multiAttrib ? ld->template as<Data *>().data() : &ld,
                       expectedNumDataElements,
                       grid->attributeTypes,
                       grid->numAttributes,
                       grid->leafData + i * grid->numAttributes));

          if (unstructuredIndices && unstructuredTimes) {
            assert(temporalFormat == VKL_TEMPORAL_FORMAT_UNSTRUCTURED);
            grid->leafUnstructuredIndices[i] = unstructuredIndices->ispc;
            grid->leafUnstructuredTimes[i]   = unstructuredTimes->ispc;
          }
        });

        grid->allLeavesCompact = static_cast<bool>(allLeavesCompact.load());

        const auto binnedLeaves =
            binLeavesPerLevel(grid->numLeaves, *leafLevel);
        const auto leafOffsets =
            computeLeafOffsets(grid->numLeaves, *leafOrigin, grid->rootOrigin);

        // Allocate buffers for all levels now, all in one go. This makes
        // inserting the nodes (below) much faster.
        std::vector<uint64_t> capacity(vklVdbNumLevels() - 1, 0);
        allocateInnerLevels(
            leafOffsets, binnedLeaves, capacity, grid, allocator);

        // This is where the magic happens. Insert leaves into the data
        // structure top down.
        insertLeaves(leafOffsets,
                     *leafFormat,
                     *leafTemporalFormat,
                     *leafData,
                     binnedLeaves,
                     capacity,
                     grid);

        CALL_ISPC(VdbVolume_setGrid,
                  this->ispcEquivalent,
                  reinterpret_cast<const ispc::VdbGrid *>(grid));

        computeValueRanges(leafOffsets, *leafLevel, *leafFormat, grid);

        // aggregate value range for first attribute only
        valueRange = range1f();
        for (size_t i = 0; i < vklVdbLevelNumVoxels(0); ++i) {
          valueRange.extend(
              grid->levels[0].valueRange[i * grid->numAttributes + 0]);
        }
      } catch (...) {
        cleanup();
        throw;
      }
    }

    template <int W>
    Observer<W> *VdbVolume<W>::newObserver(const char *type)
    {
      if (!grid)
        throw std::runtime_error(
            "Trying to create an observer on a vdb volume that was not "
            "committed.");

      const std::string t(type);

      if (t == "InnerNode") {
        auto *obs = new VdbInnerNodeObserver<W>(*this);
        return obs;
      }

      return Volume<W>::newObserver(type);
    }

    template <int W>
    Sampler<W> *VdbVolume<W>::newSampler()
    {
      return new VdbSampler<W>(*this);
    }

    VKL_REGISTER_VOLUME(VdbVolume<VKL_TARGET_WIDTH>,
                        CONCAT1(internal_vdb_, VKL_TARGET_WIDTH))

  }  // namespace cpu_device
}  // namespace openvkl
