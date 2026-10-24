#include "vulkan_renderer.hpp"
#include "vulkan_api.hpp"
#include "vulkan_effects.hpp"
#include "yami_vulkan_shaders.hpp"
#include <SDL3/SDL_vulkan.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <functional>
#include <iostream>
#include <limits>
#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace yami {
namespace {
void checked(VkResult r,const char* op) { if(r!=VK_SUCCESS) throw std::runtime_error(std::string(op)+": Vulkan result "+std::to_string(r)+"; select OpenGL in the launcher if this device is unsupported"); }
void sdl(bool r) { if(!r) throw std::runtime_error(SDL_GetError()); }
Mat4 normal_matrix4(const Mat4& m,bool illuminated=true) {
    const auto& v=m.values;
    const float a=v[0],b=v[4],c=v[8],d=v[1],e=v[5],f=v[9],g=v[2],h=v[6],i=v[10];
    Mat4 n=identity_matrix();
    n.values[0]=e*i-f*h;n.values[1]=c*h-b*i;n.values[2]=b*f-c*e;
    n.values[4]=f*g-d*i;n.values[5]=a*i-c*g;n.values[6]=c*d-a*f;
    n.values[8]=d*h-e*g;n.values[9]=b*g-a*h;n.values[10]=a*e-b*d;
    float det=a*n.values[0]+b*n.values[4]+c*n.values[8];
    if(!std::isfinite(det))throw std::runtime_error("Invalid model normal transform");
    if(det==0){if(illuminated)throw std::runtime_error("Singular illuminated model transform");return n;}
    for(unsigned x:{0u,1u,2u,4u,5u,6u,8u,9u,10u}) n.values[x]/=det;
    return n;
}
Mat4 vulkan_projection(Mat4 p) {
    // Original GL clips Z at [-W,W]; Vulkan clips [0,W] and its framebuffer Y is down.
    for(int c=0;c<4;++c) { p.values[c*4+1]=-p.values[c*4+1];p.values[c*4+2]=(p.values[c*4+2]+p.values[c*4+3])*.5f; }
    return p;
}
VkCompareOp compare(unsigned f) {
    if(f<0x200||f>0x207) throw std::runtime_error("Unsupported exported depth function");
    return static_cast<VkCompareOp>(f-0x200);
}
VkBlendFactor blend(unsigned f) {
    switch(f) {
    case 0:return VK_BLEND_FACTOR_ZERO;case 1:return VK_BLEND_FACTOR_ONE;
    case 0x300:return VK_BLEND_FACTOR_SRC_COLOR;case 0x301:return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302:return VK_BLEND_FACTOR_SRC_ALPHA;case 0x303:return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304:return VK_BLEND_FACTOR_DST_ALPHA;case 0x305:return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306:return VK_BLEND_FACTOR_DST_COLOR;case 0x307:return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308:return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:throw std::runtime_error("Unsupported exported blend factor");
    }
}
}

struct VulkanRenderer::Impl {
    VulkanApi vk{};
    SDL_Window* window=nullptr;
    bool loaded=false;
    VkInstance instance=VK_NULL_HANDLE;
    VkSurfaceKHR surface=VK_NULL_HANDLE;
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    VkDevice device=VK_NULL_HANDLE;
    // present_queue: frame generation's present thread copies and presents there. A second queue family with
    // presentation support when one exists (else the graphics queue), so each copy runs and is shown when the thread
    // paces it instead of after the next frame's graphics work queued ahead of it.
    VkQueue queue=VK_NULL_HANDLE,present_queue=VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::uint32_t family=0,present_family=0;
    bool present_timestamps=false; // the present family can reset/write the graphics family's timestamp width
    bool ray=false,anisotropic=false,fsr=false;
    std::uint32_t scratch_alignment=256;
    std::unique_ptr<VulkanContext> context;
    std::unique_ptr<VulkanEffects> effects;
    VkCommandPool pool=VK_NULL_HANDLE;
    VkCommandBuffer command=VK_NULL_HANDLE,upload_command=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    VkSemaphore acquired=VK_NULL_HANDLE;
    VkSwapchainKHR swapchain=VK_NULL_HANDLE;
    VkFormat swap_format=VK_FORMAT_UNDEFINED;
    std::vector<VkImage> swap_images;
    std::vector<VkSemaphore> complete;
    VkDescriptorSetLayout draw_set_layout=VK_NULL_HANDLE,copy_set_layout=VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool=VK_NULL_HANDLE;
    VkPipelineLayout draw_layout=VK_NULL_HANDLE,copy_layout=VK_NULL_HANDLE;
    VkShaderModule raster_vert=VK_NULL_HANDLE,raster_frag=VK_NULL_HANDLE,raster_ui_frag=VK_NULL_HANDLE,final_vert=VK_NULL_HANDLE,final_frag=VK_NULL_HANDLE;
    VkPipeline final_pipeline=VK_NULL_HANDLE,output_pipeline=VK_NULL_HANDLE; // output: ramp into swap_format outputs
    VkFormat output_format=VK_FORMAT_UNDEFINED;
    std::array<VkDescriptorSet,3> copy_sets{}; // world copy, real ramp, generated ramp (one per pass in a frame)
    VkSampler frame_sampler=VK_NULL_HANDLE;
    int width=0,height=0,max_sample=1,sample=1;
    VkSampleCountFlags sample_flags=1;
    float max_aniso=1;
    GraphicsSettings settings{};
    unsigned brightness=257,frame_number=0;
    bool finished=false,world_closed=false,ui=false,world_seen=false,history=false,recreate=true;
    std::size_t world_end=0;
    Mat4 view=identity_matrix(),projection=identity_matrix(),world_view=identity_matrix(),world_projection=identity_matrix(),previous_vp=identity_matrix();
    Viewport viewport{0,0,0,0};
    std::array<float,2> jitter{};
    std::uint64_t next_key=1;
    struct Buffer { VulkanBuffer gpu;void* mapped=nullptr; };
    // uvs: CPU copy of the last uploaded UVs, so animated updates never read mapped GPU memory (shared by copies; only the latest is updated).
    struct Geometry { std::shared_ptr<Buffer> vertices,spare_vertices,indices;std::shared_ptr<std::vector<Vec2>> uvs;std::size_t vertex_count=0,index_count=0;std::uint64_t key=0,revision=0,temporal_key=0,uv_revision=0; };
    struct Texture {
        std::shared_ptr<VulkanImage> storage;
        VulkanImage& image;
        unsigned levels=1;
        std::uint64_t content_revision=0;
        VkSampler sampler=VK_NULL_HANDLE;
        VkDescriptorSet descriptor=VK_NULL_HANDLE;
        bool flip=false,mipmaps=false;
        explicit Texture(std::shared_ptr<VulkanImage> s):storage(std::move(s)),image(*storage){}
    };
    std::vector<std::shared_ptr<Geometry>> meshes;
    std::vector<std::shared_ptr<Texture>> textures;
    std::shared_ptr<Texture> white;
    struct RasterState {
        bool cull=false,offset=false,blend=false,depth_test=false,depth_write=false;
        unsigned cull_face=0x405,source=1,destination=0,depth_function=0x203,mask=15;
        float offset_factor=0,offset_units=0;
    };
    struct Draw {
        std::shared_ptr<Geometry> mesh;std::shared_ptr<Texture> texture;
        RasterState raster;DrawState state;Mat4 view,projection;Viewport viewport;
        unsigned alpha_function=0;float alpha_reference=0;
        bool enhanced=false,world=false;
        std::shared_ptr<Buffer> previous_vertices;
        Mat4 previous_model=identity_matrix();
        std::uint32_t temporal_token=0;
        bool correspondence=false;
    };
    std::vector<Draw> draws;
    struct TemporalDraw {
        std::uint64_t id=0,key=0;
        std::uint32_t token=0;
        std::shared_ptr<Buffer> vertices;
        std::shared_ptr<Texture> texture;
        DrawState state;
        std::uint64_t uv_revision=0,texture_revision=0;
        unsigned alpha_function=0,color_mask=15;
        float alpha_reference=0;
    };
    std::vector<TemporalDraw> previous_draws,next_draws;
    std::uint32_t next_temporal_token=1;
    std::vector<VulkanRayInstance> ray_instances;
    std::vector<VkDescriptorImageInfo> ray_images;
    std::vector<const Texture*> ray_textures;
    using PipelineKey=std::array<unsigned,13>;
    std::map<PipelineKey,VkPipeline> pipelines;
    struct alignas(16) Uniform {
        Mat4 model_view,projection;
        std::array<float,12> normal_eye;
        std::array<float,4> raster_grid;
        Mat4 normal_world;
        std::array<float,4> light,diffuse,color,uv,world_up,flags,alpha_jitter;
        Mat4 previous_mvp,previous_normal_world;
        std::array<std::int32_t,4> temporal;
    };
    static_assert(sizeof(Uniform)==512&&alignof(Uniform)==16);
    static_assert(offsetof(Uniform,model_view)==0&&offsetof(Uniform,projection)==64);
    static_assert(offsetof(Uniform,normal_eye)==128&&offsetof(Uniform,raster_grid)==176);
    static_assert(offsetof(Uniform,normal_world)==192&&offsetof(Uniform,light)==256);
    static_assert(offsetof(Uniform,diffuse)==272&&offsetof(Uniform,color)==288&&offsetof(Uniform,uv)==304);
    static_assert(offsetof(Uniform,world_up)==320&&offsetof(Uniform,flags)==336&&offsetof(Uniform,alpha_jitter)==352);
    static_assert(offsetof(Uniform,previous_mvp)==368&&offsetof(Uniform,previous_normal_world)==432&&offsetof(Uniform,temporal)==496);
    std::shared_ptr<Buffer> uniforms,upload_staging,readback;
    VkDeviceSize uniform_stride=0;
    VulkanImage world_color{},world_depth{},world_normal{},world_ms_color{},world_ms_depth{},world_ms_normal{};
    VulkanImage world_motion{},world_previous_normal{},world_ms_motion{},world_ms_previous_normal{};
    VulkanImage composition{},ui_depth{};
    // Final ramped frames, presented/captured. Without frame generation only outputs[0] exists (slot 0). Outputs and
    // generated_outputs use swap_format, so presenting them is a plain byte copy (a transfer/compute queue cannot blit).
    std::array<VulkanImage,2> outputs{};
    unsigned slot=0;
    VkExtent2D world_extent{};
    // FSR 3.1.4 frame generation (IKV renderer_vk.cpp:108-128): per slot, generated_outputs is the frame
    // interpolated between the previous and this frame's HUD-less composition, outputs this frame; the current
    // HUD is drawn onto both (the scene is interpolated, the HUD never is). While generating, the present thread
    // owns the swapchain: it presents the generated frame, then the real one half a rendered-frame interval later.
    const char* generation_missing=nullptr; // device reason frame generation is unsupported (null: supported)
    const char* generation_logged=nullptr;
    bool generating=false,generated_valid=false;
    VulkanImage generated{};
    std::array<VulkanImage,2> generated_outputs{};
    std::chrono::steady_clock::time_point frame_ready{};
    std::mutex queue_mutex; // vkQueueSubmit2/vkQueuePresentKHR/vkQueueWaitIdle on queue (and present_queue when equal)
    std::mutex present_queue_mutex; // the same on a separate present_queue
    std::mutex& present_lock(){return present_queue==queue?queue_mutex:present_queue_mutex;}
    // id: this job's present_queued number; rendered: the `rendered` value its frame's submission signals.
    struct PresentJob { unsigned slot; bool generated; std::chrono::steady_clock::time_point ready; std::uint64_t id,rendered; };
    std::thread presenter;
    std::mutex present_mutex;
    std::condition_variable present_cv;
    std::deque<PresentJob> present_jobs;
    std::uint64_t present_queued=0,present_done=0;
    std::exception_ptr present_error;
    std::atomic<bool> present_stop{false},present_stale{false}; // stale: the present thread saw an out-of-date swapchain
    VkCommandPool present_pool=VK_NULL_HANDLE;
    std::array<VkCommandBuffer,2> present_commands{}; // [0] generated, [1] real
    std::array<VkFence,2> present_fences{};
    std::array<VkSemaphore,2> present_acquired{};
    // Timeline semaphores between the queues: generating frame submissions signal `rendered` (rendered_value), the
    // present copies wait for it; job N's last copy signals `copied` = N and the next frame rendering into that
    // slot (slot_job) waits for it. copied_value: the present thread's last submitted `copied` signal.
    VkSemaphore rendered=VK_NULL_HANDLE,copied=VK_NULL_HANDLE;
    std::uint64_t rendered_value=0,copied_value=0;
    std::array<std::uint64_t,2> slot_job{};
    bool present_timed=false;
    // --gpu-timings: GPU ms of every frame that ran each stage (GpuWorld..GpuPresent; present counts each blit)
    // and per-world-frame counter sums. Null: off or unsupported. present_pending: present_commands[i] unread.
    // cpu: wall ms from clear() to the frame's submit; interval: wall ms between consecutive real-frame presents
    // (written by whichever thread owns presentation; the drained hand-off orders them).
    struct GpuTimings {
        std::uint64_t mask=0;double ns_per_tick=1;
        std::array<std::vector<float>,GpuPresent+1> ms;
        std::vector<float> cpu,interval;
        std::chrono::steady_clock::time_point begin{},presented{};
        std::uint64_t frames=0,world_frames=0;
        double blas_builds=0,blas_updates=0,tlas_builds=0,tlas_updates=0,instances=0,width=0,height=0,samples=0;
        std::array<bool,2> present_pending{};
    };
    std::unique_ptr<GpuTimings> timings;

