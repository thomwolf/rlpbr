#include "optix_scene.hpp"
#include "optix_utils.hpp"
#include "shader.hpp"

#include <optix_stubs.h>

using namespace std;

namespace RLpbr {
namespace optix {

uint32_t OptixEnvironment::addLight(const glm::vec3 &position,
                  const glm::vec3 &color)
{
    (void)position;
    (void)color;
    return 0;
}

void OptixEnvironment::removeLight(uint32_t light_idx)
{
    (void)light_idx;
}


OptixLoader::OptixLoader(OptixDeviceContext ctx)
    : stream_([]() {
          cudaStream_t strm;
          REQ_CUDA(cudaStreamCreate(&strm));
          return strm;
      }()),
      ctx_(ctx)
{}

shared_ptr<Scene> OptixLoader::loadScene(SceneLoadData &&load_info)
{
    CUdeviceptr scene_storage = (CUdeviceptr)allocCU(load_info.hdr.totalBytes);

    if (holds_alternative<ifstream>(load_info.data)) {
        vector<char> staging(load_info.hdr.totalBytes);
        ifstream &file = *get_if<ifstream>(&load_info.data);
        file.read(staging.data(), load_info.hdr.totalBytes);

        cudaMemcpyAsync((void *)scene_storage, staging.data(), staging.size(),
                        cudaMemcpyHostToDevice, stream_);
    } else {
        cudaMemcpyAsync((void *)scene_storage,
                        get_if<vector<char>>(&load_info.data)->data(),
                        load_info.hdr.totalBytes, cudaMemcpyHostToDevice,
                        stream_);
    }

    OptixBuildInput geometry_info;
    geometry_info.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;

    unsigned int tri_info_flag = OPTIX_GEOMETRY_FLAG_NONE;
    auto &tri_info = geometry_info.triangleArray;
    tri_info.vertexBuffers = &scene_storage;
    tri_info.numVertices = load_info.hdr.numVertices;
    tri_info.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
    tri_info.vertexStrideInBytes = sizeof(Vertex);
    tri_info.indexBuffer = scene_storage + load_info.hdr.indexOffset;
    tri_info.numIndexTriplets = load_info.meshInfo[0].numTriangles;
    tri_info.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
    tri_info.indexStrideInBytes = 0;
    tri_info.preTransform = 0;
    tri_info.flags = &tri_info_flag;
    tri_info.numSbtRecords = 0;
    tri_info.sbtIndexOffsetBuffer = 0;
    tri_info.sbtIndexOffsetSizeInBytes = 0;
    tri_info.sbtIndexOffsetStrideInBytes = 0;
    tri_info.primitiveIndexOffset = 0;
    tri_info.transformFormat = OPTIX_TRANSFORM_FORMAT_NONE;

    OptixAccelBuildOptions accel_options {
        OPTIX_BUILD_FLAG_NONE,
        OPTIX_BUILD_OPERATION_BUILD,
        {},
    };

    OptixAccelBufferSizes buffer_sizes;
    REQ_OPTIX(optixAccelComputeMemoryUsage(ctx_, &accel_options,
                                           &geometry_info, 1,
                                           &buffer_sizes));

    CUdeviceptr scratch_storage =
        (CUdeviceptr)allocCU(buffer_sizes.tempSizeInBytes);

    CUdeviceptr accel_storage =
        (CUdeviceptr)allocCU(buffer_sizes.outputSizeInBytes);

    OptixTraversableHandle accel_struct;
    REQ_OPTIX(optixAccelBuild(ctx_, stream_, &accel_options,
                              &geometry_info, 1,
                              scratch_storage, buffer_sizes.tempSizeInBytes,
                              accel_storage, buffer_sizes.outputSizeInBytes,
                              &accel_struct, nullptr, 0));

    REQ_CUDA(cudaStreamSynchronize(stream_));

    REQ_CUDA(cudaFree((void *)scratch_storage));

    return make_shared<OptixScene>(OptixScene {
        { load_info.envInit },
        scene_storage,
        accel_storage,
        accel_struct,
    });
}

}
}
