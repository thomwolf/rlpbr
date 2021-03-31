#include "scene.hpp"
#include "utils.hpp"

#include <optix_stubs.h>
#include <iostream>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/string_cast.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

using namespace std;

namespace RLpbr {
namespace optix {

OptixScene::~OptixScene()
{
    cudaFreeHost(defaultTLAS.instanceBLASes);

    cudaFree((void *)defaultTLAS.storage);

    for (auto blas_ptr : blasStorage) {
        cudaFree((void *)blas_ptr);
    }

    cudaFree((void *)sceneStorage);
}

static void assignInstanceTransform(OptixInstance &inst,
                                    const glm::mat4x3 &mat)
{
    for (uint32_t col = 0; col < 4; col++) {
        for (uint32_t row = 0; row < 3; row++) {
            inst.transform[row * 4 + col] = mat[col][row];
        }
    }
}

static pair<TLAS, TLASIntermediate> buildTLAS(
    OptixDeviceContext ctx,
    const vector<ObjectInstance> &instances,
    const vector<InstanceTransform> &instance_transforms,
    const vector<InstanceFlags> &instance_flags,
    const OptixTraversableHandle *blases,
    cudaStream_t build_stream,
    bool update = false,
    void *tlas_storage = nullptr,
    OptixTraversableHandle tlas_hdl = 0)
{
    uint32_t num_instances = instances.size();

    OptixInstance *instance_ptr;
    REQ_CUDA(cudaHostAlloc(&instance_ptr, sizeof(OptixInstance) * num_instances,
                           cudaHostAllocMapped));

    OptixTraversableHandle *instance_blases;
    REQ_CUDA(cudaHostAlloc(&instance_blases,
                           sizeof(OptixTraversableHandle) * num_instances,
                           cudaHostAllocMapped));

    for (int inst_id = 0; inst_id < (int)num_instances; inst_id++) {
        const ObjectInstance &inst = instances[inst_id];
        const InstanceTransform &txfm = instance_transforms[inst_id];
        OptixInstance &cur_inst = instance_ptr[inst_id];

        assignInstanceTransform(cur_inst, txfm.mat);
        cur_inst.instanceId = 0;
        cur_inst.sbtOffset = 0;
        if (instance_flags[inst_id] & InstanceFlags::Transparent) {
            cur_inst.visibilityMask = 2;
        } else {
            cur_inst.visibilityMask = 1;
        }
        cur_inst.flags = 0;
        cur_inst.traversableHandle = blases[inst.objectIndex];

        instance_blases[inst_id] = blases[inst.objectIndex];
    }

    OptixBuildInput tlas_build {};
    tlas_build.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
    OptixBuildInputInstanceArray &tlas_instances = tlas_build.instanceArray;
    tlas_instances.instances = (CUdeviceptr)instance_ptr;
    tlas_instances.numInstances = num_instances;

    OptixAccelBuildOptions tlas_options;
    tlas_options.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE | OPTIX_BUILD_FLAG_ALLOW_UPDATE;
    tlas_options.motionOptions = {};

    if (update) {
        tlas_options.operation = OPTIX_BUILD_OPERATION_UPDATE;
    } else {
        tlas_options.operation = OPTIX_BUILD_OPERATION_BUILD;
    }


    OptixAccelBufferSizes tlas_buffer_sizes;
    REQ_OPTIX(optixAccelComputeMemoryUsage(ctx, &tlas_options, &tlas_build, 1,
                                           &tlas_buffer_sizes));

    if (tlas_storage == nullptr) {
        REQ_CUDA(cudaMalloc(&tlas_storage, tlas_buffer_sizes.outputSizeInBytes));
    }
    void *scratch_storage;
    REQ_CUDA(cudaMalloc(&scratch_storage, update ? tlas_buffer_sizes.tempUpdateSizeInBytes : tlas_buffer_sizes.tempSizeInBytes));

    REQ_OPTIX(optixAccelBuild(ctx, build_stream, &tlas_options, &tlas_build,
        1, (CUdeviceptr)scratch_storage,
        update ? tlas_buffer_sizes.tempUpdateSizeInBytes :
                 tlas_buffer_sizes.tempSizeInBytes,
        (CUdeviceptr)tlas_storage, tlas_buffer_sizes.outputSizeInBytes, &tlas_hdl,
        nullptr, 0));

    return {
        TLAS {
            tlas_hdl,
            (CUdeviceptr)tlas_storage,
            tlas_buffer_sizes.outputSizeInBytes,
            instance_blases,
        },
        TLASIntermediate {
            instance_ptr,
            scratch_storage,
        },
    };
}

OptixEnvironment OptixEnvironment::make(OptixDeviceContext ctx,
                                        cudaStream_t build_stream,
                                        const OptixScene &scene)
{
    uint32_t num_instances = scene.envInit.defaultInstances.size();

    InstanceTransform *transforms =
        (InstanceTransform *)allocCU(sizeof(InstanceTransform) * num_instances);
    cudaMemcpyAsync(transforms, scene.defaultTransformBuffer,
                    num_instances * sizeof(InstanceTransform),
                    cudaMemcpyHostToDevice, build_stream);

    void *tlas_storage = allocCU(scene.defaultTLAS.numBytes);
    cudaMemcpyAsync(tlas_storage, (void *)scene.defaultTLAS.storage,
                    scene.defaultTLAS.numBytes, cudaMemcpyDeviceToDevice,
                    build_stream);

    OptixAccelRelocationInfo tlas_reloc_info {};
    REQ_OPTIX(optixAccelGetRelocationInfo(ctx, scene.defaultTLAS.hdl,
                                          &tlas_reloc_info));

    OptixTraversableHandle new_tlas;
    REQ_OPTIX(optixAccelRelocate(ctx, build_stream, &tlas_reloc_info,
        (CUdeviceptr)scene.defaultTLAS.instanceBLASes,
        num_instances, (CUdeviceptr)tlas_storage,
        scene.defaultTLAS.numBytes, &new_tlas));

    REQ_CUDA(cudaStreamSynchronize(build_stream));

    // FIXME, pre-pack this in envInit somehow... backend
    // env init?
    vector<PackedLight> lights;
    lights.reserve(scene.envInit.lights.size());

    for (const auto &light : scene.envInit.lights) {
        PackedLight packed;
        packed.data[0].x = light.color.r;
        packed.data[0].y = light.color.g;
        packed.data[0].z = light.color.b;
        packed.data[0].w = light.position.x;
        packed.data[1].x = light.position.y;
        packed.data[1].y = light.position.z;

        lights.push_back(packed);
    }

    optional<PhysicsEnvironment> physics;
    if (scene.physics.has_value()) {
        physics.emplace(*scene.physics, build_stream);
    }

    return OptixEnvironment {
        {},
        (CUdeviceptr)tlas_storage,
        new_tlas,
        transforms,
        move(lights),
        scene.envInit.defaultInstanceFlags,
        move(physics),
    };
}

OptixEnvironment::~OptixEnvironment()
{
    cudaFree((void *)tlasStorage);
    cudaFree(transformBuffer);
}

uint32_t OptixEnvironment::addLight(const glm::vec3 &position,
                  const glm::vec3 &color)
{
    PackedLight packed;
    packed.data[0].x = color.r;
    packed.data[0].y = color.g;
    packed.data[0].z = color.b;
    packed.data[0].w = position.x;
    packed.data[1].x = position.y;
    packed.data[1].y = position.z;

    lights.push_back(packed);

    return lights.size() - 1;
}

void OptixEnvironment::removeLight(uint32_t light_idx)
{
    lights[light_idx] = lights.back();

    lights.pop_back();
}

TLASIntermediate OptixEnvironment::queueTLASRebuild(const Environment &env,
    OptixDeviceContext ctx, cudaStream_t strm)
{
    const OptixScene &scene = 
        *static_cast<const OptixScene *>(env.getScene().get());
    const OptixEnvironment &env_backend =
        *static_cast<const OptixEnvironment *>(env.getBackend());

    auto [new_tlas, tlas_inter] = buildTLAS(ctx, env.getInstances(),
        env.getTransforms(), env_backend.instanceFlags,
        scene.blases.data(), strm, true,
        (void *)tlasStorage, tlas);

    return tlas_inter;
}

OptixLoader::OptixLoader(OptixDeviceContext ctx, TextureManager &texture_mgr,
                         bool need_physics)
    : stream_([]() {
          cudaStream_t strm;
          REQ_CUDA(cudaStreamCreate(&strm));
          return strm;
      }()),
      ctx_(ctx),
      texture_mgr_(texture_mgr),
      need_physics_(need_physics)
{}

void TLASIntermediate::free()
{
    REQ_CUDA(cudaFreeHost(instanceTransforms));
    REQ_CUDA(cudaFree(buildScratch));
}

static LoadedTextures loadTextures(const TextureInfo &texture_info,
                                   cudaStream_t cpy_strm,
                                   TextureManager &mgr)
{
    vector<void *> host_tex_data;

    auto loadTextureList = [&](const vector<string> &texture_names,
                               TextureFormat fmt) {
        vector<Texture> gpu_textures;
        gpu_textures.reserve(texture_names.size());

        int num_components;
        if (fmt == TextureFormat::R8G8B8A8_SRGB) {
            num_components = 4;
        } else if (fmt == TextureFormat::R8G8B8A8_UNORM) {
            num_components = 4;
        } else if (fmt == TextureFormat::R8G8_UNORM) {
            num_components = 2;
        } else if (fmt == TextureFormat::R8_UNORM) {
            num_components = 1;
        } else {
            num_components = 0; // FIXME
        }

        for (const auto &tex_name : texture_names) {
            string full_path = texture_info.textureDir + tex_name;

            Texture tex = mgr.load(full_path, fmt, cudaAddressModeWrap,
                cpy_strm, [&](const string &tex_path) {
                    int x, y, n;
                    uint8_t *img_data =
                        stbi_load(tex_path.c_str(), &x, &y, &n, 4);
                    host_tex_data.push_back(img_data);

                    // FIXME passing in num_components < 3 to stbi_load
                    // doesn't work properly
                    if (num_components < 4) {
                        for (int i = 0; i < x*y; i++) {
                            img_data[i * num_components] = img_data[i * 4];
                            if (num_components > 1) {
                                img_data[i * num_components + 1] =
                                    img_data[i * 4 + 1];
                            }
                            if (num_components > 2) {
                                img_data[i * num_components + 2] =
                                    img_data[i * 4 + 2];
                            }
                        }
                    }

                    return make_pair(img_data, glm::u32vec2(x, y));
                });

            gpu_textures.emplace_back(move(tex));
        }

        return gpu_textures;
    };

    LoadedTextures loaded;
    loaded.base = loadTextureList(texture_info.base,
                                  TextureFormat::R8G8B8A8_SRGB);
    loaded.metallicRoughness = loadTextureList(texture_info.metallicRoughness,
                                               TextureFormat::R8G8B8A8_UNORM);
    loaded.specular = loadTextureList(texture_info.specular,
                                      TextureFormat::R8G8B8A8_SRGB);
    loaded.normal = loadTextureList(texture_info.normal,
                                    TextureFormat::R8G8_UNORM);
    loaded.emittance = loadTextureList(texture_info.emittance,
                                       TextureFormat::R32G32B32A32_SFLOAT);
    loaded.transmission = loadTextureList(texture_info.transmission,
                                          TextureFormat::R8_UNORM);
    loaded.clearcoat = loadTextureList(texture_info.clearcoat,
                                       TextureFormat::R8G8_UNORM);
    loaded.anisotropic = loadTextureList(texture_info.anisotropic,
                                         TextureFormat::R8G8_UNORM);

    if (!texture_info.envMap.empty()) {
        loaded.envMap.emplace(mgr.load(
            texture_info.textureDir + texture_info.envMap,
            TextureFormat::R32G32B32A32_SFLOAT, cudaAddressModeWrap,
            cpy_strm, [&](const string &tex_path) {
                int x, y, n;
                float *img_data =
                    stbi_loadf(tex_path.c_str(), &x, &y, &n, 4);
                host_tex_data.push_back(img_data);
                return make_pair(img_data, glm::u32vec2(x, y));
            }));
    }

    REQ_CUDA(cudaStreamSynchronize(cpy_strm));

    for (void *ptr : host_tex_data) {
        stbi_image_free(ptr);
    }

    return loaded;
}

shared_ptr<Scene> OptixLoader::loadScene(SceneLoadData &&load_info)
{
    auto textures = loadTextures(load_info.textureInfo, stream_, texture_mgr_);

    size_t num_texture_hdls = load_info.hdr.numMaterials *
        TextureConstants::numTexturesPerMaterial;
    if (textures.envMap.has_value()) {
        num_texture_hdls++;
    }
    size_t texture_ptr_offset = alignOffset(load_info.hdr.totalBytes, 16);
    size_t texture_ptr_bytes = num_texture_hdls * sizeof(cudaTextureObject_t);
    size_t sdf_ptr_offset = 0;
    size_t sdf_ptr_bytes = 0;
    size_t total_device_bytes = 0;
    if (need_physics_) {
        sdf_ptr_offset =
            alignOffset(texture_ptr_offset + texture_ptr_bytes, 16);
        sdf_ptr_bytes =
            load_info.physics.sdfPaths.size() * sizeof(cudaTextureObject_t);

        total_device_bytes = sdf_ptr_offset + sdf_ptr_bytes;
    } else {
        total_device_bytes = texture_ptr_offset + texture_ptr_bytes;
    }

    char *scene_storage = (char *)allocCU(total_device_bytes);

    char *data_src = nullptr;
    bool cuda_staging = false;

    if (holds_alternative<ifstream>(load_info.data)) {
        REQ_CUDA(cudaHostAlloc((void **)&data_src, load_info.hdr.totalBytes,
                               cudaHostAllocWriteCombined));
        cuda_staging = true;

        ifstream &file = *get_if<ifstream>(&load_info.data);
        file.read(data_src, load_info.hdr.totalBytes);
    } else {
        data_src = get_if<vector<char>>(&load_info.data)->data();
    }

    cudaMemcpyAsync(scene_storage, data_src, load_info.hdr.totalBytes,
                    cudaMemcpyHostToDevice, stream_);

    // Build BLASes
    uint32_t num_objects = load_info.objectInfo.size();

    vector<CUdeviceptr> blas_storage;
    blas_storage.reserve(num_objects);

    vector<OptixTraversableHandle> blases;
    blases.reserve(num_objects);

    // Improves performance slightly
    unsigned int tri_build_flag = OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT;

    OptixAccelBuildOptions accel_options {
        OPTIX_BUILD_FLAG_PREFER_FAST_TRACE,
        OPTIX_BUILD_OPERATION_BUILD,
        {},
    };

    CUdeviceptr scene_storage_dev = (CUdeviceptr)scene_storage;
    const DevicePackedVertex *base_vertex_ptr =
        (const DevicePackedVertex *)scene_storage;
    const uint32_t *base_index_ptr = 
        (const uint32_t *)(scene_storage + load_info.hdr.indexOffset);

    static_assert(sizeof(PackedVertex) == sizeof(Vertex));

    for (int obj_idx = 0; obj_idx < (int)num_objects; obj_idx++) {
        const ObjectInfo &object_info = load_info.objectInfo[obj_idx];
        vector<OptixBuildInput> geometry_infos;
        geometry_infos.reserve(object_info.numMeshes);

        for (int obj_mesh_idx = 0; obj_mesh_idx < (int)object_info.numMeshes;
             obj_mesh_idx++) {
            uint32_t mesh_idx = object_info.meshIndex + obj_mesh_idx;
            const MeshInfo &mesh_info = load_info.meshInfo[mesh_idx];

            OptixBuildInput geometry_info {};
            geometry_info.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;

            CUdeviceptr index_ptr =
                (CUdeviceptr)(base_index_ptr + mesh_info.indexOffset);
            auto &tri_info = geometry_info.triangleArray;
            tri_info.vertexBuffers = &scene_storage_dev;
            tri_info.numVertices = load_info.hdr.numVertices;
            tri_info.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
            tri_info.vertexStrideInBytes = sizeof(Vertex);
            tri_info.indexBuffer = index_ptr;
            tri_info.numIndexTriplets = mesh_info.numTriangles;
            tri_info.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
            tri_info.indexStrideInBytes = 0;
            tri_info.preTransform = 0;
            tri_info.flags = &tri_build_flag;
            tri_info.numSbtRecords = 1;
            tri_info.sbtIndexOffsetBuffer = 0;
            tri_info.sbtIndexOffsetSizeInBytes = 0;
            tri_info.sbtIndexOffsetStrideInBytes = 0;
            tri_info.primitiveIndexOffset = 0;
            tri_info.transformFormat = OPTIX_TRANSFORM_FORMAT_NONE;

            geometry_infos.push_back(geometry_info);
        }

        OptixAccelBufferSizes buffer_sizes;
        REQ_OPTIX(optixAccelComputeMemoryUsage(ctx_, &accel_options,
                                               geometry_infos.data(),
                                               geometry_infos.size(),
                                               &buffer_sizes));

        CUdeviceptr scratch_storage =
            (CUdeviceptr)allocCU(buffer_sizes.tempSizeInBytes);

        CUdeviceptr accel_storage =
            (CUdeviceptr)allocCU(buffer_sizes.outputSizeInBytes);

        OptixTraversableHandle blas;
        REQ_OPTIX(optixAccelBuild(ctx_, stream_, &accel_options,
                                  geometry_infos.data(), geometry_infos.size(),
                                  scratch_storage, buffer_sizes.tempSizeInBytes,
                                  accel_storage, buffer_sizes.outputSizeInBytes,
                                  &blas, nullptr, 0));

        REQ_CUDA(cudaStreamSynchronize(stream_));

        REQ_CUDA(cudaFree((void *)scratch_storage));

        blases.push_back(blas);
        blas_storage.push_back(accel_storage);
    }

    auto [tlas, tlas_inter] = buildTLAS(ctx_, load_info.envInit.defaultInstances,
        load_info.envInit.defaultTransforms,
        load_info.envInit.defaultInstanceFlags,
        blases.data(), stream_);

    InstanceTransform *default_transforms =
        (InstanceTransform *)allocCU(sizeof(InstanceTransform) *
                                  load_info.envInit.defaultTransforms.size());

    cudaMemcpyAsync(default_transforms, load_info.envInit.defaultTransforms.data(),
        sizeof(InstanceTransform) * load_info.envInit.defaultTransforms.size(),
        cudaMemcpyHostToDevice, stream_);

    // Based on indices in MaterialTextures build indexable list of handles
    vector<cudaTextureObject_t> texture_hdls;
    texture_hdls.reserve(num_texture_hdls);
    if (textures.envMap.has_value()) {
        texture_hdls.push_back(textures.envMap->getHandle());
    }
    for (int mat_idx = 0; mat_idx < (int)load_info.hdr.numMaterials;
         mat_idx++) {
        const MaterialTextures &tex_indices =
            load_info.textureIndices[mat_idx];

        auto appendHandle = [&](uint32_t idx, const auto &texture_list) {
            if (idx != ~0u) {
                texture_hdls.push_back(texture_list[idx].getHandle());
            } else {
                texture_hdls.push_back(0);
            }
        };

        appendHandle(tex_indices.baseColorIdx, textures.base);
        appendHandle(tex_indices.metallicRoughnessIdx,
                     textures.metallicRoughness);
        appendHandle(tex_indices.specularIdx, textures.specular);
        appendHandle(tex_indices.normalIdx, textures.normal);
        appendHandle(tex_indices.emittanceIdx, textures.emittance);
        appendHandle(tex_indices.transmissionIdx, textures.transmission);
        appendHandle(tex_indices.clearcoatIdx, textures.clearcoat);
        appendHandle(tex_indices.anisoIdx, textures.anisotropic);
    }

    cudaTextureObject_t *tex_gpu_ptr = (cudaTextureObject_t *)(
        scene_storage + texture_ptr_offset);
    cudaMemcpyAsync(tex_gpu_ptr, texture_hdls.data(), texture_ptr_bytes,
                    cudaMemcpyHostToDevice, stream_);

    REQ_CUDA(cudaStreamSynchronize(stream_));
    tlas_inter.free();
    
    if (cuda_staging) {
        REQ_CUDA(cudaFreeHost(data_src));
    }

    optional<ScenePhysicsData> physics;
    if (need_physics_) {
        physics.emplace(ScenePhysicsData::make(load_info.physics,
                                               load_info.hdr, 
                                               scene_storage,
                                               scene_storage + sdf_ptr_offset,
                                               stream_, texture_mgr_));
    }

    return shared_ptr<OptixScene>(new OptixScene {
        {
            move(load_info.meshInfo),
            move(load_info.objectInfo),
            move(load_info.envInit),
        },
        scene_storage_dev,
        base_vertex_ptr,
        base_index_ptr,
        reinterpret_cast<const PackedMaterial *>(
            scene_storage + load_info.hdr.materialOffset),
        reinterpret_cast<const PackedMeshInfo *>(
            scene_storage + load_info.hdr.meshOffset),
        move(blas_storage),
        move(blases),
        move(tlas),
        default_transforms,
        move(textures),
        tex_gpu_ptr,
        move(physics),
    });
}

}
}
