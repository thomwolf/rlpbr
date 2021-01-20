#include "optix_render.hpp"
#include "optix_utils.hpp"
#include "utils.hpp"

#include <cuda_runtime.h>
#include <nvrtc.h>
#include <optix.h>
#include <optix_function_table_definition.h>
#include <optix_stubs.h>

#include <iostream>

using namespace std;

namespace RLpbr {
namespace optix {

static void optixLog(unsigned int level, const char *tag,
                     const char *message, void *)
{
    cerr << "[" << std::setw( 2 ) << level << "][" << std::setw( 12 )
         << tag << "]: " << message << "\n";
}

static OptixDeviceContext initializeOptix(uint32_t gpu_id)
{
    REQ_CUDA(cudaSetDevice(gpu_id));

    // FIXME Drop stubs
    auto res = optixInit();
    if (res != OPTIX_SUCCESS) {
        cerr << "Optix initialization failed" << endl;
        abort();
    }

    OptixDeviceContextOptions optix_opts {};
    optix_opts.logCallbackFunction = &optixLog;
    optix_opts.logCallbackLevel = 4;

    CUcontext cuda_ctx = nullptr;

    OptixDeviceContext optix_ctx;
    REQ_OPTIX(optixDeviceContextCreate(cuda_ctx, &optix_opts, &optix_ctx));

    return optix_ctx;
}

static vector<char> compileToPTX(const char *cu_path)
{
    ifstream cu_file(cu_path, ios::binary);
    cu_file.seekg(ios::end);
    size_t num_cu_bytes = cu_file.tellg();
    cu_file.seekg(ios::beg);
    vector<char> cu_src(num_cu_bytes);
    cu_file.read(cu_src.data(), num_cu_bytes);
    cu_file.close();

    nvrtcProgram prog;
    auto res = nvrtcCreateProgram(&prog, cu_src.data(), cu_path, 0,
                                  nullptr, nullptr);

    if (res != NVRTC_SUCCESS) {
        cerr << "Creating NVRTC program failed" << endl;
        abort();
    }

    res = nvrtcCompileProgram(prog, 0, nullptr);

    if (res != NVRTC_SUCCESS) {
        cerr << "NVRTC compilation failed" << endl;
        abort();
    }

    size_t num_ptx_bytes;
    res = nvrtcGetPTXSize(prog, &num_ptx_bytes);
    if (res != NVRTC_SUCCESS) {
        cerr << "NVRTC ptx bytes failed" << endl;
        abort();
    }

    vector<char> ptx_data(num_ptx_bytes);

    res = nvrtcGetPTX(prog, ptx_data.data());
    if (res != NVRTC_SUCCESS) {
        cerr << "NVRTC failed to get PTX" << endl;
        abort();
    }

    res = nvrtcDestroyProgram(&prog);
    if (res != NVRTC_SUCCESS) {
        cerr << "NVRTC cleanup failed" << endl;
        abort();
    }

    return ptx_data;
}

static Pipeline buildPipeline(OptixDeviceContext ctx)
{
    OptixModuleCompileOptions module_compile_options {};

    OptixPipelineCompileOptions pipeline_compile_options {};
    pipeline_compile_options.traversableGraphFlags =
        OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;

    pipeline_compile_options.numPayloadValues = 1;
    
    pipeline_compile_options.pipelineLaunchParamsVariableName = "params";
    
    vector<char> ptx = compileToPTX("/home/bps/rl/rlpbr/src/optix_shader.cu");

    static constexpr size_t log_bytes = 2048;
    static char log_str[log_bytes];
    size_t log_bytes_written = log_bytes;
    
    OptixModule shader_module;
    REQ_OPTIX(optixModuleCreateFromPTX(ctx, &module_compile_options,
        &pipeline_compile_options, ptx.data(), ptx.size(),
        log_str, &log_bytes_written, &shader_module));

    OptixProgramGroup raygen_group;
    OptixProgramGroup miss_group;
    OptixProgramGroup hit_group;
    
    OptixProgramGroupOptions program_group_options {};
    OptixProgramGroupDesc raygen_prog_group_desc {};
    raygen_prog_group_desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    raygen_prog_group_desc.raygen.module = shader_module;
    raygen_prog_group_desc.raygen.entryFunctionName = "__raygen__rg";

    REQ_OPTIX(optixProgramGroupCreate(
        ctx,
        &raygen_prog_group_desc,
        1, // num program groups
        &program_group_options,
        nullptr,
        nullptr,
        &raygen_group));
    
    OptixProgramGroupDesc miss_prog_group_desc = {};
    miss_prog_group_desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    miss_prog_group_desc.miss.module = shader_module;
    miss_prog_group_desc.miss.entryFunctionName = "__miss__ms";
    REQ_OPTIX(optixProgramGroupCreate(
        ctx,
        &miss_prog_group_desc,
        1, // num program groups
        &program_group_options,
        nullptr,
        nullptr,
        &miss_group));
    
    OptixProgramGroupDesc hitgroup_prog_group_desc = {};
    hitgroup_prog_group_desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    hitgroup_prog_group_desc.hitgroup.moduleCH = shader_module;
    hitgroup_prog_group_desc.hitgroup.entryFunctionNameCH = "__closesthit__ch";
    REQ_OPTIX(optixProgramGroupCreate(
        ctx,
        &hitgroup_prog_group_desc,
        1, // num program groups
        &program_group_options,
        nullptr,
        nullptr,
        &hit_group));

    array program_groups {
        raygen_group,
        miss_group,
        hit_group,
    };

    OptixPipelineLinkOptions pipeline_link_options = {};
    pipeline_link_options.maxTraceDepth = 1;
    pipeline_link_options.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_FULL;
    
    OptixPipeline pipeline = nullptr;
    REQ_OPTIX(optixPipelineCreate(
        ctx,
        &pipeline_compile_options,
        &pipeline_link_options,
        program_groups.data(),
        program_groups.size(),
        nullptr,
        nullptr,
        &pipeline));

    return Pipeline {
        shader_module,
        move(program_groups),
        pipeline,
    };
}

static SBT buildSBT(cudaStream_t strm, const Pipeline &pipeline)
{
    struct EmptyEntry {
        __align__( OPTIX_SBT_RECORD_ALIGNMENT)
        char header[OPTIX_SBT_RECORD_HEADER_SIZE];
    };

    CUdeviceptr storage = (CUdeviceptr)allocCU(sizeof(EmptyEntry) * 3);

    array<CUdeviceptr, 3> offsets;

    CUdeviceptr cur_ptr = storage;
    for (uint32_t i = 0; i < 3; i++) {
        offsets[i] = cur_ptr;

        EmptyEntry stage_entry;
        REQ_OPTIX(optixSbtRecordPackHeader(pipeline.groups[i], &stage_entry));

        REQ_CUDA(cudaMemcpyAsync((void *)cur_ptr, &stage_entry,
            sizeof(EmptyEntry), cudaMemcpyHostToDevice, strm));

        cur_ptr += sizeof(EmptyEntry);
    }

    OptixShaderBindingTable sbt;
    sbt.raygenRecord = offsets[0];
    sbt.exceptionRecord = 0;

    sbt.missRecordBase = offsets[1];
    sbt.missRecordStrideInBytes = sizeof(EmptyEntry);
    sbt.missRecordCount = 1;

    sbt.hitgroupRecordBase = offsets[2];
    sbt.hitgroupRecordStrideInBytes = sizeof(EmptyEntry);
    sbt.hitgroupRecordCount = 1;

    sbt.callablesRecordBase = 0;
    sbt.callablesRecordStrideInBytes = 0;
    sbt.callablesRecordCount = 0;

    return SBT {
        storage,
        sbt,
    };
}

static RenderState makeRenderState(const RenderConfig &cfg, cudaStream_t strm,
                                   uint32_t num_frames)
{
    uint64_t total_param_bytes = sizeof(OptixTraversableHandle) * cfg.batchSize;
    uint64_t camera_offset = alignOffset(total_param_bytes, 16);
    total_param_bytes = camera_offset + sizeof(CameraParams) * cfg.batchSize;
    total_param_bytes = alignOffset(total_param_bytes, 16);

    void *param_buffer;
    REQ_CUDA(cudaHostAlloc(&param_buffer, total_param_bytes * num_frames,
        cudaHostAllocMapped | cudaHostAllocWriteCombined));

    RenderState state {
        (float *)allocCU(sizeof(float) * cfg.batchSize * cfg.imgHeight *
                         cfg.imgWidth * num_frames),
        param_buffer,
        (ShaderParams *)allocCU(
            alignOffset(sizeof(ShaderParams), 16) * num_frames),
        {},
    };

    for (uint32_t frame_idx = 0; frame_idx < num_frames; frame_idx++) {
        char *frame_param_buffer = 
            (char *)param_buffer + total_param_bytes * frame_idx;

        float *output_ptr = state.output + frame_idx * cfg.batchSize *
                cfg.imgHeight * cfg.imgWidth;

        OptixTraversableHandle *accel_ptr =
            (OptixTraversableHandle *)(frame_param_buffer);

        CameraParams *cam_ptr =
            (CameraParams *)(frame_param_buffer + camera_offset);

        ShaderParams stage_params {
            output_ptr,
            accel_ptr,
            cam_ptr,
        };

        cudaMemcpyAsync(state.deviceParams + frame_idx, &stage_params,
                        sizeof(ShaderParams), cudaMemcpyHostToDevice,
                        strm);

        state.hostParams[frame_idx] = stage_params;
    }

    return state;
}

OptixBackend::OptixBackend(const RenderConfig &cfg)
    : batch_size_(cfg.batchSize),
      img_dims_(cfg.imgWidth, cfg.imgHeight),
      cur_frame_(0),
      num_frames_(1), // FIXME
      stream_([]() {
          cudaStream_t strm;
          REQ_CUDA(cudaStreamCreate(&strm));
          return strm;
      }()),
      ctx_(initializeOptix(cfg.gpuID)),
      pipeline_(buildPipeline(ctx_)),
      sbt_(buildSBT(stream_, pipeline_)),
      render_state_(makeRenderState(cfg, stream_, num_frames_))
{
    REQ_CUDA(cudaStreamSynchronize(stream_));
}

LoaderImpl OptixBackend::makeLoader()
{
    OptixLoader *loader = new OptixLoader(ctx_);
    return makeLoaderImpl<OptixLoader>(loader);
}

EnvironmentImpl OptixBackend::makeEnvironment(const shared_ptr<Scene> &)
{
    OptixEnvironment *environment = new OptixEnvironment();
    return makeEnvironmentImpl<OptixEnvironment>(environment);
}

static CameraParams packCamera(const Camera &cam)
{
    CameraParams params;

    glm::vec3 scaled_up = -cam.tanFOV * cam.up;
    glm::vec3 scaled_right = cam.aspectRatio * cam.tanFOV * cam.right;

    params.data[0] = make_float4(cam.position.x, cam.position.y,
                                 cam.position.z, cam.view.x);
    params.data[1] = make_float4(cam.view.y, cam.view.z,
                                 scaled_up.x, scaled_up.y);
    params.data[2] = make_float4(scaled_up.z, scaled_right.x,
                                 scaled_right.y, scaled_right.z);

    return params;
}

void OptixBackend::render(const Environment *envs)
{
    const ShaderParams &host_params = render_state_.hostParams[cur_frame_];

    for (uint32_t batch_idx = 0; batch_idx < batch_size_; batch_idx++) {
        const Environment &env = envs[batch_idx];
        const OptixScene &scene = 
            *static_cast<OptixScene *>(env.getScene().get());

        host_params.accelStructs[batch_idx] = scene.accelStructure;

        host_params.cameras[batch_idx] = packCamera(env.getCamera());
    }

    const ShaderParams &dev_params = render_state_.deviceParams[cur_frame_];

    REQ_OPTIX(optixLaunch(pipeline_.hdl, stream_,
        (CUdeviceptr)&dev_params, sizeof(ShaderParams),
        &sbt_.hdl, img_dims_.x, img_dims_.y, batch_size_));

    REQ_CUDA(cudaStreamSynchronize(stream_));

    cur_frame_ = (cur_frame_ + 1) % num_frames_;
}

}
}
