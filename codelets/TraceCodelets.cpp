// Copyright (c) 2022 Graphcore Ltd. All rights reserved.

// This file contains IPU compute codelets (kernels) for ray/path-tracing.

#define DEBUG 0
#include "debug_print.hpp"

#include <poplar/Vertex.hpp>
#include <poplar/HalfFloat.hpp>
#include <poplar/StackSizeDefs.hpp>

#include <Arrays.hpp>
#include <Primitives.hpp>
#include <Mesh.hpp>
#include <Scene.hpp>
#include <CompactBvh.hpp>
#include <Material.hpp>
#include <Render.hpp>
#include <BxDF.hpp>
#include <embree_utils/geometry.hpp>

#include <serialisation/Deserialiser.hpp>
#include <serialisation/deserialisation.hpp>

#include <new>

#include "sincos.hpp"

using namespace poplar;
using namespace embree_utils;

// Manually set the stack size for the codelets. We need to do this
// because we have been lazy by recording the BVH traversal on the
// worker stacks. TODO: connect a tensor so graph construction
// guarantees space for the BVH traversal (we know the max depth
// of the tree at compute graph construction/compile time):
DEF_STACK_USAGE(960, __runCodelet_PathTrace);
DEF_STACK_USAGE(960, __runCodelet_ShadowTrace);

// Utility to get a uniform sample between 0 and 1
// from the IPU's hardware RNG:
inline float hw_uniform_0_1() {
  return __builtin_ipu_urand_f32() + .5f;
}

// Global data (per-tile) that stores scene data.
// This is a workaround for Poplar not supporting
// connection of arbitrary structured data to vertices.
// It is global so that we only have to pay the cost of
// unpacking it once when/if it changes:
SceneRef tileLocalScene;
ArrayRef<Sphere> wrappedSpheres;
ArrayRef<Disc> wrappedDiscs;
ArrayRef<CompiledTriangleMesh> wrappedMeshes;

/// Some objects have been transferred direct from CPU to IPU but
/// will contain incomptible pointer data but compatible plain old data.
/// This vertex re-allocates these objects/structures for the IPU.
/// Note: this is very questionable but it simplifies things considerably.
/// At some point this will vanish anyway as the scene description will be
/// organised into opaque binary chunks so that we can page chunks in and
/// out of SRAM:
class BuildDataStructures : public Vertex {
public:
  // Scene description:
  InOut<Vector<unsigned char, poplar::VectorLayout::SPAN, alignof(Sphere)>> spheres;
  InOut<Vector<unsigned char, poplar::VectorLayout::SPAN, alignof(Disc)>> discs;
  InOut<Vector<unsigned char, poplar::VectorLayout::SPAN, alignof(CompiledTriangleMesh)>> meshes;
  Input<Vector<unsigned char, poplar::VectorLayout::SPAN, 16>> serialisedScene;

  bool compute() {
    // De-serialise the scene ref data. This is stored in a tile-global object.
    // The global object can be used by any subsequent vertex that keeps the
    // `serialisedScene` byte tensor live:
    Deserialiser<16> d(&serialisedScene[0], serialisedScene.size());
    d >> tileLocalScene;

    // For each primitive copy its position to a tensor:
    wrappedSpheres = ArrayRef<Sphere>::reinterpret(&spheres[0], spheres.size());
    wrappedDiscs   = ArrayRef<Disc>::reinterpret(&discs[0], discs.size());
    wrappedMeshes  = ArrayRef<CompiledTriangleMesh>::reinterpret(&meshes[0], meshes.size());

    // Need to re-new anything that inherits from Primitive i.e. reconstruct in place
    // using placement-new! Any struct with pointers (e.g. vtable) will not be
    // compatible between IPU and host.
    //
    // There are some assumptions that make this work:
    // 1. Host pointers are larger than IPU pointers so everything has been over allocated.
    // 2. V-tables come at the end so other data has same layout.
    // 3. Tensors are not moved on IPU between this vertex and the trace vertex running
    //    (we are storing pointers for reuse between different vertices!)
    // Basically this is all very dubious but works for now...

    // Mesh objects are constructed from the mesh info data:
    auto meshIdx = 0u;
    for (const auto& info : tileLocalScene.meshInfo) {
      auto firstNormalIndex = 0u;
      auto numNormals = 0u;
      if (tileLocalScene.meshNormals.size()) {
        // If scene has normals assume every mesh has normals:
        firstNormalIndex = info.firstVertex;
        numNormals = info.numVertices;
      }
      new ((void*)&wrappedMeshes[meshIdx]) CompiledTriangleMesh(
        embree_utils::Bounds3d(),
        ArrayRef(&tileLocalScene.meshTris[info.firstIndex], info.numTriangles),
        ArrayRef(&tileLocalScene.meshVerts[info.firstVertex], info.numVertices),
        ArrayRef(&tileLocalScene.meshNormals[firstNormalIndex], numNormals)
      );
      meshIdx += 1;
    }

    // Other primitives are "re-newed" using their own data:
    for (auto &s : wrappedSpheres) {
      new ((void*)&s) Sphere(Vec3fa(s.x, s.y, s.z), s.radius);
    }

    for (auto &d : wrappedDiscs) {
      new ((void*)&d) Disc(Vec3fa(d.nx, d.ny, d.nz), Vec3fa(d.cx, d.cy, d.cz), d.r);
    }

    return true;
  }
};