    explicit Impl(const DisplayOptions& options):settings(options) {
        if(options.width<=0||options.height<=0||!valid_graphics(options)) throw std::runtime_error("Invalid display options");
        try { initialize(options); } catch(...) { cleanup();throw; }
    }
    ~Impl() { cleanup(); }
    void initialize(const DisplayOptions& options) {
        sdl(SDL_Vulkan_LoadLibrary(nullptr));loaded=true;
        vk.vkGetInstanceProcAddr=reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        if(!vk.vkGetInstanceProcAddr) throw std::runtime_error("SDL Vulkan loader has no vkGetInstanceProcAddr");
#define LOAD_GLOBAL(name) vk.name=reinterpret_cast<PFN_##name>(vk.vkGetInstanceProcAddr(VK_NULL_HANDLE,#name));if(!vk.name)throw std::runtime_error("Missing Vulkan loader function " #name);
        YAMI_VK_GLOBAL(LOAD_GLOBAL)
#undef LOAD_GLOBAL
        std::uint32_t version=0;checked(vk.vkEnumerateInstanceVersion(&version),"Vulkan loader version");
        if(version<VK_API_VERSION_1_3) throw std::runtime_error("Vulkan 1.3 is required; choose OpenGL for older drivers");
        window=SDL_CreateWindow("ETI Yami — Vulkan",options.width,options.height,SDL_WINDOW_VULKAN|SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY|(options.fullscreen?SDL_WINDOW_FULLSCREEN:0));
        if(!window) throw std::runtime_error(SDL_GetError());
        std::uint32_t extension_count=0;const char* const* extensions=SDL_Vulkan_GetInstanceExtensions(&extension_count);
        if(!extensions) throw std::runtime_error(SDL_GetError());
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="ETI Yami";app.apiVersion=VK_API_VERSION_1_3;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;ci.enabledExtensionCount=extension_count;ci.ppEnabledExtensionNames=extensions;
        checked(vk.vkCreateInstance(&ci,nullptr,&instance),"Create Vulkan instance");
#define LOAD_INSTANCE(name) vk.name=reinterpret_cast<PFN_##name>(vk.vkGetInstanceProcAddr(instance,#name));if(!vk.name)throw std::runtime_error("Missing Vulkan instance function " #name);
        YAMI_VK_INSTANCE(LOAD_INSTANCE)
#undef LOAD_INSTANCE
        sdl(SDL_Vulkan_CreateSurface(window,instance,nullptr,&surface));
        choose_device();
#define LOAD_DEVICE(name) vk.name=reinterpret_cast<PFN_##name>(vk.vkGetDeviceProcAddr(device,#name));if(!vk.name)throw std::runtime_error("Missing Vulkan device function " #name);
        YAMI_VK_DEVICE(LOAD_DEVICE)
        if(ray) { YAMI_VK_RAY(LOAD_DEVICE) }
        if(VulkanRenderer::gpu_timings) { YAMI_VK_QUERY(LOAD_DEVICE) }
#undef LOAD_DEVICE
        vk.vkGetDeviceQueue(device,family,0,&queue);vk.vkGetDeviceQueue(device,present_family,0,&present_queue);
        vk.vkGetPhysicalDeviceMemoryProperties(physical,&memory);
        context=std::make_unique<VulkanContext>(VulkanContext{vk,physical,device,queue,family,ray,properties,memory,scratch_alignment,instance,fsr});
        if(VulkanRenderer::gpu_timings)create_timings();
        present_timed=timings&&(present_family==family||present_timestamps);
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pc.queueFamilyIndex=family;
        checked(vk.vkCreateCommandPool(device,&pc,nullptr,&pool),"Create command pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=2;
        VkCommandBuffer cmds[2];checked(vk.vkAllocateCommandBuffers(device,&ca,cmds),"Allocate command buffers");command=cmds[0];upload_command=cmds[1];
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fc.flags=VK_FENCE_CREATE_SIGNALED_BIT;checked(vk.vkCreateFence(device,&fc,nullptr,&fence),"Create fence");
        VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};checked(vk.vkCreateSemaphore(device,&sc,nullptr,&acquired),"Create acquire semaphore");
        create_layouts();
        raster_vert=shader(spirv::vulkan_raster_vert);raster_frag=shader(spirv::vulkan_raster_frag);raster_ui_frag=shader(spirv::vulkan_raster_ui_frag);final_vert=shader(spirv::vulkan_final_vert);final_frag=shader(spirv::vulkan_final_frag);
        final_pipeline=fullscreen_pipeline(VK_FORMAT_R8G8B8A8_UNORM);
        frame_sampler=sampler(false,true);
        uniform_stride=(sizeof(Uniform)+properties.limits.minUniformBufferOffsetAlignment-1)&~(properties.limits.minUniformBufferOffsetAlignment-1);
        uniforms=buffer(uniform_stride,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,false);
        const std::array<std::uint8_t,4> pixels{{255,255,255,255}};white=make_texture(1,1,pixels,false,false);
        effects=std::make_unique<VulkanEffects>(*context);
        if(!context->fsr&&!generation_missing)generation_missing="FSR 3.1 unavailable";
        update_generation();
        resize();clear();present(); // Commit a real framebuffer before Wayland input-focus gating.
        std::cerr<<"Vulkan device: "<<properties.deviceName<<"; API "<<VK_VERSION_MAJOR(properties.apiVersion)<<'.'<<VK_VERSION_MINOR(properties.apiVersion)<<"; ray query "<<(ray?"available":"unavailable")<<"; FSR 3.1 "<<(context->fsr?"available":"unavailable")<<"; FSR 3.1.4 frame generation "<<(generation_missing?"unavailable (":"available")<<(generation_missing?generation_missing:"")<<(generation_missing?")":"")<<'\n';
    }
    void choose_device() {
        std::uint32_t count=0;checked(vk.vkEnumeratePhysicalDevices(instance,&count,nullptr),"Enumerate GPUs");std::vector<VkPhysicalDevice> devices(count);checked(vk.vkEnumeratePhysicalDevices(instance,&count,devices.data()),"Enumerate GPUs");
        int best=-1;
        for(auto candidate:devices) {
            VkPhysicalDeviceProperties props;vk.vkGetPhysicalDeviceProperties(candidate,&props);if(props.apiVersion<VK_API_VERSION_1_3)continue;
            std::uint32_t qc=0;vk.vkGetPhysicalDeviceQueueFamilyProperties(candidate,&qc,nullptr);std::vector<VkQueueFamilyProperties> families(qc);vk.vkGetPhysicalDeviceQueueFamilyProperties(candidate,&qc,families.data());
            for(std::uint32_t q=0;q<qc;++q) { VkBool32 supported=false;checked(vk.vkGetPhysicalDeviceSurfaceSupportKHR(candidate,q,surface,&supported),"Query presentation");
                if(!supported||!(families[q].queueFlags&VK_QUEUE_GRAPHICS_BIT)||!(families[q].queueFlags&VK_QUEUE_COMPUTE_BIT))continue;
                VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f.pNext=&f13;vk.vkGetPhysicalDeviceFeatures2(candidate,&f);
                if(!f13.dynamicRendering||!f13.synchronization2||!f13.shaderDemoteToHelperInvocation||!f.features.independentBlend)continue;
                int score=props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU?2:1;if(score>best){best=score;physical=candidate;family=q;properties=props;}
            }
        }
        if(!physical)throw std::runtime_error("No Vulkan 1.3 graphics/presentation device with dynamic rendering, synchronization2 and independent blend; select OpenGL");
        std::uint32_t ec=0;checked(vk.vkEnumerateDeviceExtensionProperties(physical,nullptr,&ec,nullptr),"Enumerate device extensions");std::vector<VkExtensionProperties> ext(ec);checked(vk.vkEnumerateDeviceExtensionProperties(physical,nullptr,&ec,ext.data()),"Enumerate device extensions");
        const auto has=[&](const char* n){return std::any_of(ext.begin(),ext.end(),[&](const auto& e){return std::strcmp(e.extensionName,n)==0;});};
        if(!has(VK_KHR_SWAPCHAIN_EXTENSION_NAME))throw std::runtime_error("GPU lacks Vulkan swapchain support");
        VkPhysicalDeviceAccelerationStructureFeaturesKHR fas{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};VkPhysicalDeviceRayQueryFeaturesKHR fray{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f.pNext=&f13;f13.pNext=&f12;f12.pNext=&fas;fas.pNext=&fray;vk.vkGetPhysicalDeviceFeatures2(physical,&f);
        // MSAA ray effects add eight frame samplers and thirteen fixed resources (two storage buffers queue
        // history-unsafe pixels from the fragment pass: fragmentStoresAndAtomics).
        ray=has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)&&has(VK_KHR_RAY_QUERY_EXTENSION_NAME)&&has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)&&fas.accelerationStructure&&fray.rayQuery&&f.features.shaderInt64&&f.features.fragmentStoresAndAtomics&&f12.bufferDeviceAddress&&f12.runtimeDescriptorArray&&f12.shaderSampledImageArrayNonUniformIndexing&&f12.descriptorBindingPartiallyBound&&properties.limits.maxPerStageDescriptorSamplers>=8&&properties.limits.maxDescriptorSetSamplers>=8&&properties.limits.maxPerStageDescriptorSampledImages>=8&&properties.limits.maxDescriptorSetSampledImages>=8&&properties.limits.maxPerStageResources>=13;
        VkPhysicalDeviceAccelerationStructurePropertiesKHR ap{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};VkPhysicalDeviceDepthStencilResolveProperties dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};VkPhysicalDeviceSubgroupProperties sp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p.pNext=&sp;sp.pNext=&dp;if(ray)dp.pNext=&ap;vk.vkGetPhysicalDeviceProperties2(physical,&p);if(ray)scratch_alignment=ap.minAccelerationStructureScratchOffsetAlignment;
        if(!(dp.supportedDepthResolveModes&VK_RESOLVE_MODE_SAMPLE_ZERO_BIT))throw std::runtime_error("Vulkan device lacks sample-zero depth resolve");
        anisotropic=f.features.samplerAnisotropy;max_aniso=anisotropic?properties.limits.maxSamplerAnisotropy:1;
        VkPhysicalDeviceVulkan13Features enable13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};enable13.dynamicRendering=enable13.synchronization2=enable13.shaderDemoteToHelperInvocation=true;
        VkPhysicalDeviceVulkan12Features enable12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};enable13.pNext=&enable12;
        const auto storage=[&](VkFormat format){VkFormatProperties flags{};vk.vkGetPhysicalDeviceFormatProperties(physical,format,&flags);return (flags.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)!=0;};
        // FSR blobs (FP32/wave32: yami_ffx_clamp_capabilities) write formatless storage images and use compute quad subgroup operations.
        fsr=f.features.shaderStorageImageWriteWithoutFormat&&(sp.supportedStages&VK_SHADER_STAGE_COMPUTE_BIT)&&(sp.supportedOperations&VK_SUBGROUP_FEATURE_QUAD_BIT)&&storage(VK_FORMAT_R16G16B16A16_SFLOAT)&&storage(VK_FORMAT_R16G16_SFLOAT)&&storage(VK_FORMAT_R32_SFLOAT)&&storage(VK_FORMAT_R32_UINT);
        // Frame generation (IKV fsr_vk.cpp:126-143): optical flow reduces with subgroup add/min, interpolation reads
        // formatless storage images, and the RGBA8 display images are interpolation source/target. Timeline
        // semaphores order the present queue against the graphics queue.
        VkFormatProperties display{};vk.vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R8G8B8A8_UNORM,&display);
        constexpr VkFormatFeatureFlags display_needed=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        generation_missing=!fsr?"FSR 3.1 unsupported":!(sp.supportedOperations&VK_SUBGROUP_FEATURE_ARITHMETIC_BIT)?"compute subgroup arithmetic unavailable":
            !f.features.shaderStorageImageReadWithoutFormat?"shaderStorageImageReadWithoutFormat unavailable":
            (display.optimalTilingFeatures&display_needed)!=display_needed?"RGBA8 display format lacks storage/sampled/attachment/transfer support":
            !f12.timelineSemaphore?"timeline semaphores unavailable":nullptr;
        // Present queue (AMD's FI swapchain: "UI composition and presents will always get executed on an async queue,
        // so they can be paced and injected into the middle of the workloads generating the next frame"): another
        // family that can present to this surface, compute/graphics (timestamps, usually not a slow DMA engine)
        // before transfer-only. None: the graphics queue, which serialises each copy behind the next frame.
        present_family=family;
        if(!generation_missing){
            std::uint32_t qc=0;vk.vkGetPhysicalDeviceQueueFamilyProperties(physical,&qc,nullptr);std::vector<VkQueueFamilyProperties> families(qc);vk.vkGetPhysicalDeviceQueueFamilyProperties(physical,&qc,families.data());
            int present_score=0;
            for(std::uint32_t q=0;q<qc;++q){
                VkBool32 supported=false;checked(vk.vkGetPhysicalDeviceSurfaceSupportKHR(physical,q,surface,&supported),"Query presentation");
                const auto flags=families[q].queueFlags;
                const int score=q==family||!supported||!families[q].queueCount?0:flags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)?2:flags&VK_QUEUE_TRANSFER_BIT?1:0;
                if(score>present_score){present_score=score;present_family=q;}
            }
            // vkCmdResetQueryPool needs a graphics/compute queue; one timestamp mask serves both families.
            present_timestamps=present_score==2&&families[present_family].timestampValidBits==families[family].timestampValidBits;
        }
        enable12.timelineSemaphore=f12.timelineSemaphore;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR enable_as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};VkPhysicalDeviceRayQueryFeaturesKHR enable_ray{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        if(ray){enable12.bufferDeviceAddress=enable12.runtimeDescriptorArray=enable12.shaderSampledImageArrayNonUniformIndexing=enable12.descriptorBindingPartiallyBound=true;enable12.pNext=&enable_as;enable_as.accelerationStructure=true;enable_as.pNext=&enable_ray;enable_ray.rayQuery=true;extensions.insert(extensions.end(),{VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,VK_KHR_RAY_QUERY_EXTENSION_NAME,VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME});}
        float priority=1;std::array<VkDeviceQueueCreateInfo,2> qi{};
        for(unsigned i=0;i<2;++i){qi[i].sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;qi[i].queueFamilyIndex=i?present_family:family;qi[i].queueCount=1;qi[i].pQueuePriorities=&priority;}
        VkPhysicalDeviceFeatures base{};base.samplerAnisotropy=anisotropic;base.independentBlend=true;base.shaderInt64=ray;base.fragmentStoresAndAtomics=ray;base.shaderStorageImageReadWithoutFormat=fsr&&f.features.shaderStorageImageReadWithoutFormat;base.shaderStorageImageWriteWithoutFormat=fsr;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.pNext=&enable13;dc.queueCreateInfoCount=present_family==family?1:2;dc.pQueueCreateInfos=qi.data();dc.enabledExtensionCount=extensions.size();dc.ppEnabledExtensionNames=extensions.data();dc.pEnabledFeatures=&base;checked(vk.vkCreateDevice(physical,&dc,nullptr,&device),"Create Vulkan device");
        sample_flags=properties.limits.framebufferColorSampleCounts&properties.limits.framebufferDepthSampleCounts&properties.limits.sampledImageIntegerSampleCounts&properties.limits.sampledImageColorSampleCounts;
        for(const auto format:{VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16G16B16A16_SINT,VK_FORMAT_R32G32B32A32_SINT,VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_D32_SFLOAT}) {
            VkImageFormatProperties support{};
            const bool color=format!=VK_FORMAT_D32_SFLOAT;
            const VkImageUsageFlags usage=(color?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) |
                (color?VK_IMAGE_USAGE_SAMPLED_BIT:0);
            if(color) {
                VkFormatProperties flags{}; vk.vkGetPhysicalDeviceFormatProperties(physical,format,&flags);
                const auto required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
                if((flags.optimalTilingFeatures&required)!=required) throw std::runtime_error("Vulkan world format lacks sampled color-attachment support");
            }
            checked(vk.vkGetPhysicalDeviceImageFormatProperties(physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,usage,0,&support),"Query framebuffer sample support");
            sample_flags&=support.sampleCounts;
        }
        for(int n=64;n>=1;n/=2)if(sample_flags&unsigned(n)){max_sample=n;break;}
        settings.anisotropy=std::min(settings.anisotropy,max_aniso);select_samples();
    }
    void select_samples(){sample=1;for(int n=64;n>=2;n/=2)if(n<=settings.samples&&(sample_flags&unsigned(n))){sample=n;break;}}
    unsigned memory_type(unsigned bits,VkMemoryPropertyFlags flags) {
        for(unsigned i=0;i<memory.memoryTypeCount;++i)if((bits&(1u<<i))&&(memory.memoryTypes[i].propertyFlags&flags)==flags)return i;
        throw std::runtime_error("No compatible Vulkan memory type");
    }
    void wait(){if(device&&fence)checked(vk.vkWaitForFences(device,1,&fence,true,UINT64_MAX),"Wait frame fence");}
    void create_timings(){
        std::uint32_t count=0;vk.vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);vk.vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
        const auto bits=families[family].timestampValidBits;const float period=properties.limits.timestampPeriod;
        if(!bits||!(period>0)){std::cerr<<"Vulkan GPU timings unavailable: queue family has no timestamp support\n";return;}
        VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};ci.queryType=VK_QUERY_TYPE_TIMESTAMP;ci.queryCount=2*GpuQueryStages;
        checked(vk.vkCreateQueryPool(device,&ci,nullptr,&context->timings),"Create GPU timestamp pool");
        timings=std::make_unique<GpuTimings>();timings->mask=bits>=64?~0ull:(1ull<<bits)-1;timings->ns_per_tick=period;
        for(auto& v:timings->ms)v.reserve(1u<<16); // ~18 min at 60 fps before any growth
        timings->cpu.reserve(1u<<16);timings->interval.reserve(1u<<16);
    }
    // Appends each stage in [first,first+count) whose start and end timestamps were both written; target overrides the slot.
    void read_timings(std::uint32_t first,std::uint32_t count,std::uint32_t target=GpuQueryStages){
        std::array<std::uint64_t,4*(GpuFrame+1)> q{}; // per query: value, availability
        const VkResult r=vk.vkGetQueryPoolResults(device,context->timings,2*first,2*count,4*count*sizeof(std::uint64_t),q.data(),2*sizeof(std::uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if(r!=VK_NOT_READY)checked(r,"Read GPU timestamps");
        for(std::uint32_t s=0;s<count;++s)if(q[4*s+1]&&q[4*s+3])
            timings->ms[target<GpuQueryStages?target:first+s].push_back(float(double((q[4*s+2]-q[4*s])&timings->mask)*timings->ns_per_tick*1e-6));
    }
    void record_frame_timings(){
        read_timings(0,GpuFrame+1);auto& t=*timings;auto& c=*context;++t.frames;
        if(world_seen){++t.world_frames;t.blas_builds+=c.blas_builds;t.blas_updates+=c.blas_updates;t.tlas_builds+=c.tlas_builds;t.tlas_updates+=c.tlas_updates;
            t.instances+=settings.enhanced?ray_instances.size():0;t.width+=world_extent.width;t.height+=world_extent.height;t.samples+=sample;}
        c.blas_builds=c.blas_updates=c.tlas_builds=c.tlas_updates=0;
    }
    static float elapsed_ms(std::chrono::steady_clock::time_point from,std::chrono::steady_clock::time_point to){return std::chrono::duration<float,std::milli>(to-from).count();}
    void record_present(){auto& t=*timings;const auto now=std::chrono::steady_clock::now();if(t.presented!=std::chrono::steady_clock::time_point{})t.interval.push_back(elapsed_ms(t.presented,now));t.presented=now;}
    void print_timings()noexcept{
        auto& t=*timings;
        static constexpr const char* names[]{"world","accel","classify","effects","sun","temporal","fsr-inputs","fsr","display","composite","framegen","ui","frame","present"};
        static_assert(std::size(names)==GpuPresent+1);
        std::fprintf(stderr,"Vulkan GPU timings: %llu frames (%llu with world), timestamp period %.3f ns\n  %-10s %7s %8s %8s %8s\n",
            static_cast<unsigned long long>(t.frames),static_cast<unsigned long long>(t.world_frames),t.ns_per_tick,"stage (ms)","count","mean","median","p95");
        const auto row=[](const char* name,std::vector<float>& v){
            const std::size_t n=v.size();
            if(!n){std::fprintf(stderr,"  %-10s %7zu %8s %8s %8s\n",name,n,"-","-","-");return;}
            std::sort(v.begin(),v.end());double sum=0;for(const float x:v)sum+=x;
            const double median=n%2?v[n/2]:(double(v[n/2-1])+v[n/2])*.5;
            std::fprintf(stderr,"  %-10s %7zu %8.3f %8.3f %8.3f\n",name,n,sum/n,median,double(v[(n*95+99)/100-1]));
        };
        for(std::size_t s=0;s<t.ms.size();++s)row(names[s],t.ms[s]);
        row("cpu",t.cpu);row("interval",t.interval);
        const double w=double(std::max<std::uint64_t>(t.world_frames,1));
        std::fprintf(stderr,"  per world frame: BLAS builds %.2f, BLAS updates %.2f, TLAS builds %.2f, TLAS updates %.2f, instances %.1f, render extent %.0fx%.0f, MSAA %.2fx\n",
            t.blas_builds/w,t.blas_updates/w,t.tlas_builds/w,t.tlas_updates/w,t.instances/w,t.width/w,t.height/w,t.samples/w);
    }
    // Frame generation needs Vulkan enhanced graphics, FSR 3.1 on and available, and device support; any other
    // request is logged (once per reason) and left inactive. Changing it hands the swapchain between threads.
    void update_generation(){
        const char* missing=!settings.frame_generation?nullptr:!settings.enhanced?"enhanced graphics off":
            !settings.temporal_aa?"FSR 3.1 off":generation_missing;
        if(missing&&missing!=generation_logged)std::cerr<<"Vulkan FSR 3.1.4 frame generation inactive: "<<missing<<'\n';
        generation_logged=missing;
        const bool active=settings.frame_generation&&!missing;
        if(active==generating)return;
        destroy_generation_targets();wait();
        generating=active;generated_valid=false;slot=0;
        if(active)start_presenter();
        std::cerr<<"Vulkan FSR 3.1.4 frame generation "<<(active?"on":"off")<<'\n';
    }
    // Wait until at most `outstanding` present jobs remain, then rethrow a present-thread failure.
    void drain_presents(std::uint64_t outstanding=0){
        std::unique_lock lock(present_mutex);
        present_cv.wait(lock,[&]{return present_queued-present_done<=outstanding;});
        if(present_error)std::rethrow_exception(std::exchange(present_error,nullptr));
    }
    void start_presenter(){
        if(presenter.joinable())return;
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pc.queueFamilyIndex=present_family;
        checked(vk.vkCreateCommandPool(device,&pc,nullptr,&present_pool),"Create present command pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=present_pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=2;
        checked(vk.vkAllocateCommandBuffers(device,&ca,present_commands.data()),"Allocate present commands");
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fc.flags=VK_FENCE_CREATE_SIGNALED_BIT;VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for(unsigned i=0;i<2;++i){checked(vk.vkCreateFence(device,&fc,nullptr,&present_fences[i]),"Create present fence");checked(vk.vkCreateSemaphore(device,&sc,nullptr,&present_acquired[i]),"Create present acquire semaphore");}
        VkSemaphoreTypeCreateInfo timeline{};timeline.sType=VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;timeline.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;sc.pNext=&timeline;
        checked(vk.vkCreateSemaphore(device,&sc,nullptr,&rendered),"Create rendered timeline");checked(vk.vkCreateSemaphore(device,&sc,nullptr,&copied),"Create copied timeline");
        presenter=std::thread([this]{present_loop();});
    }
    // Present thread (IKV renderer_vk.cpp:190-236): acquire, copy the finished source into the swapchain image and
    // present, on present_queue. Out-of-date/suboptimal marks the swapchain stale for the render thread to rebuild.
    // The real (last) copy of a job signals `copied` = job.id.
    void present_image(unsigned index,const VulkanImage& source,const PresentJob& job){
        auto& done=present_fences[index];const auto cmd=present_commands[index];
        checked(vk.vkWaitForFences(device,1,&done,true,UINT64_MAX),"Wait present copy");
        if(timings&&std::exchange(timings->present_pending[index],false))read_timings(GpuPresentGenerated+index,1,GpuPresent);
        std::uint32_t image=0;VkResult result;
        // Bounded waits let shutdown abandon an acquire a hidden window never satisfies.
        do result=vk.vkAcquireNextImageKHR(device,swapchain,50'000'000,present_acquired[index],VK_NULL_HANDLE,&image);
        while((result==VK_TIMEOUT||result==VK_NOT_READY)&&!present_stop);
        if(result==VK_TIMEOUT||result==VK_NOT_READY)return;
        if(result==VK_ERROR_OUT_OF_DATE_KHR){present_stale=true;return;}
        if(result==VK_SUBOPTIMAL_KHR)present_stale=true;else checked(result,"Acquire swapchain image");
        begin(cmd);
        const auto timed=GpuStage(GpuPresentGenerated+index);if(present_timed){vk.vkCmdResetQueryPool(cmd,context->timings,2*timed,2);gpu_timestamp(*context,cmd,timed,false);}
        // Same format and extent (outputs are swap_format): a byte copy, which transfer/compute queues support.
        VulkanImage swap{};swap.image=swap_images[image];swap.format=swap_format;swap.extent=source.extent;transition(cmd,swap,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.extent={source.extent.width,source.extent.height,1};
        vk.vkCmdCopyImage(cmd,source.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,swap.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        transition(cmd,swap,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);if(present_timed)gpu_timestamp(*context,cmd,timed,true);checked(vk.vkEndCommandBuffer(cmd),"End present copy");
        checked(vk.vkResetFences(device,1,&done),"Reset present fence");
        VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=cmd;
        // The source was written by the frame submission that signalled `rendered` = job.rendered (a full memory
        // dependency across queues; the render thread also waited for it on the host before queueing the job).
        std::array<VkSemaphoreSubmitInfo,2> waits{};for(auto& w:waits){w.sType=VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;w.stageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;}
        waits[0].semaphore=present_acquired[index];waits[1].semaphore=rendered;waits[1].value=job.rendered;
        std::array<VkSemaphoreSubmitInfo,2> signals{};for(auto& s:signals){s.sType=VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;s.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;}
        signals[0].semaphore=complete[image];signals[1].semaphore=copied;signals[1].value=job.id;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};si.waitSemaphoreInfoCount=2;si.pWaitSemaphoreInfos=waits.data();si.commandBufferInfoCount=1;si.pCommandBufferInfos=&cb;si.signalSemaphoreInfoCount=index==1?2:1;si.pSignalSemaphoreInfos=signals.data();
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&complete[image];pi.swapchainCount=1;pi.pSwapchains=&swapchain;pi.pImageIndices=&image;
        {
            std::scoped_lock lock(present_lock());
            checked(vk.vkQueueSubmit2(present_queue,1,&si,done),"Submit present copy");
            if(index==1)copied_value=job.id;
            result=vk.vkQueuePresentKHR(present_queue,&pi);
        }
        if(present_timed)timings->present_pending[index]=true;
        if(result==VK_ERROR_OUT_OF_DATE_KHR||result==VK_SUBOPTIMAL_KHR)present_stale=true;else checked(result,"Present Vulkan frame");
        if(timings&&index==1&&result!=VK_ERROR_OUT_OF_DATE_KHR)record_present();
    }
    // Present thread (after IKV renderer_vk.cpp:242-323): the generated frame is presented as soon as its frame is
    // ready, the real frame exactly half the measured rendered-frame interval (0.9/0.1 EMA) later. IKV subtracts a
    // 2 ms lead from that delay; at IKV's ~82 rendered fps a 165 Hz FIFO swapchain absorbs it, but below the refresh
    // cap a variable-refresh display shows each image when presented, so the lead split every rendered interval
    // ~40/60 between the generated and the real frame (7.3/11.3 ms at 54 fps) while their content advances by
    // equal halves: visible judder, most obvious on high-contrast moving shadows. FIFO still orders the images.
    void present_loop()noexcept{
        using clock=std::chrono::steady_clock;
        clock::time_point last_ready{};double interval_ms=0;
        for(;;){
            PresentJob job{};
            {
                std::unique_lock lock(present_mutex);
                present_cv.wait(lock,[&]{return present_stop||!present_jobs.empty();});
                if(present_jobs.empty())return;
                job=present_jobs.front();
            }
            try{
                // Ready: the frame finished and this thread is free to show it.
                const auto ready=std::max(job.ready,clock::now());
                const double period=last_ready==clock::time_point{}?0:std::chrono::duration<double,std::milli>(ready-last_ready).count();
                last_ready=ready;
                if(period>0&&period<250)interval_ms=interval_ms>0?interval_ms*.9+period*.1:period;
                if(job.generated){
                    present_image(0,generated_outputs[job.slot],job);
                    const auto target=ready+std::chrono::duration_cast<clock::duration>(std::chrono::duration<double,std::milli>(interval_ms*.5));
                    for(;;){
                        const auto remaining=target-clock::now();
                        if(remaining<=clock::duration::zero())break;
                        SDL_DelayPrecise(static_cast<Uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count()));
                    }
                }
                present_image(1,outputs[job.slot],job);
            }catch(...){
                std::scoped_lock lock(present_mutex);
                if(!present_error)present_error=std::current_exception();
            }
            // The frame that next renders into this slot waits for `copied` = job.id: signal it after this job's
            // copies (queue order) even when the real copy was skipped (out-of-date, shutdown) or failed.
            if(copied_value<job.id)try{
                VkSemaphoreSubmitInfo signal{};signal.sType=VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;signal.semaphore=copied;signal.value=job.id;signal.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                VkSubmitInfo2 si{};si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO_2;si.signalSemaphoreInfoCount=1;si.pSignalSemaphoreInfos=&signal;
                std::scoped_lock lock(present_lock());
                checked(vk.vkQueueSubmit2(present_queue,1,&si,VK_NULL_HANDLE),"Signal skipped present copy");copied_value=job.id;
            }catch(...){
                std::scoped_lock lock(present_mutex);
                if(!present_error)present_error=std::current_exception();
            }
            {std::scoped_lock lock(present_mutex);present_jobs.pop_front();++present_done;}
            present_cv.notify_all();
        }
    }
    // read: the CPU reads the mapping (prefer cached system RAM); otherwise the CPU only writes it (prefer mappable VRAM).
    std::shared_ptr<Buffer> buffer(VkDeviceSize bytes,VkBufferUsageFlags usage,bool address,bool read=false) {
        auto result=std::shared_ptr<Buffer>(new Buffer,[this](Buffer* b){if(b->mapped)vk.vkUnmapMemory(device,b->gpu.memory);if(b->gpu.buffer)vk.vkDestroyBuffer(device,b->gpu.buffer,nullptr);if(b->gpu.memory)vk.vkFreeMemory(device,b->gpu.memory,nullptr);delete b;});
        result->gpu.size=bytes;VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=bytes;bi.usage=usage|(address?VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT:0);bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;checked(vk.vkCreateBuffer(device,&bi,nullptr,&result->gpu.buffer),"Create buffer");
        VkMemoryRequirements mr;vk.vkGetBufferMemoryRequirements(device,result->gpu.buffer,&mr);VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};flags.flags=address?VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT:0;
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.pNext=address?&flags:nullptr;ma.allocationSize=mr.size;checked(allocate_host_memory(*context,ma,mr.memoryTypeBits,read?host_readback_memory:host_upload_memory,host_memory,result->gpu.memory),"Allocate buffer memory");checked(vk.vkBindBufferMemory(device,result->gpu.buffer,result->gpu.memory,0),"Bind buffer");checked(vk.vkMapMemory(device,result->gpu.memory,0,bytes,0,&result->mapped),"Map buffer");
        if(address){VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};ai.buffer=result->gpu.buffer;result->gpu.address=vk.vkGetBufferDeviceAddress(device,&ai);}
        return result;
    }
    // presented: also read by present_queue's copies (concurrent sharing between the two families, no ownership transfers).
    VulkanImage image(VkExtent2D extent,VkFormat format,VkImageUsageFlags usage,unsigned samples=1,unsigned levels=1,bool presented=false) {
        VulkanImage result{};result.extent=extent;result.format=format;const std::array<std::uint32_t,2> families{family,present_family};
        try { VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={extent.width,extent.height,1};ci.mipLevels=levels;ci.arrayLayers=1;ci.samples=static_cast<VkSampleCountFlagBits>(samples);ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            if(presented&&present_family!=family){ci.sharingMode=VK_SHARING_MODE_CONCURRENT;ci.queueFamilyIndexCount=2;ci.pQueueFamilyIndices=families.data();}
            checked(vk.vkCreateImage(device,&ci,nullptr,&result.image),"Create image");
            VkMemoryRequirements mr;vk.vkGetImageMemoryRequirements(device,result.image,&mr);VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);checked(vk.vkAllocateMemory(device,&ma,nullptr,&result.memory),"Allocate image");checked(vk.vkBindImageMemory(device,result.image,result.memory,0),"Bind image");
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=result.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange={format==VK_FORMAT_D32_SFLOAT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,levels,0,1};checked(vk.vkCreateImageView(device,&vi,nullptr,&result.view),"Create image view");
        }catch(...){destroy_image(result);throw;}return result;
    }
    void destroy_image(VulkanImage& i)noexcept{if(i.view)vk.vkDestroyImageView(device,i.view,nullptr);if(i.image)vk.vkDestroyImage(device,i.image,nullptr);if(i.memory)vk.vkFreeMemory(device,i.memory,nullptr);i={};}
    void transition(VkCommandBuffer cmd,VulkanImage& image,VkImageLayout layout,unsigned levels=1) {
        if(image.layout==layout)return;
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};barrier.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;barrier.srcAccessMask=image.layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;barrier.oldLayout=image.layout;barrier.newLayout=layout;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=image.image;barrier.subresourceRange={image.format==VK_FORMAT_D32_SFLOAT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,levels,0,1};VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.imageMemoryBarrierCount=1;dep.pImageMemoryBarriers=&barrier;vk.vkCmdPipelineBarrier2(cmd,&dep);image.layout=layout;
    }
    void begin(VkCommandBuffer cmd){checked(vk.vkResetCommandBuffer(cmd,0),"Reset command buffer");VkCommandBufferBeginInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};ci.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;checked(vk.vkBeginCommandBuffer(cmd,&ci),"Begin commands");}
    void submit_upload(){checked(vk.vkEndCommandBuffer(upload_command),"End upload");VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=upload_command;VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};si.commandBufferInfoCount=1;si.pCommandBufferInfos=&cb;std::scoped_lock lock(queue_mutex);checked(vk.vkQueueSubmit2(queue,1,&si,VK_NULL_HANDLE),"Submit upload");checked(vk.vkQueueWaitIdle(queue),"Wait upload");}
    template<std::size_t N> VkShaderModule shader(const std::uint32_t(&code)[N]){VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=sizeof(code);ci.pCode=code;VkShaderModule m;checked(vk.vkCreateShaderModule(device,&ci,nullptr,&m),"Create shader");return m;}
    VkSampler sampler(bool mipmaps,bool clamp=false){VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};ci.magFilter=ci.minFilter=VK_FILTER_LINEAR;ci.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;ci.addressModeU=ci.addressModeV=ci.addressModeW=clamp?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:VK_SAMPLER_ADDRESS_MODE_REPEAT;ci.anisotropyEnable=mipmaps&&anisotropic;ci.maxAnisotropy=settings.anisotropy;ci.maxLod=mipmaps?VK_LOD_CLAMP_NONE:0;VkSampler s;checked(vk.vkCreateSampler(device,&ci,nullptr,&s),"Create sampler");return s;}
    VkDescriptorSet descriptor(VkDescriptorSetLayout layout){VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=descriptor_pool;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;VkDescriptorSet set;checked(vk.vkAllocateDescriptorSets(device,&ai,&set),"Allocate descriptor");return set;}
    void write_texture_descriptor(Texture& t){VkDescriptorImageInfo ii{t.sampler,t.image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkDescriptorBufferInfo bi{uniforms->gpu.buffer,0,sizeof(Uniform)};VkWriteDescriptorSet writes[2]{};for(auto& w:writes){w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=t.descriptor;w.descriptorCount=1;}writes[0].dstBinding=0;writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[0].pImageInfo=&ii;writes[1].dstBinding=1;writes[1].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;writes[1].pBufferInfo=&bi;vk.vkUpdateDescriptorSets(device,2,writes,0,nullptr);}
    void create_layouts(){
        VkDescriptorSetLayoutBinding bindings[2]{};bindings[0]={0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};bindings[1]={1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};ci.bindingCount=2;ci.pBindings=bindings;checked(vk.vkCreateDescriptorSetLayout(device,&ci,nullptr,&draw_set_layout),"Create draw layout");ci.bindingCount=1;checked(vk.vkCreateDescriptorSetLayout(device,&ci,nullptr,&copy_set_layout),"Create composition layout");
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,8192},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,8192}};VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pc.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;pc.maxSets=8192;pc.poolSizeCount=2;pc.pPoolSizes=sizes;checked(vk.vkCreateDescriptorPool(device,&pc,nullptr,&descriptor_pool),"Create descriptor pool");
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pi.setLayoutCount=1;pi.pSetLayouts=&draw_set_layout;checked(vk.vkCreatePipelineLayout(device,&pi,nullptr,&draw_layout),"Create raster pipeline layout");VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,8};pi.pSetLayouts=&copy_set_layout;pi.pushConstantRangeCount=1;pi.pPushConstantRanges=&push;checked(vk.vkCreatePipelineLayout(device,&pi,nullptr,&copy_layout),"Create final pipeline layout");for(auto& set:copy_sets)set=descriptor(copy_set_layout);
    }
    VkPipeline create_pipeline(VkFormat color,const RasterState* r,unsigned samples,bool world){
        VkPipelineShaderStageCreateInfo stages[2]{};for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=r?raster_vert:final_vert;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=r?(world?raster_frag:raster_ui_frag):final_frag;
        VkVertexInputBindingDescription bindings[]={{0,sizeof(Vertex),VK_VERTEX_INPUT_RATE_VERTEX},{1,sizeof(Vertex),VK_VERTEX_INPUT_RATE_VERTEX}};
        VkVertexInputAttributeDescription attributes[]={{0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,position)},{1,0,VK_FORMAT_R32G32_SFLOAT,offsetof(Vertex,uv)},{2,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,normal)},{3,1,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,position)},{4,1,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,normal)}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};if(r){vi.vertexBindingDescriptionCount=2;vi.pVertexBindingDescriptions=bindings;vi.vertexAttributeDescriptionCount=5;vi.pVertexAttributeDescriptions=attributes;}
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;VkPipelineViewportStateCreateInfo vs{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vs.viewportCount=vs.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.lineWidth=1;rs.frontFace=VK_FRONT_FACE_CLOCKWISE;
        if(r){rs.depthBiasEnable=r->offset;if(r->cull){if(r->cull_face==0x404)rs.cullMode=VK_CULL_MODE_FRONT_BIT;else if(r->cull_face==0x405)rs.cullMode=VK_CULL_MODE_BACK_BIT;else if(r->cull_face==0x408)rs.cullMode=VK_CULL_MODE_FRONT_AND_BACK;else throw std::runtime_error("Unsupported exported cull face");}}
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=static_cast<VkSampleCountFlagBits>(samples);
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};if(r){ds.depthTestEnable=r->depth_test;ds.depthWriteEnable=r->depth_test&&r->depth_write;ds.depthCompareOp=compare(r->depth_function);}
        VkPipelineColorBlendAttachmentState attachments[4]{};attachments[0].colorWriteMask=r?r->mask:15;
        if(r&&r->blend){attachments[0].blendEnable=true;attachments[0].srcColorBlendFactor=blend(r->source);attachments[0].dstColorBlendFactor=blend(r->destination);attachments[0].srcAlphaBlendFactor=attachments[0].srcColorBlendFactor==VK_BLEND_FACTOR_SRC_ALPHA_SATURATE?VK_BLEND_FACTOR_ONE:attachments[0].srcColorBlendFactor;attachments[0].dstAlphaBlendFactor=attachments[0].dstColorBlendFactor;}
        for(unsigned i=1;i<4;++i)attachments[i].colorWriteMask=r&&r->depth_test&&r->depth_write?15:0;
        VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};bs.attachmentCount=world?4:1;bs.pAttachments=attachments;
        VkDynamicState dynamics[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_DEPTH_BIAS};VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=r?3:2;dy.pDynamicStates=dynamics;
        VkFormat formats[]={color,VK_FORMAT_R16G16B16A16_SINT,VK_FORMAT_R32G32B32A32_SINT,VK_FORMAT_R32G32B32A32_SINT};VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.colorAttachmentCount=world?4:1;rendering.pColorAttachmentFormats=formats;rendering.depthAttachmentFormat=r?VK_FORMAT_D32_SFLOAT:VK_FORMAT_UNDEFINED;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.pNext=&rendering;pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&vi;pi.pInputAssemblyState=&ia;pi.pViewportState=&vs;pi.pRasterizationState=&rs;pi.pMultisampleState=&ms;pi.pDepthStencilState=&ds;pi.pColorBlendState=&bs;pi.pDynamicState=&dy;pi.layout=r?draw_layout:copy_layout;VkPipeline result;checked(vk.vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pi,nullptr,&result),"Create graphics pipeline");return result;
    }
    VkPipeline fullscreen_pipeline(VkFormat format){return create_pipeline(format,nullptr,1,false);}
    VkPipeline raster_pipeline(const Draw& d,bool world){
        const auto& r=d.raster;
        const VkFormat format=world&&settings.enhanced?VK_FORMAT_R16G16B16A16_SFLOAT:VK_FORMAT_R8G8B8A8_UNORM;
        PipelineKey k{{unsigned(r.cull),r.cull_face,unsigned(r.offset),unsigned(r.blend),r.source,r.destination,unsigned(r.depth_test),r.depth_function,unsigned(r.depth_write),r.mask,world?unsigned(sample):1,world?1u+unsigned(settings.enhanced)*2:0,unsigned(format)}};
        auto it=pipelines.find(k);if(it!=pipelines.end())return it->second;
        auto p=create_pipeline(format,&r,world?sample:1,world);
        pipelines.emplace(k,p);return p;
    }
    void make_swapchain(){
        drain_presents();wait();{std::scoped_lock lock(queue_mutex);checked(vk.vkQueueWaitIdle(queue),"Wait presentation resize");}idle_present_queue();VkSurfaceCapabilitiesKHR caps;checked(vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps),"Surface capabilities");
        std::uint32_t fc=0;checked(vk.vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&fc,nullptr),"Surface formats");std::vector<VkSurfaceFormatKHR> formats(fc);checked(vk.vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&fc,formats.data()),"Surface formats");
        auto chosen=std::find_if(formats.begin(),formats.end(),[](auto f){return (f.format==VK_FORMAT_B8G8R8A8_UNORM||f.format==VK_FORMAT_R8G8B8A8_UNORM)&&f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;});if(chosen==formats.end())throw std::runtime_error("No non-gamma-encoding Vulkan presentation format");swap_format=chosen->format;
        if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))throw std::runtime_error("Vulkan surface does not support final image transfer");
        VkExtent2D extent=caps.currentExtent;if(extent.width==UINT32_MAX){extent.width=std::clamp(unsigned(width),caps.minImageExtent.width,caps.maxImageExtent.width);extent.height=std::clamp(unsigned(height),caps.minImageExtent.height,caps.maxImageExtent.height);}width=extent.width;height=extent.height;
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};ci.surface=surface;ci.minImageCount=std::max(caps.minImageCount,2u);if(caps.maxImageCount)ci.minImageCount=std::min(ci.minImageCount,caps.maxImageCount);ci.imageFormat=swap_format;ci.imageColorSpace=chosen->colorSpace;ci.imageExtent=extent;ci.imageArrayLayers=1;ci.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.preTransform=caps.currentTransform;ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;if(!(caps.supportedCompositeAlpha&ci.compositeAlpha))ci.compositeAlpha=static_cast<VkCompositeAlphaFlagBitsKHR>(1u<<std::countr_zero(caps.supportedCompositeAlpha));ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;ci.clipped=true;ci.oldSwapchain=swapchain;VkSwapchainKHR fresh;checked(vk.vkCreateSwapchainKHR(device,&ci,nullptr,&fresh),"Create swapchain");if(swapchain)vk.vkDestroySwapchainKHR(device,swapchain,nullptr);swapchain=fresh;
        for(auto s:complete)vk.vkDestroySemaphore(device,s,nullptr);complete.clear();std::uint32_t count=0;checked(vk.vkGetSwapchainImagesKHR(device,swapchain,&count,nullptr),"Swapchain images");swap_images.resize(count);checked(vk.vkGetSwapchainImagesKHR(device,swapchain,&count,swap_images.data()),"Swapchain images");for(unsigned i=0;i<count;++i){VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};VkSemaphore s;checked(vk.vkCreateSemaphore(device,&sc,nullptr,&s),"Create present semaphore");complete.push_back(s);}recreate=false;
    }
    // After drain_presents the present thread is idle but its last copies may still run (cleanup joins it after a
    // device-wide idle). The swapchain stays exclusive: every copy transitions it from UNDEFINED, which takes
    // ownership without a transfer (contents discarded), and each image is presented by the queue that wrote it.
    void idle_present_queue(){if(!presenter.joinable())return;std::scoped_lock lock(present_lock());checked(vk.vkQueueWaitIdle(present_queue),"Wait present copies");}
    // The present thread reads outputs/generated_outputs: drain it before destroying them.
    void destroy_generation_targets(){drain_presents();idle_present_queue();for(auto* i:{&outputs[1],&generated,&generated_outputs[0],&generated_outputs[1]})destroy_image(*i);}
    void destroy_targets(){destroy_generation_targets();for(auto* i:{&world_color,&world_depth,&world_normal,&world_motion,&world_previous_normal,&world_ms_color,&world_ms_depth,&world_ms_normal,&world_ms_motion,&world_ms_previous_normal,&composition,&ui_depth,&outputs[0]})destroy_image(*i);world_extent={};}
    // Composition is also frame generation's interpolation source, which FSR copies for the next frame. Outputs are
    // swap_format (output_pipeline ramps into them), so presentation copies bytes without a format conversion.
    void native_targets(){const VkExtent2D e{unsigned(width),unsigned(height)};if(composition.image&&composition.extent.width==e.width&&composition.extent.height==e.height&&outputs[0].format==swap_format)return;wait();destroy_targets();
        if(output_format!=swap_format){if(output_pipeline)vk.vkDestroyPipeline(device,std::exchange(output_pipeline,VK_NULL_HANDLE),nullptr);output_pipeline=fullscreen_pipeline(swap_format);output_format=swap_format;}
        composition=image(e,VK_FORMAT_R8G8B8A8_UNORM,VulkanEffects::generation_source_usage);ui_depth=image(e,VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);outputs[0]=image(e,swap_format,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,1,1,true);history=false;if(effects)effects->reset_history();}
    // generated stays RGBA8 (FSR interpolation's target); its HUD pass ramps it into a swap_format generated_output.
    void generation_targets(){
        if(generated.image)return;
        const VkExtent2D e{unsigned(width),unsigned(height)};constexpr auto final_usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        generated=image(e,VK_FORMAT_R8G8B8A8_UNORM,VulkanEffects::generation_target_usage);
        outputs[1]=image(e,swap_format,final_usage,1,1,true);
        for(auto& i:generated_outputs)i=image(e,swap_format,final_usage,1,1,true);
    }
    void world_targets(){
        VkExtent2D e{unsigned(std::max(1,int(width*(settings.enhanced?settings.render_scale:1)))),unsigned(std::max(1,int(height*(settings.enhanced?settings.render_scale:1))))};
        const VkFormat color_format=settings.enhanced?VK_FORMAT_R16G16B16A16_SFLOAT:VK_FORMAT_R8G8B8A8_UNORM;
        if(world_color.image&&world_extent.width==e.width&&world_extent.height==e.height&&world_color.format==color_format&&(!world_ms_color.image?sample==1:true))return;
        wait();for(auto* i:{&world_color,&world_depth,&world_normal,&world_motion,&world_previous_normal,&world_ms_color,&world_ms_depth,&world_ms_normal,&world_ms_motion,&world_ms_previous_normal})destroy_image(*i);
        world_extent=e;const auto color_usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        world_color=image(e,color_format,color_usage);world_normal=image(e,VK_FORMAT_R16G16B16A16_SINT,color_usage);
        world_motion=image(e,VK_FORMAT_R32G32B32A32_SINT,color_usage);
        world_previous_normal=image(e,VK_FORMAT_R32G32B32A32_SINT,color_usage);
        world_depth=image(e,VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
        if(sample>1){
            world_ms_color=image(e,color_format,color_usage,sample);
            world_ms_normal=image(e,VK_FORMAT_R16G16B16A16_SINT,color_usage,sample);
            world_ms_motion=image(e,VK_FORMAT_R32G32B32A32_SINT,color_usage,sample);
            world_ms_previous_normal=image(e,VK_FORMAT_R32G32B32A32_SINT,color_usage,sample);
            world_ms_depth=image(e,VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,sample);
        }
        history=false;effects->reset_history();
    }
    void resize(){if(present_stale.exchange(false))recreate=true;int w,h;sdl(SDL_GetWindowSizeInPixels(window,&w,&h));if(w<=0||h<=0||(SDL_GetWindowFlags(window)&SDL_WINDOW_MINIMIZED)){width=std::max(w,0);height=std::max(h,0);recreate=true;history=false;return;}if(unsigned(w)>properties.limits.maxImageDimension2D||unsigned(h)>properties.limits.maxImageDimension2D)throw std::runtime_error("Drawable exceeds Vulkan image limits");if(width!=w||height!=h){width=w;height=h;recreate=true;finished=false;history=false;}if(recreate){make_swapchain();native_targets();}}
    void clear(){wait();if(timings)timings->begin=std::chrono::steady_clock::now();draws.clear();finished=false;world_closed=false;ui=false;world_seen=false;world_end=0;resize();viewport={0,0,width,height};
        // FSR 3.1's own Halton(2,3) sequence; its length follows the actual upscale ratio.
        jitter={0,0};
        if(settings.enhanced&&settings.temporal_aa&&context->fsr&&width>0){
            const int phases=ffxFsr3UpscalerGetJitterPhaseCount(std::max(1,int(width*settings.render_scale)),width);
            if(phases<=0||ffxFsr3UpscalerGetJitterOffset(&jitter[0],&jitter[1],int(frame_number%unsigned(phases)),phases)!=FFX_OK)throw std::runtime_error("FSR jitter sequence unavailable");
        }}
    void camera(const Mat4& v,const Mat4& p,bool interface){if(interface&&world_seen)close_world();view=v;projection=vulkan_projection(p);ui=interface||world_closed;viewport=interface&&width>0&&height>0?fit_original_interface(width,height):Viewport{0,0,width,height};if(!ui){world_view=view;world_projection=projection;}}
    void close_world(){if(world_closed)return;world_end=draws.size();world_closed=true;}
    void hud_camera(){close_world();auto p=interface_projection();if(std::int64_t(width)*3<std::int64_t(height)*4)camera(identity_matrix(),p,true);else{if(height>0)p.values[0]=2.f/(768.f*width/height);camera(identity_matrix(),p,false);}ui=true;}
    std::shared_ptr<Texture> texture_resource(std::shared_ptr<VulkanImage> storage){
        return std::shared_ptr<Texture>(new Texture(std::move(storage)),[this](Texture* t){
            if(t->descriptor)vk.vkFreeDescriptorSets(device,descriptor_pool,1,&t->descriptor);
            if(t->sampler)vk.vkDestroySampler(device,t->sampler,nullptr);
            delete t;
        });
    }
    std::shared_ptr<Texture> make_texture(int w,int h,std::span<const std::uint8_t> pixels,bool mipmaps,bool flip){
        auto storage=std::shared_ptr<VulkanImage>(new VulkanImage,[this](VulkanImage* i){destroy_image(*i);delete i;});
        auto t=texture_resource(std::move(storage));t->mipmaps=mipmaps;t->flip=flip;t->levels=mipmaps?std::bit_width(unsigned(std::max(w,h))):1;t->image=image({unsigned(w),unsigned(h)},VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,1,t->levels);t->sampler=sampler(mipmaps);t->descriptor=descriptor(draw_set_layout);write_texture_descriptor(*t);if(!pixels.empty())upload_pixels(*t,w,h,pixels,0,0);return t;
    }
    void upload_pixels(Texture& t,int w,int h,std::span<const std::uint8_t> pixels,int x,int y){
        wait();
        if(!upload_staging||upload_staging->gpu.size<pixels.size())upload_staging=buffer(pixels.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,false);
        std::memcpy(upload_staging->mapped,pixels.data(),pixels.size());
        begin(upload_command);transition(upload_command,t.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,t.levels);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageOffset={x,y,0};copy.imageExtent={unsigned(w),unsigned(h),1};
        vk.vkCmdCopyBufferToImage(upload_command,upload_staging->gpu.buffer,t.image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        mipmaps(upload_command,t);submit_upload();
    }
    void mipmaps(VkCommandBuffer cmd,Texture& t){
        if(t.levels>1){for(unsigned n=1;n<t.levels;++n){VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};b.srcStageMask=b.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;b.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=t.image.image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,n-1,1,0,1};VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.imageMemoryBarrierCount=1;dep.pImageMemoryBarriers=&b;vk.vkCmdPipelineBarrier2(cmd,&dep);VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,n-1,0,1};blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,n,0,1};blit.srcOffsets[1]={int(std::max(1u,t.image.extent.width>>(n-1))),int(std::max(1u,t.image.extent.height>>(n-1))),1};blit.dstOffsets[1]={int(std::max(1u,t.image.extent.width>>n)),int(std::max(1u,t.image.extent.height>>n)),1};vk.vkCmdBlitImage(cmd,t.image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,t.image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);}
            for(unsigned n=0;n<t.levels;++n){VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};b.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;b.dstStageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;b.srcAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;b.oldLayout=n+1==t.levels?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=t.image.image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,n,1,0,1};VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.imageMemoryBarrierCount=1;dep.pImageMemoryBarriers=&b;vk.vkCmdPipelineBarrier2(cmd,&dep);}t.image.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }else transition(cmd,t.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);}
    void validate_texture(int w,int h,std::span<const std::uint8_t> data){if(w<=0||h<=0||unsigned(w)>properties.limits.maxImageDimension2D||unsigned(h)>properties.limits.maxImageDimension2D||data.size()!=std::size_t(w)*h*4)throw std::runtime_error("Invalid texture dimensions or RGBA byte count");}
    unsigned upload_mesh(std::span<const Vertex> vertices,std::span<const std::uint16_t> indices){if(vertices.empty()||indices.empty()||indices.size()%3||indices.size()>UINT32_MAX||vertices.size()>UINT32_MAX)throw std::runtime_error("Invalid triangle mesh");for(auto i:indices)if(i>=vertices.size())throw std::runtime_error("Mesh index out of range");wait();auto g=std::make_shared<Geometry>();const auto usage=ray?VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT:0;g->vertices=buffer(vertices.size_bytes(),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|usage,ray);g->indices=buffer(indices.size()*4,VK_BUFFER_USAGE_INDEX_BUFFER_BIT|usage,ray);std::memcpy(g->vertices->mapped,vertices.data(),vertices.size_bytes());auto* out=static_cast<std::uint32_t*>(g->indices->mapped);for(std::size_t i=0;i<indices.size();++i)out[i]=indices[i];g->uvs=std::make_shared<std::vector<Vec2>>(vertices.size());std::transform(vertices.begin(),vertices.end(),g->uvs->begin(),[](const Vertex& v){return v.uv;});g->vertex_count=vertices.size();g->index_count=indices.size();g->key=next_key++;g->temporal_key=g->key;g->revision=1;meshes.push_back(std::move(g));return meshes.size();}
    void update_mesh(unsigned id,std::span<const Vertex> vertices){
        if(!id||id>meshes.size()||!meshes[id-1]||vertices.size()!=meshes[id-1]->vertex_count)throw std::runtime_error("Invalid animated mesh update");
        wait();auto& g=meshes[id-1];
        // Compare artwork with the CPU copy of the last upload before rotating uploads; pose/normal changes keep history.
        bool uv_changed=false;auto& uvs=*g->uvs;
        for(std::size_t i=0;i<vertices.size();++i){const Vec2 uv=vertices[i].uv;uv_changed|=uv.x!=uvs[i].x||uv.y!=uvs[i].y;uvs[i]=uv;}
        if(g.use_count()>1){
            auto n=std::make_shared<Geometry>(*g);
            n->key=next_key++;g=std::move(n);
        }
        if(uv_changed)++g->uv_revision;
        // Retain the exact last submitted pose. Reuse two uploads; no CPU pose copy.
        if(g->vertices.use_count()>1) {
            if(!g->spare_vertices || g->spare_vertices.use_count()>1)
                g->spare_vertices=buffer(vertices.size_bytes(),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|(ray?VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT:0),ray);
            g->vertices.swap(g->spare_vertices);
        }
        std::memcpy(g->vertices->mapped,vertices.data(),vertices.size_bytes());++g->revision;
    }
    void record(unsigned id,unsigned texture,const Material& m,const MaterialPass& p,const DrawState& state){if(finished)throw std::runtime_error("Cannot draw after frame capture/finalization; clear first");if(!id||id>meshes.size()||!meshes[id-1]||texture>textures.size())throw std::runtime_error("Unknown mesh or texture");if(p.alpha_test&&(p.alpha_function<0x200||p.alpha_function>0x207))throw std::runtime_error("Unsupported exported alpha function");Draw d;d.mesh=meshes[id-1];d.texture=texture?textures[texture-1]:white;d.state=state;d.view=view;d.projection=projection;d.viewport=viewport;d.world=!ui&&!world_closed;d.enhanced=settings.enhanced&&d.world;d.alpha_function=p.alpha_test?p.alpha_function:0;d.alpha_reference=std::clamp(p.alpha_reference,0.f,1.f);d.raster={m.cull,m.polygon_offset,p.blend,p.depth_test,p.depth_write,m.cull_face,p.blend_source,p.blend_destination,p.depth_function,0,m.polygon_offset_factor,m.polygon_offset_units};for(unsigned i=0;i<4;++i)if(p.color_write[i])d.raster.mask|=1u<<i;compare(p.depth_function);if(p.blend){blend(p.blend_source);blend(p.blend_destination);}if(state.lighting){normal_matrix4(multiply(view,state.model));normal_matrix4(state.model);}world_seen|=d.world;draws.push_back(std::move(d));}
    void prepare_correspondence() {
        next_draws.clear();
        if(next_temporal_token+draws.size()>16777216) {
            previous_draws.clear();next_temporal_token=1;history=false;effects->reset_history();
        }
        for(auto& d:draws) {
            d.previous_vertices=d.mesh->vertices;d.previous_model=d.state.model;
            if(!d.enhanced || !d.state.ray_geometry || !d.state.ray_primary || d.raster.blend || !d.raster.depth_write) continue;
            if(!d.state.temporal_id) { d.correspondence=d.state.temporal_static;continue; }
            const auto identity=std::pair{d.state.temporal_id,d.mesh->temporal_key};
            const auto old=std::lower_bound(previous_draws.begin(),previous_draws.end(),identity,
                [](const TemporalDraw& a,const auto& b){return std::pair{a.id,a.key}<b;});
            if(old!=previous_draws.end() && std::pair{old->id,old->key}==identity) {
                const bool material_match=old->texture==d.texture &&
                    old->texture_revision==d.texture->content_revision && old->uv_revision==d.mesh->uv_revision &&
                    old->state.uv_transform==d.state.uv_transform && old->state.color==d.state.color &&
                    old->state.diffuse_light[0]==d.state.diffuse_light[0] &&
                    old->state.diffuse_light[1]==d.state.diffuse_light[1] &&
                    old->state.diffuse_light[2]==d.state.diffuse_light[2] &&
                    old->state.lighting==d.state.lighting && old->state.fog==d.state.fog &&
                    old->alpha_function==d.alpha_function && old->alpha_reference==d.alpha_reference &&
                    old->color_mask==d.raster.mask;
                d.temporal_token=material_match?old->token:next_temporal_token++;
                d.correspondence=history && material_match;
                d.previous_vertices=old->vertices;d.previous_model=old->state.model;
            } else d.temporal_token=next_temporal_token++;
            next_draws.push_back({identity.first,identity.second,d.temporal_token,d.mesh->vertices,d.texture,d.state,
                                  d.mesh->uv_revision,d.texture->content_revision,d.alpha_function,d.raster.mask,d.alpha_reference});
        }
        std::sort(next_draws.begin(),next_draws.end(),[](const TemporalDraw& a,const TemporalDraw& b){
            return std::pair{a.id,a.key}<std::pair{b.id,b.key};
        });
    }
    void update_uniforms(){if(draws.size()*uniform_stride>UINT32_MAX)throw std::runtime_error("Frame uniform storage exceeds dynamic offset range");const auto bytes=std::max<VkDeviceSize>(uniform_stride,draws.size()*uniform_stride);if(uniforms->gpu.size<bytes){uniforms=buffer(bytes,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,false);write_texture_descriptor(*white);for(auto& t:textures)write_texture_descriptor(*t);for(auto& d:draws)write_texture_descriptor(*d.texture);}
        const float world_sx=float(world_extent.width)/width,world_sy=float(world_extent.height)/height;
        for(std::size_t i=0;i<draws.size();++i){
            const auto& d=draws[i];Uniform u{};
            u.model_view=multiply(d.view,d.state.model);u.projection=d.projection;
            const auto normal_eye=d.state.lighting?normal_matrix4(u.model_view):identity_matrix();
            std::copy_n(normal_eye.values.begin(),u.normal_eye.size(),u.normal_eye.begin());
            const float sx=d.world?world_sx:1.f,sy=d.world?world_sy:1.f;
            const float vp_x=d.viewport.x*sx,vp_y=(height-d.viewport.y-d.viewport.height)*sy;
            const float vp_width=d.viewport.width*sx,vp_height=d.viewport.height*sy;
            if(vp_width>0&&vp_height>0){
                const float scale_x=2.f/vp_width,scale_y=2.f/vp_height;
                u.raster_grid={scale_x,scale_y,-1.f-scale_x*vp_x,-1.f-scale_y*vp_y};
            }
            u.normal_world=d.world&&d.state.ray_geometry?normal_matrix4(d.state.model,d.state.lighting):identity_matrix();
            const auto light=transform_direction(d.view,{.5f,1,.3f});u.light={light.x,light.y,light.z,0};
            u.previous_mvp=multiply(previous_vp,d.previous_model);
            u.previous_normal_world=d.world&&d.state.ray_geometry?normal_matrix4(d.previous_model,d.state.lighting):identity_matrix();
            u.temporal={std::int32_t(d.temporal_token),d.correspondence?1:0,0,0};
            u.diffuse=d.state.diffuse_light;u.diffuse[3]=d.state.fog?-1:1;
            u.color=d.state.color;u.uv=d.state.uv_transform;
            auto up=transform_direction(d.view,{0,1,0});const float length=std::sqrt(up.x*up.x+up.y*up.y+up.z*up.z);
            u.world_up=length>0?std::array<float,4>{up.x/length,up.y/length,up.z/length,d.texture->flip?1.f:0.f}:std::array<float,4>{0,1,0,d.texture->flip?1.f:0.f};
            const float validity=d.world&&d.state.ray_geometry?((d.state.temporal_static||d.correspondence)&&!d.raster.blend&&d.raster.depth_write?1.f:0.f):-1.f;
            u.flags={d.state.lighting?1.f:0.f,d.enhanced?1.f:0.f,d.texture!=white?1.f:0.f,validity};
            u.alpha_jitter={float(d.alpha_function),d.alpha_reference,d.world&&d.enhanced?2*jitter[0]/world_extent.width:0,d.world&&d.enhanced?2*jitter[1]/world_extent.height:0};
            std::memcpy(static_cast<std::byte*>(uniforms->mapped)+i*uniform_stride,&u,sizeof(u));
        }
    }
    void viewport_for(VkExtent2D extent,Viewport v){const float sx=float(extent.width)/width,sy=float(extent.height)/height;VkViewport vp{v.x*sx,(height-v.y-v.height)*sy,v.width*sx,v.height*sy,0,1};VkRect2D sc{{int(vp.x),int(vp.y)},{unsigned(vp.width),unsigned(vp.height)}};vk.vkCmdSetViewport(command,0,1,&vp);vk.vkCmdSetScissor(command,0,1,&sc);}
    void raster_draws(std::size_t first,std::size_t last,bool world){for(std::size_t i=first;i<last;++i){const auto& d=draws[i];viewport_for(world?world_extent:VkExtent2D{unsigned(width),unsigned(height)},d.viewport);vk.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,raster_pipeline(d,world));vk.vkCmdSetDepthBias(command,d.raster.offset_units,0,d.raster.offset_factor);const std::uint32_t offset=i*uniform_stride;vk.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,draw_layout,0,1,&d.texture->descriptor,1,&offset);VkDeviceSize offsets[2]{};VkBuffer buffers[]={d.mesh->vertices->gpu.buffer,d.previous_vertices->gpu.buffer};vk.vkCmdBindVertexBuffers(command,0,2,buffers,offsets);vk.vkCmdBindIndexBuffer(command,d.mesh->indices->gpu.buffer,0,VK_INDEX_TYPE_UINT32);vk.vkCmdDrawIndexed(command,d.mesh->index_count,1,0,0,0);}}
    void render_world(){
        VulkanImage& color=sample>1?world_ms_color:world_color;VulkanImage& depth=sample>1?world_ms_depth:world_depth;VulkanImage& normal=sample>1?world_ms_normal:world_normal;
        VulkanImage& motion=sample>1?world_ms_motion:world_motion;VulkanImage& previous_normal=sample>1?world_ms_previous_normal:world_previous_normal;
        for(auto* i:{&color,&normal,&motion,&previous_normal,&world_color,&world_normal,&world_motion,&world_previous_normal})transition(command,*i,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);for(auto* i:{&depth,&world_depth})transition(command,*i,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo colors[4]{};for(auto& c:colors){c.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;c.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;c.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;c.storeOp=VK_ATTACHMENT_STORE_OP_STORE;}colors[0].imageView=color.view;colors[0].clearValue.color={{0,0,0,1}};colors[1].imageView=normal.view;colors[1].clearValue.color.int32[3]=-1;
        colors[2].imageView=motion.view;colors[2].clearValue.color.int32[3]=-1;colors[3].imageView=previous_normal.view;
        colors[3].clearValue.color.int32[0]=colors[3].clearValue.color.int32[2]=colors[3].clearValue.color.int32[3]=std::bit_cast<std::int32_t>(std::uint32_t{0x80008000u});
        // Integer geometry can legally resolve sample zero, matching depth.
        // Authored color still averages every MSAA sample in display space.
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};da.imageView=depth.view;da.imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;da.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;da.storeOp=VK_ATTACHMENT_STORE_OP_STORE;da.clearValue.depthStencil={1,0};
        if(sample>1){
            VulkanImage* resolved[]={&world_color,&world_normal,&world_motion,&world_previous_normal};
            for(unsigned i=0;i<4;++i){colors[i].resolveMode=i==0?VK_RESOLVE_MODE_AVERAGE_BIT:VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;colors[i].resolveImageView=resolved[i]->view;colors[i].resolveImageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;}
            da.resolveMode=VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;da.resolveImageView=world_depth.view;da.resolveImageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea.extent=world_extent;ri.layerCount=1;ri.colorAttachmentCount=4;ri.pColorAttachments=colors;ri.pDepthAttachment=&da;vk.vkCmdBeginRendering(command,&ri);raster_draws(0,world_end,true);vk.vkCmdEndRendering(command);
        for(auto* i:{&world_color,&world_normal,&world_motion,&world_previous_normal})transition(command,*i,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if(sample>1) for(auto* i:{&world_ms_normal,&world_ms_motion,&world_ms_previous_normal})transition(command,*i,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // Effects sample depth and FSR declares it COMPUTE_READ (shader-read-only layout).
        transition(command,world_depth,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    void fullscreen(const VulkanImage& source,VulkanImage& target,VkPipeline pipeline,bool copy,unsigned set){const auto copy_set=copy_sets[set];transition(command,target,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);VkDescriptorImageInfo ii{frame_sampler,source.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=copy_set;w.dstBinding=0;w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.descriptorCount=1;w.pImageInfo=&ii;vk.vkUpdateDescriptorSets(device,1,&w,0,nullptr);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};color.imageView=target.view;color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;color.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea.extent=target.extent;ri.layerCount=1;ri.colorAttachmentCount=1;ri.pColorAttachments=&color;vk.vkCmdBeginRendering(command,&ri);viewport_for(target.extent,{0,0,width,height});vk.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vk.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,copy_layout,0,1,&copy_set,0,nullptr);struct {float ramp;unsigned copy;}push{float(brightness),unsigned(copy)};vk.vkCmdPushConstants(command,copy_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);vk.vkCmdDraw(command,3,1,0,0);vk.vkCmdEndRendering(command);}
    void render_ui(VulkanImage& target){
        transition(command,target,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        transition(command,ui_depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView=target.view;color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp=world_seen?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;color.clearValue.color={{0,0,0,1}};
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView=ui_depth.view;depth.imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;depth.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;depth.clearValue.depthStencil={1,0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea.extent=target.extent;ri.layerCount=1;
        ri.colorAttachmentCount=1;ri.pColorAttachments=&color;ri.pDepthAttachment=&depth;
        vk.vkCmdBeginRendering(command,&ri);raster_draws(world_seen?world_end:0,draws.size(),false);vk.vkCmdEndRendering(command);
        transition(command,target,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    void finish(){if(finished)return;if(width<=0||height<=0||(SDL_GetWindowFlags(window)&SDL_WINDOW_MINIMIZED)){history=false;previous_draws.clear();next_draws.clear();effects->reset_history();finished=true;return;}if(!world_closed)close_world();if(world_seen)world_targets();else{world_end=0;history=false;previous_draws.clear();effects->reset_history();}prepare_correspondence();update_uniforms();
        // Frame generation: this frame's slot was last presented by job frame-2 (at most one job outstanding).
        generated_valid=false;
        if(generating){slot^=1u;drain_presents(1);generation_targets();}
        begin(command);if(timings)vk.vkCmdResetQueryPool(command,context->timings,0,2*(GpuFrame+1));gpu_timestamp(*context,command,GpuFrame,false);
        VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};host.srcStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;host.srcAccessMask=VK_ACCESS_2_HOST_WRITE_BIT;host.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;host.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT;VkDependencyInfo visibility{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};visibility.memoryBarrierCount=1;visibility.pMemoryBarriers=&host;vk.vkCmdPipelineBarrier2(command,&visibility);
        if(world_seen){gpu_timestamp(*context,command,GpuWorld,false);render_world();gpu_timestamp(*context,command,GpuWorld,true);if(settings.enhanced){
            ray_instances.clear();ray_images.clear();ray_textures.clear();
            ray_textures.push_back(white.get());
            const auto ray_draw=[](const Draw& d){return d.state.ray_geometry&&d.state.ray_primary&&!d.raster.blend&&d.raster.depth_write;};
            if(ray&&settings.ray_tracing){
                for(std::size_t i=0;i<world_end;++i)if(ray_draw(draws[i]))ray_textures.push_back(draws[i].texture.get());
            }
            // A sorted flat texture-index table keeps capacity without per-frame map-node allocations.
            const std::less<const Texture*> texture_order;
            std::sort(ray_textures.begin(),ray_textures.end(),texture_order);
            ray_textures.erase(std::unique(ray_textures.begin(),ray_textures.end()),ray_textures.end());
            for(const auto* t:ray_textures)ray_images.push_back({t->sampler,t->image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            if(ray&&settings.ray_tracing)for(std::size_t i=0;i<world_end;++i){
                const auto& d=draws[i];
                if(!ray_draw(d))continue;
                const unsigned texture_index=std::lower_bound(ray_textures.begin(),ray_textures.end(),d.texture.get(),texture_order)-ray_textures.begin();
                VulkanRayInstance instance;
                instance.mesh={d.mesh->vertices->gpu,d.mesh->indices->gpu,unsigned(d.mesh->vertex_count),unsigned(d.mesh->index_count),d.mesh->revision,d.mesh->key};
                instance.model=d.state.model;instance.previous_vertices=d.previous_vertices->gpu;instance.previous_model=d.previous_model;instance.texture_index=texture_index;
                instance.uv_transform=d.state.uv_transform;instance.color=d.state.color;instance.diffuse_light=d.state.diffuse_light;
                instance.alpha_function=d.alpha_function;instance.alpha_reference=d.alpha_reference;instance.alpha_test=d.alpha_function!=0;
                instance.flip_vertical=d.texture->flip;instance.cull=d.raster.cull;instance.cull_face=d.raster.cull_face;
                instance.lighting=d.state.lighting;
                ray_instances.push_back(instance);
            }
            // Ray surfaces (normal w>=0) come only from world ray_geometry draws; fog is all-or-none per level in the game.
            const bool fog=std::all_of(draws.begin(),draws.begin()+std::ptrdiff_t(world_end),[](const Draw& d){return !d.state.ray_geometry||d.state.fog;});
            VulkanEffectsFrame frame{command,&world_color,&world_depth,&world_normal,world_extent,{unsigned(width),unsigned(height)},world_view,world_projection,previous_vp,jitter,ray_instances,ray_images,settings,history,&world_motion,&world_previous_normal,sample>1?&world_ms_motion:nullptr,sample>1?&world_ms_normal:nullptr,sample>1?&world_ms_previous_normal:nullptr,generating?composition.format:VK_FORMAT_UNDEFINED,fog};const auto& result=effects->render(frame);
            gpu_timestamp(*context,command,GpuComposite,false);fullscreen(result,composition,final_pipeline,true,0);gpu_timestamp(*context,command,GpuComposite,true);
            // IKV renderer_vk.cpp:327-361: interpolate the HUD-less composition, then draw the HUD onto both frames.
            if(generating&&effects->frame_generation()){gpu_timestamp(*context,command,GpuGeneration,false);generated_valid=effects->interpolate(command,composition,generated);gpu_timestamp(*context,command,GpuGeneration,true);}
        }else{gpu_timestamp(*context,command,GpuComposite,false);fullscreen(world_color,composition,final_pipeline,true,0);gpu_timestamp(*context,command,GpuComposite,true);}previous_vp=multiply(world_projection,world_view);history=settings.enhanced;++frame_number;}
        gpu_timestamp(*context,command,GpuUi,false);
        render_ui(composition);fullscreen(composition,outputs[slot],output_pipeline,false,1);transition(command,outputs[slot],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if(generated_valid){render_ui(generated);fullscreen(generated,generated_outputs[slot],output_pipeline,false,2);transition(command,generated_outputs[slot],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);}
        gpu_timestamp(*context,command,GpuUi,true);gpu_timestamp(*context,command,GpuFrame,true);
        checked(vk.vkEndCommandBuffer(command),"End frame commands");checked(vk.vkResetFences(device,1,&fence),"Reset frame fence");VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=command;VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};si.commandBufferInfoCount=1;si.pCommandBufferInfos=&cb;
        // Generating: wait until the present queue finished copying this slot's previous job (rewriting outputs and
        // generated_outputs), and signal `rendered` for this frame's copies.
        VkSemaphoreSubmitInfo reuse{},done{};reuse.sType=done.sType=VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        if(generating){reuse.semaphore=copied;reuse.value=slot_job[slot];reuse.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;done.semaphore=rendered;done.value=++rendered_value;done.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            si.waitSemaphoreInfoCount=1;si.pWaitSemaphoreInfos=&reuse;si.signalSemaphoreInfoCount=1;si.pSignalSemaphoreInfos=&done;}
        {std::scoped_lock lock(queue_mutex);checked(vk.vkQueueSubmit2(queue,1,&si,fence),"Submit rendered frame");}
        // A resubmit without clear() (out-of-date swapchain) has no recording start of its own.
        if(timings&&timings->begin!=std::chrono::steady_clock::time_point{})timings->cpu.push_back(elapsed_ms(std::exchange(timings->begin,{}),std::chrono::steady_clock::now()));
        wait();frame_ready=std::chrono::steady_clock::now();finished=true;
        if(timings)record_frame_timings();
        previous_draws.swap(next_draws);next_draws.clear();
    }
    void present(){finish();if(width<=0||height<=0||(SDL_GetWindowFlags(window)&SDL_WINDOW_MINIMIZED))return;
        if(generating){
            // The present thread shows the generated frame (unless history reset) and then this one.
            {std::scoped_lock lock(present_mutex);++present_queued;slot_job[slot]=present_queued;present_jobs.push_back({slot,generated_valid,frame_ready,present_queued,rendered_value});}
            present_cv.notify_all();return;
        }
        // Not generating: the present thread is drained and idle, so this thread owns queue and swapchain.
        std::uint32_t index=0;auto result=vk.vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,acquired,VK_NULL_HANDLE,&index);if(result==VK_ERROR_OUT_OF_DATE_KHR){recreate=true;resize();finished=false;finish();result=vk.vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,acquired,VK_NULL_HANDLE,&index);}if(result!=VK_SUCCESS&&result!=VK_SUBOPTIMAL_KHR)checked(result,"Acquire swapchain image");if(result==VK_SUBOPTIMAL_KHR)recreate=true;
        begin(upload_command);if(timings)vk.vkCmdResetQueryPool(upload_command,context->timings,2*GpuPresent,2);gpu_timestamp(*context,upload_command,GpuPresent,false);
        VulkanImage swap{};swap.image=swap_images[index];swap.format=swap_format;swap.extent={unsigned(width),unsigned(height)};transition(upload_command,swap,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.srcOffsets[1]=blit.dstOffsets[1]={width,height,1};vk.vkCmdBlitImage(upload_command,outputs[slot].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,swap.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);transition(upload_command,swap,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        gpu_timestamp(*context,upload_command,GpuPresent,true);checked(vk.vkEndCommandBuffer(upload_command),"End presentation transfer");VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=upload_command;VkSemaphoreSubmitInfo waits{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};waits.semaphore=acquired;waits.stageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;VkSemaphoreSubmitInfo signals{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};signals.semaphore=complete[index];signals.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};si.waitSemaphoreInfoCount=1;si.pWaitSemaphoreInfos=&waits;si.commandBufferInfoCount=1;si.pCommandBufferInfos=&cb;si.signalSemaphoreInfoCount=1;si.pSignalSemaphoreInfos=&signals;checked(vk.vkQueueSubmit2(queue,1,&si,VK_NULL_HANDLE),"Submit presentation transfer");VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&complete[index];pi.swapchainCount=1;pi.pSwapchains=&swapchain;pi.pImageIndices=&index;result=vk.vkQueuePresentKHR(queue,&pi);if(result==VK_ERROR_OUT_OF_DATE_KHR||result==VK_SUBOPTIMAL_KHR)recreate=true;else checked(result,"Present Vulkan frame");
        if(timings&&result!=VK_ERROR_OUT_OF_DATE_KHR)record_present();
        // ponytail: one fully completed frame in flight; split frame resources only if profiling warrants overlap.
        checked(vk.vkQueueWaitIdle(queue),"Wait presentation");
        if(timings)read_timings(GpuPresent,1);
    }
    std::vector<std::uint8_t> capture(bool generated_frame=false){
        finish();if(width<=0||height<=0||(SDL_GetWindowFlags(window)&SDL_WINDOW_MINIMIZED))return {};
        if(generated_frame&&!generated_valid)return {};
        const VulkanImage& source=generated_frame?generated_outputs[slot]:outputs[slot];
        const VkDeviceSize bytes=VkDeviceSize(width)*height*4;
        if(!readback||readback->gpu.size<bytes)readback=buffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,false,true);
        begin(upload_command);VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={unsigned(width),unsigned(height),1};
        vk.vkCmdCopyImageToBuffer(upload_command,source.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback->gpu.buffer,1,&region);
        VkMemoryBarrier2 host_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};host_read.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;host_read.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;host_read.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;host_read.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo visibility{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};visibility.memoryBarrierCount=1;visibility.pMemoryBarriers=&host_read;vk.vkCmdPipelineBarrier2(upload_command,&visibility);submit_upload();
        std::vector<std::uint8_t> result(std::size_t(width)*height*4);const std::size_t stride=std::size_t(width)*4;
        for(int y=0;y<height;++y)std::memcpy(result.data()+std::size_t(y)*stride,static_cast<std::uint8_t*>(readback->mapped)+std::size_t(height-1-y)*stride,stride);
        if(source.format==VK_FORMAT_B8G8R8A8_UNORM)for(std::size_t i=0;i<result.size();i+=4)std::swap(result[i],result[i+2]); // swap_format BGRA outputs
        return result;
    }
    void set_graphics(const GraphicsSettings& s){
        if(!valid_graphics(s))throw std::runtime_error("Invalid graphics settings");
        wait();
        if(s.fullscreen!=bool(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN))sdl(SDL_SetWindowFullscreen(window,s.fullscreen));
        const bool reset=s.samples!=settings.samples||s.render_scale!=settings.render_scale||s.temporal_aa!=settings.temporal_aa||s.enhanced!=settings.enhanced;
        const bool filtering=std::min(s.anisotropy,max_aniso)!=settings.anisotropy;
        settings=s;settings.anisotropy=std::min(s.anisotropy,max_aniso);select_samples();update_generation();
        if(filtering)for(auto& t:textures)if(t->mipmaps){
            if(t.use_count()>1){
                auto replacement=texture_resource(t->storage);
                replacement->levels=t->levels;replacement->mipmaps=t->mipmaps;replacement->flip=t->flip;
                replacement->sampler=sampler(true);replacement->descriptor=descriptor(draw_set_layout);
                write_texture_descriptor(*replacement);t=std::move(replacement);
            }else{vk.vkDestroySampler(device,t->sampler,nullptr);t->sampler=sampler(true);write_texture_descriptor(*t);}
        }
        if(reset){for(auto* i:{&world_color,&world_depth,&world_normal,&world_motion,&world_previous_normal,&world_ms_color,&world_ms_depth,&world_ms_normal,&world_ms_motion,&world_ms_previous_normal})destroy_image(*i);history=false;previous_draws.clear();effects->reset_history();}
        resize();
    }
    void cleanup()noexcept{
        {std::scoped_lock lock(present_mutex);present_stop=true;}present_cv.notify_all();if(presenter.joinable())presenter.join();present_error=nullptr;
        if(device&&vk.vkDeviceWaitIdle)vk.vkDeviceWaitIdle(device);
        if(effects){std::cerr<<"Vulkan execution: acceleration builds="<<effects->acceleration_builds()<<", ray-query frames="<<effects->ray_query_frames()<<'\n';effects.reset();}
        if(timings)print_timings();
        if(context&&context->timings)vk.vkDestroyQueryPool(device,context->timings,nullptr);
        draws.clear();previous_draws.clear();next_draws.clear();meshes.clear();textures.clear();white.reset();uniforms.reset();upload_staging.reset();readback.reset();context.reset();
        if(device){if(output_pipeline)vk.vkDestroyPipeline(device,output_pipeline,nullptr);for(auto s:{rendered,copied})if(s)vk.vkDestroySemaphore(device,s,nullptr);}
        if(device){destroy_targets();for(auto [key,p]:pipelines)vk.vkDestroyPipeline(device,p,nullptr);if(final_pipeline)vk.vkDestroyPipeline(device,final_pipeline,nullptr);for(auto m:{raster_vert,raster_frag,raster_ui_frag,final_vert,final_frag})if(m)vk.vkDestroyShaderModule(device,m,nullptr);if(frame_sampler)vk.vkDestroySampler(device,frame_sampler,nullptr);if(draw_layout)vk.vkDestroyPipelineLayout(device,draw_layout,nullptr);if(copy_layout)vk.vkDestroyPipelineLayout(device,copy_layout,nullptr);if(descriptor_pool)vk.vkDestroyDescriptorPool(device,descriptor_pool,nullptr);if(draw_set_layout)vk.vkDestroyDescriptorSetLayout(device,draw_set_layout,nullptr);if(copy_set_layout)vk.vkDestroyDescriptorSetLayout(device,copy_set_layout,nullptr);for(auto s:complete)vk.vkDestroySemaphore(device,s,nullptr);if(acquired)vk.vkDestroySemaphore(device,acquired,nullptr);for(auto s:present_acquired)if(s)vk.vkDestroySemaphore(device,s,nullptr);for(auto f:present_fences)if(f)vk.vkDestroyFence(device,f,nullptr);if(present_pool)vk.vkDestroyCommandPool(device,present_pool,nullptr);if(fence)vk.vkDestroyFence(device,fence,nullptr);if(pool)vk.vkDestroyCommandPool(device,pool,nullptr);if(swapchain)vk.vkDestroySwapchainKHR(device,swapchain,nullptr);vk.vkDestroyDevice(device,nullptr);device=VK_NULL_HANDLE;}
        if(surface&&vk.vkDestroySurfaceKHR)vk.vkDestroySurfaceKHR(instance,surface,nullptr);if(instance&&vk.vkDestroyInstance)vk.vkDestroyInstance(instance,nullptr);if(window)SDL_DestroyWindow(window);if(loaded)SDL_Vulkan_UnloadLibrary();
    }
};
VulkanRenderer::VulkanRenderer(const DisplayOptions& o):impl_(std::make_unique<Impl>(o)){}
VulkanRenderer::~VulkanRenderer()noexcept=default;
SDL_Window* VulkanRenderer::window()const noexcept{return impl_->window;}
int VulkanRenderer::pixel_width()const noexcept{return impl_->width;}
int VulkanRenderer::pixel_height()const noexcept{return impl_->height;}
int VulkanRenderer::samples()const noexcept{return impl_->sample==1?0:impl_->sample;}
float VulkanRenderer::anisotropy()const noexcept{return impl_->settings.anisotropy;}
int VulkanRenderer::max_samples()const noexcept{return impl_->max_sample;}
float VulkanRenderer::max_anisotropy()const noexcept{return impl_->max_aniso;}
void VulkanRenderer::set_graphics(const GraphicsSettings& s){impl_->set_graphics(s);}
GraphicsSettings VulkanRenderer::graphics_settings()const noexcept{auto s=impl_->settings;s.samples=samples();s.fullscreen=SDL_GetWindowFlags(impl_->window)&SDL_WINDOW_FULLSCREEN;return s;}
void VulkanRenderer::resize(){impl_->resize();}
void VulkanRenderer::clear(){impl_->clear();}
void VulkanRenderer::set_brightness(std::uint16_t b)noexcept{impl_->brightness=unsigned(b)+128;}
void VulkanRenderer::camera(const Mat4& v,const Mat4& p,bool i){impl_->camera(v,p,i);}
void VulkanRenderer::hud_camera(){impl_->hud_camera();}
void VulkanRenderer::finish_world(){impl_->close_world();}
unsigned VulkanRenderer::upload_mesh(std::span<const Vertex> v,std::span<const std::uint16_t> i){return impl_->upload_mesh(v,i);}
void VulkanRenderer::update_mesh(unsigned m,std::span<const Vertex> v){impl_->update_mesh(m,v);}
void VulkanRenderer::release_mesh(unsigned m)noexcept{if(m&&m<=impl_->meshes.size())impl_->meshes[m-1].reset();}
unsigned VulkanRenderer::upload_texture(int w,int h,std::span<const std::uint8_t> p,bool m,bool f){impl_->validate_texture(w,h,p);impl_->textures.push_back(impl_->make_texture(w,h,p,m,f));return impl_->textures.size();}
void VulkanRenderer::update_texture(unsigned id,int w,int h,std::span<const std::uint8_t> p,int x,int y){
    if(!id||id>impl_->textures.size())throw std::runtime_error("Unknown texture");auto& t=impl_->textures[id-1];if(w<=0||h<=0||x<0||y<0||unsigned(w)>t->image.extent.width||unsigned(h)>t->image.extent.height||unsigned(x)>t->image.extent.width-unsigned(w)||unsigned(y)>t->image.extent.height-unsigned(h)||p.size()!=std::size_t(w)*h*4)throw std::runtime_error("Invalid video texture update rectangle");
    impl_->wait();
    if(t.use_count()>1||t->storage.use_count()>1){
        auto old=t;
        t=impl_->make_texture(old->image.extent.width,old->image.extent.height,{},old->mipmaps,old->flip);
        t->content_revision=old->content_revision;
        impl_->begin(impl_->upload_command);
        impl_->transition(impl_->upload_command,old->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,old->levels);
        impl_->transition(impl_->upload_command,t->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,t->levels);
        std::array<VkImageCopy,32> copies{};
        for(unsigned n=0;n<old->levels;++n){copies[n].srcSubresource=copies[n].dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,n,0,1};copies[n].extent={std::max(1u,old->image.extent.width>>n),std::max(1u,old->image.extent.height>>n),1};}
        impl_->vk.vkCmdCopyImage(impl_->upload_command,old->image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,t->image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,old->levels,copies.data());
        impl_->transition(impl_->upload_command,old->image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,old->levels);
        impl_->transition(impl_->upload_command,t->image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,t->levels);
        impl_->submit_upload();
    }
    impl_->upload_pixels(*t,w,h,p,x,y);
    ++t->content_revision;
}
void VulkanRenderer::draw(unsigned m,unsigned t,const Material& a,const MaterialPass& p,const DrawState& s){impl_->record(m,t,a,p,s);}
void VulkanRenderer::present(){impl_->present();}
std::vector<std::uint8_t> VulkanRenderer::capture_rgba(){return impl_->capture();}
std::vector<std::uint8_t> VulkanRenderer::capture_generated_rgba(){return impl_->capture(true);}
void VulkanRenderer::reset_history(){impl_->history=false;impl_->previous_draws.clear();impl_->effects->reset_history();}
const char* VulkanRenderer::backend_name()const noexcept{return "Vulkan";}
bool VulkanRenderer::ray_tracing_available()const noexcept{return impl_->ray;}
} // namespace yami
