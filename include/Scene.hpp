// Copyright (c) 2022 Graphcore Ltd. All rights reserved.

// Utility structures to ease movement of scene data and parameters.
// TODO: Needs sanitising and de-duplication.

#pragma once

#include "Primitives.hpp"
#include "Mesh.hpp"
#include "Arrays.hpp"
#include "Material.hpp"

enum class GeomType : uint8_t {
  Mesh = 0,
  Sphere,
  Disc,
  NumTypes
};

struct CropWindow {
  std::int32_t w;
  std::int32_t h;
  std::int32_t c;
  std::int32_t r;
};

struct GeomRef {
  GeomRef(std::uint16_t i, GeomType t) : index(i), type(t), pad(0) {}
  std::uint16_t index;
  GeomType type;
  std::uint8_t pad;
};

#ifndef __IPU__

struct SceneData {
  std::vector<GeomRef> geometry; // Geometric primitive array
  std::vector<MeshInfo> meshInfo; // Stores offsets for each mesh in the unified index and vertex buffers
  std::vector<Triangle> meshTris;
  std::vector<embree_utils::Vec3fa> meshVerts;
  std::vector<embree_utils::Vec3fa> meshNormals;
  std::vector<std::uint32_t> matIDs;  // Material index corresponding to each primitive
  std::vector<Material> materials;   // Materials
  std::vector<CompactBVH2Node> bvhNodes; // BVH Nodes
  std::uint32_t bvhMaxDepth;
};

#endif // ifndef __IPU__

struct SceneRef {
  ArrayRef<GeomRef> geometry;
  ArrayRef<MeshInfo> meshInfo;
  ArrayRef<Triangle> meshTris;
  ArrayRef<embree_utils::Vec3fa> meshVerts;
  ArrayRef<embree_utils::Vec3fa> meshNormals;
  ArrayRef<std::uint32_t> matIDs;
  ArrayRef<Material> materials;
  ArrayRef<CompactBVH2Node> bvhNodes;
  std::uint32_t maxLeafDepth; // Max depth of BVH tree: i.e. size of stack required for traversal.

  // Params used in path-trace kernel:
  float imageWidth;
  float imageHeight;
  float fovRadians;
  float antiAliasScale;
  std::uint32_t maxPathLength; // Hard limit on number of bounces in path tracing.
  std::uint32_t rouletteStartDepth; // Random stopping enabled at this depth

  // Params used external to kernel:
  std::uint32_t samplesPerPixel;
  std::uint64_t rngSeed;
  CropWindow window;
  bool pathTrace;
};

class Intersector {};