// Look up the underlying primitive from a geometry type and ID:
const Primitive* primLookup(std::uint16_t geomID, std::uint32_t primID) {
  const auto& geom = tileLocalScene.geometry[geomID];
  switch (geom.type) {
    case GeomType::Mesh:
      return &wrappedMeshes[geom.index];
    case GeomType::Sphere:
      return &wrappedSpheres[geom.index];
    case GeomType::Disc:
      return &wrappedDiscs[geom.index];
    case GeomType::NumTypes:
    default:
      return nullptr;
  }
}

void sampleCameraRays(int workerID,
                      float imageWidth, float imageHeight,
                      float2 antiAliasScale, float fovRadians,
                      ArrayRef<embree_utils::TraceResult>& wrappedRays) {
  // Do trig outside of loop:
  float s, c;
  sincos(fovRadians / 2.f, s, c);
  const auto fovTanTheta = s / c;
  const auto rayOrigin = embree_utils::Vec3fa(0.f, 0.f, 0.f);

  // Generate camera rays. Each worker starts processing offset by their worker IDs.
  // The external Poplar graph construction code ensures the number of rays to process on each
  // tile is a multiple of 6 (by padding or otherwise):
  for (auto r = workerID; r < wrappedRays.size(); r += poplar::MultiVertex::numWorkers()) {
    auto& result = wrappedRays[r];
    // Sample around the pixel coord in the ray stream (anti-aliasing):
    float2 g = __builtin_ipu_f32v2grand();
    float2 p = {result.p.u, result.p.v}; // row, col
    p += antiAliasScale * g;
    const auto rayDir = pixelToRayDir(p[1], p[0], imageWidth, imageHeight, fovTanTheta);
    result.h = embree_utils::HitRecord(rayOrigin, rayDir);
  }
}

/// Simple uni-directional path trace vertex. Rays are path traced one by one
/// alternating BVH intersection and BxDF sampling to produce the incoming ray
/// direction. There is no light sampling so we rely on hitting light sources by
/// chance.
class PathTrace : public MultiVertex {
public:
  // Storage for sphere, disc, and mesh primitives:
  Input<Vector<unsigned char, poplar::VectorLayout::SPAN, alignof(Sphere)>> spheres;
  Input<Vector<unsigned char, poplar::VectorLayout::SPAN, alignof(Disc)>> discs;
  Input<Vector<unsigned char, poplar::VectorLayout::SPAN, alignof(CompiledTriangleMesh)>> meshes;

  // Scene description and BVH:
  Input<Vector<unsigned char, poplar::VectorLayout::SPAN, 16>> serialisedScene;

  // Ray stream:
  InOut<Vector<unsigned char, poplar::VectorLayout::SPAN, alignof(TraceResult)>> rays;
  Input<unsigned> vertexSampleCount; // Number of samples to take inside the vertex itself

  bool compute(unsigned int workerID) {
    // Wrap all byte arrays with their correct types:
    auto wrappedRays = ArrayRef<embree_utils::TraceResult>::reinterpret(&rays[0], rays.size());

    // Construct a BVH from all the wrapped arrays:
    CompactBvh bvh(tileLocalScene.bvhNodes, tileLocalScene.maxLeafDepth);

    for (auto s = 0u; s < vertexSampleCount; ++s) {
      // Generate ray samples:
      sampleCameraRays(workerID, tileLocalScene.imageWidth, tileLocalScene.imageHeight,
                      float2{tileLocalScene.antiAliasScale, tileLocalScene.antiAliasScale},
                      tileLocalScene.fovRadians, wrappedRays);

      // Intersect all rays against the BVH. Each worker starts processing offset by their worker IDs.
      // The external Poplar graph construction code ensures the number of rays to process on each
      // tile is a multiple of 6 (by padding or otherwise):
      for (auto r = workerID; r < wrappedRays.size(); r += numWorkers()) {
        auto& result = wrappedRays[r];
        auto& hit = result.h;
        hit.throughput = Vec3fa(1.f, 1.f, 1.f);
        Vec3fa color(0.f, 0.f, 0.f);

        for (auto i = 0u; i < tileLocalScene.maxPathLength; ++i) {
          offsetRay(hit.r, hit.normal); // offset rays to avoid self intersection.
          // Reset ray limits for next bounce:
          hit.r.tMin = 0.f;
          hit.r.tMax = std::numeric_limits<float>::infinity();
          auto intersected = bvh.intersect(hit.r, primLookup);

          if (intersected) {
            updateHit(intersected, hit);
            const auto& material = tileLocalScene.materials[tileLocalScene.matIDs[hit.geomID]];

            if (material.emissive) {
              color += hit.throughput * material.emission;
            }

            if (material.type == Material::Type::Diffuse) {
              // Use HW random number generator for samples:
              const float u1 = hw_uniform_0_1();
              const float u2 = hw_uniform_0_1();
              hit.r.direction = sampleDiffuse(hit.normal, u1, u2);
              // Update throughput
              //const float w = std::abs(wiWorld.dot(normal));
              //const float pdf = cosineHemispherePdf(wiTangent);
              // The terms w / (Pi * pdf) all cancel for diffuse throughput:
              hit.throughput *= material.albedo; // * (w / (Pi * pdf)); // PDF terms cancel for cosine weighted samples
              //throughput *= material.albedo * (wiTangent.z * 2.f); // Apply PDF for hemisphere samples (sampleDir is in tangent space so cos(theta) == z-coord).
            } else if (material.type == Material::Type::Specular) {
              hit.r.direction = reflect(hit.r.direction, hit.normal);
              hit.throughput *= material.albedo;
            } else if (material.type == Material::Type::Refractive) {
              const float u1 = hw_uniform_0_1();
              const auto [dir, refracted] = dielectric(hit.r, hit.normal, material.ior, u1);
              hit.r.direction = dir;
              if (refracted) { hit.throughput *= material.albedo; }
            } else {
              // Mark an error:
              result.rgb *= std::numeric_limits<float>::quiet_NaN();
              hit.flags |= HitRecord::ERROR;
            }
          } else {
            hit.flags |= HitRecord::ESCAPED;
            break;
          }

          // Random stopping:
          if (i > tileLocalScene.rouletteStartDepth) {
            const float u1 = hw_uniform_0_1();
            if (evaluateRoulette(u1, hit.throughput)) { break; }
          }
        } // end of path trace loop

        result.rgb += color;
      } // end of loop over rays

    } // end of sampling loop

    return true;
  }
};

/// Simple ray trace vertex primarily intended for testing and validation. The vertex intersects
/// every ray with the BVH to get primary hits and then traces one shadow ray from each hit
/// to a fixed point light source.
class ShadowTrace : public MultiVertex {
public:
  // Storage for sphere, disc, and mesh primitives:
  Input<Vector<unsigned char, poplar::VectorLayout::SPAN, alignof(Sphere)>> spheres;
  Input<Vector<unsigned char, poplar::VectorLayout::SPAN, alignof(Disc)>> discs;
  Input<Vector<unsigned char, poplar::VectorLayout::SPAN, alignof(CompiledTriangleMesh)>> meshes;

  // Scene description and BVH:
  Input<Vector<unsigned char, poplar::VectorLayout::SPAN, 16>> serialisedScene;

  // Other scene parameters:
  float ambientLightFactor;
  Input<Vector<float>> lightPos;

  // Ray stream:
  InOut<Vector<unsigned char, poplar::VectorLayout::SPAN, alignof(TraceResult)>> rays;

  bool compute(unsigned int workerID) {
    // Unpack the scene data:
    Deserialiser<16> d(&serialisedScene[0], serialisedScene.size());
    d >> tileLocalScene;

    auto wrappedRays = ArrayRef<embree_utils::TraceResult>::reinterpret(&rays[0], rays.size());

    // Construct a BVH from all the wrapped arrays:
    CompactBvh bvh(tileLocalScene.bvhNodes, tileLocalScene.maxLeafDepth);

    Vec3fa lp(lightPos[0], lightPos[1], lightPos[2]);

    // Note: there is no need for ray gen in this vertex since we are tracing
    // rays that were initialised on the host.

    // Intersect all rays against the BVH. Each worker starts processing offset by their worker IDs.
    // The external Poplar graph construction code ensures the number of rays to process on each
    // tile is a multiple of 6 (by padding or otherwise):
    for (auto r = workerID; r < wrappedRays.size(); r += numWorkers()) {
      auto& result = wrappedRays[r];

      traceShadowRay(
        bvh,
        tileLocalScene.matIDs, tileLocalScene.materials,
        ambientLightFactor,
        result, primLookup, lp);
    }

    return true;
  }
};

// Take ray results and calculate UV coords for all the escaped rays
// in order to lookup lighting values from the HDRI environment map.
// UVs are calculated using equirectangular projection.
class PreProcessEscapedRays : public MultiVertex {
public:
  InOut<Vector<unsigned char, poplar::VectorLayout::SPAN, alignof(TraceResult)>> results;
  Input<float> azimuthRotation;
  Output<Vector<float>> u;
  Output<Vector<float>> v;

  bool compute(unsigned workerId) {
    constexpr auto workerCount = numWorkers();
    auto wrappedResults = ConstArrayRef<embree_utils::TraceResult>::reinterpret(&results[0], results.size());

    // Parallelise over all workers (each worker starts at a different offset):
    for (auto r = workerId; r < wrappedResults.size(); r += workerCount) {
      auto& result = wrappedResults[r];
      auto& hit = result.h;
      if (hit.flags & HitRecord::ESCAPED) {
        const auto& dir = hit.r.direction;
        // Convert ray direction to UV coords using equirectangular projection.
        // Calc assumes ray-dir was already normalised (note: normalised in Ray constructor).
        auto theta = acosf(dir.y);
        auto phi = atan2(dir.z, dir.x) + azimuthRotation;
        if (phi < 0.f) {
          phi += TwoPi;
        } else if (phi > TwoPi) {
          phi -= TwoPi;
        }
        u[r] = theta * InvPi;
        v[r] = phi * Inv2Pi;
      } else {
        // Avoid fp exceptions as these could otherwise remain uninitialised:
        u[r] = 0.f;
        v[r] = 0.f;
      }
    }

    return true;
  }
};

// Update escaped rays with the result of env-map lighting lookup:
class PostProcessEscapedRays : public MultiVertex {
public:
  InOut<Vector<unsigned char, poplar::VectorLayout::SPAN, alignof(TraceResult)>> results;
  Vector<Input<Vector<float>>> bgr;

  bool compute(unsigned workerId) {
    constexpr auto workerCount = numWorkers();
    auto wrappedResults = ArrayRef<embree_utils::TraceResult>::reinterpret(&results[0], results.size());

    // Parallelise over all workers (each worker starts at a different offset):
    for (auto r = workerId; r < wrappedResults.size(); r += workerCount) {
      auto& result = wrappedResults[r];
      auto& hit = result.h;
      if (hit.flags & HitRecord::ESCAPED) {
        auto v = bgr[r];
        result.rgb += hit.throughput * Vec3fa(v[2], v[1], v[0]);
      }
    }

    return true;
  }
};
