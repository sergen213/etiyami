#include "vulkan_renderer.hpp"
#include "vulkan_api.hpp"
#include "vulkan_effects.hpp"
#include "yami_vulkan_shaders.hpp"
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
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
float halton(unsigned i,unsigned base) { float f=1,r=0;while(i){f/=float(base);r+=f*float(i%base);i/=base;}return r; }
}

struct VulkanRenderer::Impl {
    VulkanApi vk{};
    SDL_Window* window=nullptr;
    bool loaded=false;
    VkInstance instance=VK_NULL_HANDLE;
    VkSurfaceKHR surface=VK_NULL_HANDLE;
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    VkDevice device=VK_NULL_HANDLE;
    VkQueue queue=VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::uint32_t family=0;
    bool ray=false,anisotropic=false;
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
    VkShaderModule raster_vert=VK_NULL_HANDLE,raster_frag=VK_NULL_HANDLE,raster_ui_frag=VK_NULL_HANDLE,raster_mask_frag=VK_NULL_HANDLE,final_vert=VK_NULL_HANDLE,final_frag=VK_NULL_HANDLE;
    VkPipeline final_pipeline=VK_NULL_HANDLE;
    std::array<VkDescriptorSet,2> copy_sets{};
    VkSampler frame_sampler=VK_NULL_HANDLE;
    int width=0,height=0,max_sample=1,sample=1;
    VkSampleCountFlags sample_flags=1;
    float max_aniso=1;
    GraphicsSettings settings{};
    unsigned brightness=257,frame_number=0;
    bool finished=false,world_closed=false,ui=false,world_seen=false,history=false,recreate=true,artist_mask_active=false;
    std::size_t world_end=0;
    Mat4 view=identity_matrix(),projection=identity_matrix(),world_view=identity_matrix(),world_projection=identity_matrix(),previous_vp=identity_matrix();
    Viewport viewport{0,0,0,0};
    std::array<float,2> jitter{};
    std::uint64_t next_key=1;
    struct Buffer { VulkanBuffer gpu;void* mapped=nullptr; };
    struct Geometry { std::shared_ptr<Buffer> vertices,spare_vertices,indices;std::size_t vertex_count=0,index_count=0;std::uint64_t key=0,revision=0,temporal_key=0,uv_revision=0; };
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
        Mat4 model_view,projection,normal_eye,normal_world;
        std::array<float,4> light,diffuse,color,uv,world_up,flags,alpha_jitter;
        Mat4 previous_mvp,previous_normal_world;
        std::array<std::int32_t,4> temporal;
    };
    std::shared_ptr<Buffer> uniforms,upload_staging,readback;
    VkDeviceSize uniform_stride=0;
    VulkanImage world_color{},world_depth{},world_normal{},world_ms_color{},world_ms_depth{},world_ms_normal{};
    VulkanImage world_motion{},world_previous_normal{},world_ms_motion{},world_ms_previous_normal{};
    VulkanImage world_artist_mask{};
    VulkanImage composition{},ui_depth{},output{};
    VkExtent2D world_extent{};

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
#undef LOAD_DEVICE
        vk.vkGetDeviceQueue(device,family,0,&queue);
        vk.vkGetPhysicalDeviceMemoryProperties(physical,&memory);
        context=std::make_unique<VulkanContext>(VulkanContext{vk,physical,device,queue,family,ray,properties,memory,scratch_alignment});
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pc.queueFamilyIndex=family;
        checked(vk.vkCreateCommandPool(device,&pc,nullptr,&pool),"Create command pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=2;
        VkCommandBuffer cmds[2];checked(vk.vkAllocateCommandBuffers(device,&ca,cmds),"Allocate command buffers");command=cmds[0];upload_command=cmds[1];
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fc.flags=VK_FENCE_CREATE_SIGNALED_BIT;checked(vk.vkCreateFence(device,&fc,nullptr,&fence),"Create fence");
        VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};checked(vk.vkCreateSemaphore(device,&sc,nullptr,&acquired),"Create acquire semaphore");
        create_layouts();
        raster_vert=shader(spirv::vulkan_raster_vert);raster_frag=shader(spirv::vulkan_raster_frag);raster_ui_frag=shader(spirv::vulkan_raster_ui_frag);raster_mask_frag=shader(spirv::vulkan_raster_mask_frag);final_vert=shader(spirv::vulkan_final_vert);final_frag=shader(spirv::vulkan_final_frag);
        final_pipeline=fullscreen_pipeline(VK_FORMAT_R8G8B8A8_UNORM);
        frame_sampler=sampler(false,true);
        uniform_stride=(sizeof(Uniform)+properties.limits.minUniformBufferOffsetAlignment-1)&~(properties.limits.minUniformBufferOffsetAlignment-1);
        uniforms=buffer(uniform_stride,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,false);
        const std::array<std::uint8_t,4> pixels{{255,255,255,255}};white=make_texture(1,1,pixels,false,false);
        effects=std::make_unique<VulkanEffects>(*context);
        resize();clear();present(); // Commit a real framebuffer before Wayland input-focus gating.
        std::cerr<<"Vulkan device: "<<properties.deviceName<<"; API "<<VK_VERSION_MAJOR(properties.apiVersion)<<'.'<<VK_VERSION_MINOR(properties.apiVersion)<<"; ray query "<<(ray?"available":"unavailable")<<'\n';
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
        // MSAA ray effects add eight frame samplers and eleven fixed resources.
        ray=has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)&&has(VK_KHR_RAY_QUERY_EXTENSION_NAME)&&has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)&&fas.accelerationStructure&&fray.rayQuery&&f.features.shaderInt64&&f12.bufferDeviceAddress&&f12.runtimeDescriptorArray&&f12.shaderSampledImageArrayNonUniformIndexing&&f12.descriptorBindingPartiallyBound&&properties.limits.maxPerStageDescriptorSamplers>=8&&properties.limits.maxDescriptorSetSamplers>=8&&properties.limits.maxPerStageDescriptorSampledImages>=8&&properties.limits.maxDescriptorSetSampledImages>=8&&properties.limits.maxPerStageResources>=11;
        VkPhysicalDeviceAccelerationStructurePropertiesKHR ap{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};VkPhysicalDeviceDepthStencilResolveProperties dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p.pNext=&dp;if(ray)dp.pNext=&ap;vk.vkGetPhysicalDeviceProperties2(physical,&p);if(ray)scratch_alignment=ap.minAccelerationStructureScratchOffsetAlignment;
        if(!(dp.supportedDepthResolveModes&VK_RESOLVE_MODE_SAMPLE_ZERO_BIT))throw std::runtime_error("Vulkan device lacks sample-zero depth resolve");
        anisotropic=f.features.samplerAnisotropy;max_aniso=anisotropic?properties.limits.maxSamplerAnisotropy:1;
        VkPhysicalDeviceVulkan13Features enable13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};enable13.dynamicRendering=enable13.synchronization2=enable13.shaderDemoteToHelperInvocation=true;
        VkPhysicalDeviceVulkan12Features enable12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};enable13.pNext=&enable12;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR enable_as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};VkPhysicalDeviceRayQueryFeaturesKHR enable_ray{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        if(ray){enable12.bufferDeviceAddress=enable12.runtimeDescriptorArray=enable12.shaderSampledImageArrayNonUniformIndexing=enable12.descriptorBindingPartiallyBound=true;enable12.pNext=&enable_as;enable_as.accelerationStructure=true;enable_as.pNext=&enable_ray;enable_ray.rayQuery=true;extensions.insert(extensions.end(),{VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,VK_KHR_RAY_QUERY_EXTENSION_NAME,VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME});}
        float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
        VkPhysicalDeviceFeatures base{};base.samplerAnisotropy=anisotropic;base.independentBlend=true;base.shaderInt64=ray;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.pNext=&enable13;dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qi;dc.enabledExtensionCount=extensions.size();dc.ppEnabledExtensionNames=extensions.data();dc.pEnabledFeatures=&base;checked(vk.vkCreateDevice(physical,&dc,nullptr,&device),"Create Vulkan device");
        sample_flags=properties.limits.framebufferColorSampleCounts&properties.limits.framebufferDepthSampleCounts&properties.limits.sampledImageIntegerSampleCounts&properties.limits.sampledImageColorSampleCounts;
        for(const auto format:{VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16G16B16A16_SINT,VK_FORMAT_R32G32B32A32_SINT,VK_FORMAT_R32G32_SINT,VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_D32_SFLOAT}) {
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
    std::shared_ptr<Buffer> buffer(VkDeviceSize bytes,VkBufferUsageFlags usage,bool address) {
        auto result=std::shared_ptr<Buffer>(new Buffer,[this](Buffer* b){if(b->mapped)vk.vkUnmapMemory(device,b->gpu.memory);if(b->gpu.buffer)vk.vkDestroyBuffer(device,b->gpu.buffer,nullptr);if(b->gpu.memory)vk.vkFreeMemory(device,b->gpu.memory,nullptr);delete b;});
        result->gpu.size=bytes;VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=bytes;bi.usage=usage|(address?VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT:0);bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;checked(vk.vkCreateBuffer(device,&bi,nullptr,&result->gpu.buffer),"Create buffer");
        VkMemoryRequirements mr;vk.vkGetBufferMemoryRequirements(device,result->gpu.buffer,&mr);VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};flags.flags=address?VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT:0;
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.pNext=address?&flags:nullptr;ma.allocationSize=mr.size;ma.memoryTypeIndex=memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);checked(vk.vkAllocateMemory(device,&ma,nullptr,&result->gpu.memory),"Allocate buffer memory");checked(vk.vkBindBufferMemory(device,result->gpu.buffer,result->gpu.memory,0),"Bind buffer");checked(vk.vkMapMemory(device,result->gpu.memory,0,bytes,0,&result->mapped),"Map buffer");
        if(address){VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};ai.buffer=result->gpu.buffer;result->gpu.address=vk.vkGetBufferDeviceAddress(device,&ai);}
        return result;
    }
    VulkanImage image(VkExtent2D extent,VkFormat format,VkImageUsageFlags usage,unsigned samples=1,unsigned levels=1) {
        VulkanImage result{};result.extent=extent;result.format=format;
        try { VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={extent.width,extent.height,1};ci.mipLevels=levels;ci.arrayLayers=1;ci.samples=static_cast<VkSampleCountFlagBits>(samples);ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;checked(vk.vkCreateImage(device,&ci,nullptr,&result.image),"Create image");
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
    void submit_upload(){checked(vk.vkEndCommandBuffer(upload_command),"End upload");VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=upload_command;VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};si.commandBufferInfoCount=1;si.pCommandBufferInfos=&cb;checked(vk.vkQueueSubmit2(queue,1,&si,VK_NULL_HANDLE),"Submit upload");checked(vk.vkQueueWaitIdle(queue),"Wait upload");}
    template<std::size_t N> VkShaderModule shader(const std::uint32_t(&code)[N]){VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=sizeof(code);ci.pCode=code;VkShaderModule m;checked(vk.vkCreateShaderModule(device,&ci,nullptr,&m),"Create shader");return m;}
    VkSampler sampler(bool mipmaps,bool clamp=false){VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};ci.magFilter=ci.minFilter=VK_FILTER_LINEAR;ci.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;ci.addressModeU=ci.addressModeV=ci.addressModeW=clamp?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:VK_SAMPLER_ADDRESS_MODE_REPEAT;ci.anisotropyEnable=mipmaps&&anisotropic;ci.maxAnisotropy=settings.anisotropy;ci.maxLod=mipmaps?VK_LOD_CLAMP_NONE:0;VkSampler s;checked(vk.vkCreateSampler(device,&ci,nullptr,&s),"Create sampler");return s;}
    VkDescriptorSet descriptor(VkDescriptorSetLayout layout){VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=descriptor_pool;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;VkDescriptorSet set;checked(vk.vkAllocateDescriptorSets(device,&ai,&set),"Allocate descriptor");return set;}
    void write_texture_descriptor(Texture& t){VkDescriptorImageInfo ii{t.sampler,t.image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkDescriptorBufferInfo bi{uniforms->gpu.buffer,0,sizeof(Uniform)};VkWriteDescriptorSet writes[2]{};for(auto& w:writes){w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=t.descriptor;w.descriptorCount=1;}writes[0].dstBinding=0;writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[0].pImageInfo=&ii;writes[1].dstBinding=1;writes[1].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;writes[1].pBufferInfo=&bi;vk.vkUpdateDescriptorSets(device,2,writes,0,nullptr);}
    void create_layouts(){
        VkDescriptorSetLayoutBinding bindings[2]{};bindings[0]={0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};bindings[1]={1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};ci.bindingCount=2;ci.pBindings=bindings;checked(vk.vkCreateDescriptorSetLayout(device,&ci,nullptr,&draw_set_layout),"Create draw layout");ci.bindingCount=1;checked(vk.vkCreateDescriptorSetLayout(device,&ci,nullptr,&copy_set_layout),"Create composition layout");
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,8192},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,8192}};VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pc.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;pc.maxSets=8192;pc.poolSizeCount=2;pc.pPoolSizes=sizes;checked(vk.vkCreateDescriptorPool(device,&pc,nullptr,&descriptor_pool),"Create descriptor pool");
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pi.setLayoutCount=1;pi.pSetLayouts=&draw_set_layout;checked(vk.vkCreatePipelineLayout(device,&pi,nullptr,&draw_layout),"Create raster pipeline layout");VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,8};pi.pSetLayouts=&copy_set_layout;pi.pushConstantRangeCount=1;pi.pPushConstantRanges=&push;checked(vk.vkCreatePipelineLayout(device,&pi,nullptr,&copy_layout),"Create final pipeline layout");for(auto& set:copy_sets)set=descriptor(copy_set_layout);
    }
    VkPipeline create_pipeline(VkFormat color,const RasterState* r,unsigned samples,bool world,bool artist=false){
        VkPipelineShaderStageCreateInfo stages[2]{};for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=r?raster_vert:final_vert;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=r?(artist?raster_mask_frag:world?raster_frag:raster_ui_frag):final_frag;
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
        if(artist){
            attachments[0].colorWriteMask=artist_influence(*r)?15:0;
            attachments[0].blendEnable=true;
            attachments[0].srcColorBlendFactor=attachments[0].srcAlphaBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
            attachments[0].dstColorBlendFactor=attachments[0].dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        }
        for(unsigned i=1;i<4;++i)attachments[i].colorWriteMask=r&&r->depth_test&&r->depth_write?15:0;
        VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};bs.attachmentCount=world&&!artist?4:1;bs.pAttachments=attachments;
        VkDynamicState dynamics[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_DEPTH_BIAS};VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=r?3:2;dy.pDynamicStates=dynamics;
        VkFormat formats[]={color,VK_FORMAT_R16G16B16A16_SINT,VK_FORMAT_R32G32B32A32_SINT,VK_FORMAT_R32G32_SINT};VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.colorAttachmentCount=world&&!artist?4:1;rendering.pColorAttachmentFormats=formats;rendering.depthAttachmentFormat=r?VK_FORMAT_D32_SFLOAT:VK_FORMAT_UNDEFINED;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.pNext=&rendering;pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&vi;pi.pInputAssemblyState=&ia;pi.pViewportState=&vs;pi.pRasterizationState=&rs;pi.pMultisampleState=&ms;pi.pDepthStencilState=&ds;pi.pColorBlendState=&bs;pi.pDynamicState=&dy;pi.layout=r?draw_layout:copy_layout;VkPipeline result;checked(vk.vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pi,nullptr,&result),"Create graphics pipeline");return result;
    }
    VkPipeline fullscreen_pipeline(VkFormat format){return create_pipeline(format,nullptr,1,false);}
    VkPipeline raster_pipeline(const Draw& d,bool world,bool artist=false){
        const auto& r=d.raster;
        const VkFormat format=artist?world_artist_mask.format:world&&settings.enhanced?VK_FORMAT_R16G16B16A16_SFLOAT:VK_FORMAT_R8G8B8A8_UNORM;
        PipelineKey k{{unsigned(r.cull),r.cull_face,unsigned(r.offset),unsigned(r.blend),r.source,r.destination,unsigned(r.depth_test),r.depth_function,unsigned(r.depth_write),r.mask,world?unsigned(sample):1,artist?4u:world?1u+unsigned(settings.enhanced)*2:0,unsigned(format)}};
        auto it=pipelines.find(k);if(it!=pipelines.end())return it->second;
        auto p=create_pipeline(format,&r,world?sample:1,world,artist);
        pipelines.emplace(k,p);return p;
    }
    void make_swapchain(){
        wait();checked(vk.vkQueueWaitIdle(queue),"Wait presentation resize");VkSurfaceCapabilitiesKHR caps;checked(vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps),"Surface capabilities");
        std::uint32_t fc=0;checked(vk.vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&fc,nullptr),"Surface formats");std::vector<VkSurfaceFormatKHR> formats(fc);checked(vk.vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&fc,formats.data()),"Surface formats");
        auto chosen=std::find_if(formats.begin(),formats.end(),[](auto f){return (f.format==VK_FORMAT_B8G8R8A8_UNORM||f.format==VK_FORMAT_R8G8B8A8_UNORM)&&f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;});if(chosen==formats.end())throw std::runtime_error("No non-gamma-encoding Vulkan presentation format");swap_format=chosen->format;
        if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))throw std::runtime_error("Vulkan surface does not support final image transfer");
        VkExtent2D extent=caps.currentExtent;if(extent.width==UINT32_MAX){extent.width=std::clamp(unsigned(width),caps.minImageExtent.width,caps.maxImageExtent.width);extent.height=std::clamp(unsigned(height),caps.minImageExtent.height,caps.maxImageExtent.height);}width=extent.width;height=extent.height;
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};ci.surface=surface;ci.minImageCount=std::max(caps.minImageCount,2u);if(caps.maxImageCount)ci.minImageCount=std::min(ci.minImageCount,caps.maxImageCount);ci.imageFormat=swap_format;ci.imageColorSpace=chosen->colorSpace;ci.imageExtent=extent;ci.imageArrayLayers=1;ci.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.preTransform=caps.currentTransform;ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;if(!(caps.supportedCompositeAlpha&ci.compositeAlpha))ci.compositeAlpha=static_cast<VkCompositeAlphaFlagBitsKHR>(1u<<std::countr_zero(caps.supportedCompositeAlpha));ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;ci.clipped=true;ci.oldSwapchain=swapchain;VkSwapchainKHR fresh;checked(vk.vkCreateSwapchainKHR(device,&ci,nullptr,&fresh),"Create swapchain");if(swapchain)vk.vkDestroySwapchainKHR(device,swapchain,nullptr);swapchain=fresh;
        for(auto s:complete)vk.vkDestroySemaphore(device,s,nullptr);complete.clear();std::uint32_t count=0;checked(vk.vkGetSwapchainImagesKHR(device,swapchain,&count,nullptr),"Swapchain images");swap_images.resize(count);checked(vk.vkGetSwapchainImagesKHR(device,swapchain,&count,swap_images.data()),"Swapchain images");for(unsigned i=0;i<count;++i){VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};VkSemaphore s;checked(vk.vkCreateSemaphore(device,&sc,nullptr,&s),"Create present semaphore");complete.push_back(s);}recreate=false;
    }
    void destroy_targets(){for(auto* i:{&world_color,&world_depth,&world_normal,&world_motion,&world_previous_normal,&world_ms_color,&world_ms_depth,&world_ms_normal,&world_ms_motion,&world_ms_previous_normal,&world_artist_mask,&composition,&ui_depth,&output})destroy_image(*i);world_extent={};}
    void native_targets(){const VkExtent2D e{unsigned(width),unsigned(height)};if(composition.image&&composition.extent.width==e.width&&composition.extent.height==e.height)return;wait();destroy_targets();composition=image(e,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);ui_depth=image(e,VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);output=image(e,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);history=false;if(effects)effects->reset_history();}
    void world_targets(){
        VkExtent2D e{unsigned(std::max(1,int(width*(settings.enhanced?settings.render_scale:1)))),unsigned(std::max(1,int(height*(settings.enhanced?settings.render_scale:1))))};
        const VkFormat color_format=settings.enhanced?VK_FORMAT_R16G16B16A16_SFLOAT:VK_FORMAT_R8G8B8A8_UNORM;
        if(world_color.image&&world_extent.width==e.width&&world_extent.height==e.height&&world_color.format==color_format&&(!world_ms_color.image?sample==1:true))return;
        wait();for(auto* i:{&world_color,&world_depth,&world_normal,&world_motion,&world_previous_normal,&world_ms_color,&world_ms_depth,&world_ms_normal,&world_ms_motion,&world_ms_previous_normal,&world_artist_mask})destroy_image(*i);
        world_extent=e;const auto color_usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        world_color=image(e,color_format,color_usage);world_normal=image(e,VK_FORMAT_R16G16B16A16_SINT,color_usage);
        world_motion=image(e,VK_FORMAT_R32G32B32A32_SINT,color_usage);
        world_previous_normal=image(e,VK_FORMAT_R32G32_SINT,color_usage);
        world_depth=image(e,VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
        if(sample>1){
            world_ms_color=image(e,color_format,color_usage,sample);
            world_ms_normal=image(e,VK_FORMAT_R16G16B16A16_SINT,color_usage,sample);
            world_ms_motion=image(e,VK_FORMAT_R32G32B32A32_SINT,color_usage,sample);
            world_ms_previous_normal=image(e,VK_FORMAT_R32G32_SINT,color_usage,sample);
            world_ms_depth=image(e,VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,sample);
        }
        history=false;effects->reset_history();
    }
    void resize(){int w,h;sdl(SDL_GetWindowSizeInPixels(window,&w,&h));if(w<=0||h<=0||(SDL_GetWindowFlags(window)&SDL_WINDOW_MINIMIZED)){width=std::max(w,0);height=std::max(h,0);recreate=true;history=false;return;}if(unsigned(w)>properties.limits.maxImageDimension2D||unsigned(h)>properties.limits.maxImageDimension2D)throw std::runtime_error("Drawable exceeds Vulkan image limits");if(width!=w||height!=h){width=w;height=h;recreate=true;finished=false;history=false;}if(recreate){make_swapchain();native_targets();}}
    void clear(){wait();draws.clear();finished=false;world_closed=false;ui=false;world_seen=false;world_end=0;resize();viewport={0,0,width,height};
        // Rotate the same eight locations each epoch: a 64-frame cycle balances four repeating poses.
        const auto phase=(frame_number+frame_number/8)%8+1;
        jitter=settings.enhanced&&settings.temporal_aa?std::array<float,2>{halton(phase,2)-.5f,halton(phase,3)-.5f}:std::array<float,2>{0,0};}
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
    unsigned upload_mesh(std::span<const Vertex> vertices,std::span<const std::uint16_t> indices){if(vertices.empty()||indices.empty()||indices.size()%3||indices.size()>UINT32_MAX||vertices.size()>UINT32_MAX)throw std::runtime_error("Invalid triangle mesh");for(auto i:indices)if(i>=vertices.size())throw std::runtime_error("Mesh index out of range");wait();auto g=std::make_shared<Geometry>();const auto usage=ray?VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT:0;g->vertices=buffer(vertices.size_bytes(),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|usage,ray);g->indices=buffer(indices.size()*4,VK_BUFFER_USAGE_INDEX_BUFFER_BIT|usage,ray);std::memcpy(g->vertices->mapped,vertices.data(),vertices.size_bytes());auto* out=static_cast<std::uint32_t*>(g->indices->mapped);for(std::size_t i=0;i<indices.size();++i)out[i]=indices[i];g->vertex_count=vertices.size();g->index_count=indices.size();g->key=next_key++;g->temporal_key=g->key;g->revision=1;meshes.push_back(std::move(g));return meshes.size();}
    void update_mesh(unsigned id,std::span<const Vertex> vertices){
        if(!id||id>meshes.size()||!meshes[id-1]||vertices.size()!=meshes[id-1]->vertex_count)throw std::runtime_error("Invalid animated mesh update");
        wait();auto& g=meshes[id-1];
        // Compare artwork before rotating uploads; pose/normal changes keep history.
        const auto* uploaded=g->vertices?static_cast<const Vertex*>(g->vertices->mapped):nullptr;
        bool uv_changed=!uploaded;
        if(uploaded)for(std::size_t i=0;i<vertices.size();++i)
            if(vertices[i].uv.x!=uploaded[i].uv.x || vertices[i].uv.y!=uploaded[i].uv.y) { uv_changed=true;break; }
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
    static int artist_influence(const RasterState& r){
        if(!(r.mask&7) || (r.blend&&r.source==0&&r.destination==1))return 0;
        // Only these alpha-dependent factors prove zero RGB influence without
        // reading destination color/alpha. Other supported blends stay reactive.
        return r.blend&&r.source==0x302&&(r.destination==0x303||r.destination==1)?2:1;
    }
    static bool artist_clean(const Draw& d){
        return d.world&&d.enhanced&&(d.temporal_token>0||d.correspondence)&&
            !d.raster.blend&&d.raster.depth_test&&d.raster.depth_write&&(d.raster.mask&7)==7;
    }
    void prepare_artist_mask(){
        artist_mask_active=false;
        if(!world_seen||!settings.enhanced||!settings.temporal_aa)return;
        for(std::size_t i=0;i<world_end;++i){
            const auto& d=draws[i];const int influence=artist_influence(d.raster);
            if(!influence||artist_clean(d))continue;
            if(influence==2&&!d.state.lighting&&d.state.color[3]==0)continue;
            artist_mask_active=true;break;
        }
        if(!artist_mask_active||world_artist_mask.image)return;
        const auto usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        for(const auto format:{VK_FORMAT_R8_UNORM,VK_FORMAT_R16G16B16A16_SFLOAT}){
            VkFormatProperties flags{};vk.vkGetPhysicalDeviceFormatProperties(physical,format,&flags);
            const auto required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            if((flags.optimalTilingFeatures&required)!=required)continue;
            VkImageFormatProperties support{};
            const auto result=vk.vkGetPhysicalDeviceImageFormatProperties(physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,usage,0,&support);
            if(result==VK_ERROR_FORMAT_NOT_SUPPORTED)continue;
            checked(result,"Query authored-history mask support");
            if(!(support.sampleCounts&unsigned(sample))||support.maxExtent.width<world_extent.width||support.maxExtent.height<world_extent.height)continue;
            world_artist_mask=image(world_extent,format,usage,sample);return;
        }
        throw std::runtime_error("Vulkan authored-history mask lacks sampled blending support at the selected world sample count");
    }
    void update_uniforms(){if(draws.size()*uniform_stride>UINT32_MAX)throw std::runtime_error("Frame uniform storage exceeds dynamic offset range");const auto bytes=std::max<VkDeviceSize>(uniform_stride,draws.size()*uniform_stride);if(uniforms->gpu.size<bytes){uniforms=buffer(bytes,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,false);write_texture_descriptor(*white);for(auto& t:textures)write_texture_descriptor(*t);for(auto& d:draws)write_texture_descriptor(*d.texture);}
        for(std::size_t i=0;i<draws.size();++i){
            const auto& d=draws[i];Uniform u{};
            u.model_view=multiply(d.view,d.state.model);u.projection=d.projection;
            u.normal_eye=d.state.lighting?normal_matrix4(u.model_view):identity_matrix();
            u.normal_world=d.world&&d.state.ray_geometry?normal_matrix4(d.state.model,d.state.lighting):identity_matrix();
            const auto light=transform_direction(d.view,{.5f,1,.3f});u.light={light.x,light.y,light.z,0};
            u.previous_mvp=multiply(previous_vp,d.previous_model);
            u.previous_normal_world=d.world&&d.state.ray_geometry?normal_matrix4(d.previous_model,d.state.lighting):identity_matrix();
            u.temporal={std::int32_t(d.temporal_token),d.correspondence?1:0,artist_clean(d)?1:0,artist_influence(d.raster)};
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
    void raster_draws(std::size_t first,std::size_t last,bool world,bool artist=false){for(std::size_t i=first;i<last;++i){const auto& d=draws[i];viewport_for(world?world_extent:VkExtent2D{unsigned(width),unsigned(height)},d.viewport);vk.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,raster_pipeline(d,world,artist));vk.vkCmdSetDepthBias(command,d.raster.offset_units,0,d.raster.offset_factor);const std::uint32_t offset=i*uniform_stride;vk.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,draw_layout,0,1,&d.texture->descriptor,1,&offset);VkDeviceSize offsets[2]{};VkBuffer buffers[]={d.mesh->vertices->gpu.buffer,d.previous_vertices->gpu.buffer};vk.vkCmdBindVertexBuffers(command,0,2,buffers,offsets);vk.vkCmdBindIndexBuffer(command,d.mesh->indices->gpu.buffer,0,VK_INDEX_TYPE_UINT32);vk.vkCmdDrawIndexed(command,d.mesh->index_count,1,0,0,0);}}
    void render_world(){
        VulkanImage& color=sample>1?world_ms_color:world_color;VulkanImage& depth=sample>1?world_ms_depth:world_depth;VulkanImage& normal=sample>1?world_ms_normal:world_normal;
        VulkanImage& motion=sample>1?world_ms_motion:world_motion;VulkanImage& previous_normal=sample>1?world_ms_previous_normal:world_previous_normal;
        for(auto* i:{&color,&normal,&motion,&previous_normal,&world_color,&world_normal,&world_motion,&world_previous_normal})transition(command,*i,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);for(auto* i:{&depth,&world_depth})transition(command,*i,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo colors[4]{};for(auto& c:colors){c.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;c.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;c.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;c.storeOp=VK_ATTACHMENT_STORE_OP_STORE;}colors[0].imageView=color.view;colors[0].clearValue.color={{0,0,0,1}};colors[1].imageView=normal.view;colors[1].clearValue.color.int32[3]=-1;
        colors[2].imageView=motion.view;colors[2].clearValue.color.int32[3]=-1;colors[3].imageView=previous_normal.view;
        colors[3].clearValue.color.int32[0]=std::bit_cast<std::int32_t>(std::uint32_t{0x80008000u});
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
        if(sample>1) for(auto* i:{&world_ms_color,&world_ms_normal,&world_ms_motion,&world_ms_previous_normal})transition(command,*i,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if(artist_mask_active)render_artist_mask(depth);
        transition(command,world_depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    }
    void render_artist_mask(VulkanImage& depth){
        // Primary MS depth stays in attachment layout, whose transition helper
        // deliberately early-returns. Order its writes before the replay clear.
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask=barrier.dstStageMask=VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        barrier.srcAccessMask=VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout=barrier.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=depth.image;barrier.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dependency.imageMemoryBarrierCount=1;dependency.pImageMemoryBarriers=&barrier;
        vk.vkCmdPipelineBarrier2(command,&dependency);
        transition(command,world_artist_mask,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView=world_artist_mask.view;color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;color.clearValue.color={{1,1,1,1}};
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView=depth.view;da.imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;da.storeOp=VK_ATTACHMENT_STORE_OP_STORE;da.clearValue.depthStencil={1,0};
        // Replay ALL original packets, including depth-only and RGB-no-op draws.
        // No resolves: the primary MS sample-zero depth/metadata stay untouched.
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea.extent=world_extent;ri.layerCount=1;
        ri.colorAttachmentCount=1;ri.pColorAttachments=&color;ri.pDepthAttachment=&da;
        vk.vkCmdBeginRendering(command,&ri);raster_draws(0,world_end,true,true);vk.vkCmdEndRendering(command);
        transition(command,world_artist_mask,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    void fullscreen(const VulkanImage& source,VulkanImage& target,VkPipeline pipeline,bool copy){const auto copy_set=copy_sets[copy?0:1];transition(command,target,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);VkDescriptorImageInfo ii{frame_sampler,source.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=copy_set;w.dstBinding=0;w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.descriptorCount=1;w.pImageInfo=&ii;vk.vkUpdateDescriptorSets(device,1,&w,0,nullptr);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};color.imageView=target.view;color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;color.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea.extent=target.extent;ri.layerCount=1;ri.colorAttachmentCount=1;ri.pColorAttachments=&color;vk.vkCmdBeginRendering(command,&ri);viewport_for(target.extent,{0,0,width,height});vk.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vk.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,copy_layout,0,1,&copy_set,0,nullptr);struct {float ramp;unsigned copy;}push{float(brightness),unsigned(copy)};vk.vkCmdPushConstants(command,copy_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);vk.vkCmdDraw(command,3,1,0,0);vk.vkCmdEndRendering(command);}
    void render_ui(){
        transition(command,composition,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        transition(command,ui_depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView=composition.view;color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp=world_seen?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;color.clearValue.color={{0,0,0,1}};
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView=ui_depth.view;depth.imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;depth.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;depth.clearValue.depthStencil={1,0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea.extent=composition.extent;ri.layerCount=1;
        ri.colorAttachmentCount=1;ri.pColorAttachments=&color;ri.pDepthAttachment=&depth;
        vk.vkCmdBeginRendering(command,&ri);raster_draws(world_seen?world_end:0,draws.size(),false);vk.vkCmdEndRendering(command);
        transition(command,composition,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    void finish(){if(finished)return;if(width<=0||height<=0||(SDL_GetWindowFlags(window)&SDL_WINDOW_MINIMIZED)){history=false;previous_draws.clear();next_draws.clear();effects->reset_history();finished=true;return;}if(!world_closed)close_world();if(world_seen)world_targets();else{world_end=0;history=false;previous_draws.clear();effects->reset_history();}prepare_correspondence();prepare_artist_mask();update_uniforms();begin(command);
        VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};host.srcStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;host.srcAccessMask=VK_ACCESS_2_HOST_WRITE_BIT;host.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;host.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT;VkDependencyInfo visibility{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};visibility.memoryBarrierCount=1;visibility.pMemoryBarriers=&host;vk.vkCmdPipelineBarrier2(command,&visibility);
        if(world_seen){render_world();if(settings.enhanced){
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
                instance.model=d.state.model;instance.texture_index=texture_index;
                instance.uv_transform=d.state.uv_transform;instance.color=d.state.color;instance.diffuse_light=d.state.diffuse_light;
                instance.alpha_function=d.alpha_function;instance.alpha_reference=d.alpha_reference;instance.alpha_test=d.alpha_function!=0;
                instance.flip_vertical=d.texture->flip;instance.cull=d.raster.cull;instance.cull_face=d.raster.cull_face;
                instance.lighting=d.state.lighting;
                ray_instances.push_back(instance);
            }
            VulkanEffectsFrame frame{command,&world_color,&world_depth,&world_normal,world_extent,{unsigned(width),unsigned(height)},world_view,world_projection,previous_vp,jitter,ray_instances,ray_images,settings,history,&world_motion,&world_previous_normal,sample>1?&world_ms_motion:nullptr,sample>1?&world_ms_normal:nullptr,sample>1?&world_ms_previous_normal:nullptr,artist_mask_active?&world_artist_mask:nullptr,sample>1?&world_ms_color:nullptr};const auto& result=effects->render(frame);fullscreen(result,composition,final_pipeline,true);
        }else fullscreen(world_color,composition,final_pipeline,true);previous_vp=multiply(world_projection,world_view);history=settings.enhanced;++frame_number;}
        render_ui();fullscreen(composition,output,final_pipeline,false);transition(command,output,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);checked(vk.vkEndCommandBuffer(command),"End frame commands");checked(vk.vkResetFences(device,1,&fence),"Reset frame fence");VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=command;VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};si.commandBufferInfoCount=1;si.pCommandBufferInfos=&cb;checked(vk.vkQueueSubmit2(queue,1,&si,fence),"Submit rendered frame");wait();finished=true;
        previous_draws.swap(next_draws);next_draws.clear();
    }
    void present(){finish();if(width<=0||height<=0||(SDL_GetWindowFlags(window)&SDL_WINDOW_MINIMIZED))return;
        std::uint32_t index=0;auto result=vk.vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,acquired,VK_NULL_HANDLE,&index);if(result==VK_ERROR_OUT_OF_DATE_KHR){recreate=true;resize();finished=false;finish();result=vk.vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,acquired,VK_NULL_HANDLE,&index);}if(result!=VK_SUCCESS&&result!=VK_SUBOPTIMAL_KHR)checked(result,"Acquire swapchain image");if(result==VK_SUBOPTIMAL_KHR)recreate=true;
        begin(upload_command);VulkanImage swap{};swap.image=swap_images[index];swap.format=swap_format;swap.extent={unsigned(width),unsigned(height)};transition(upload_command,swap,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.srcOffsets[1]=blit.dstOffsets[1]={width,height,1};vk.vkCmdBlitImage(upload_command,output.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,swap.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);transition(upload_command,swap,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);checked(vk.vkEndCommandBuffer(upload_command),"End presentation transfer");VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=upload_command;VkSemaphoreSubmitInfo waits{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};waits.semaphore=acquired;waits.stageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;VkSemaphoreSubmitInfo signals{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};signals.semaphore=complete[index];signals.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};si.waitSemaphoreInfoCount=1;si.pWaitSemaphoreInfos=&waits;si.commandBufferInfoCount=1;si.pCommandBufferInfos=&cb;si.signalSemaphoreInfoCount=1;si.pSignalSemaphoreInfos=&signals;checked(vk.vkQueueSubmit2(queue,1,&si,VK_NULL_HANDLE),"Submit presentation transfer");VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&complete[index];pi.swapchainCount=1;pi.pSwapchains=&swapchain;pi.pImageIndices=&index;result=vk.vkQueuePresentKHR(queue,&pi);if(result==VK_ERROR_OUT_OF_DATE_KHR||result==VK_SUBOPTIMAL_KHR)recreate=true;else checked(result,"Present Vulkan frame");
        // ponytail: one fully completed frame in flight; split frame resources only if profiling warrants overlap.
        checked(vk.vkQueueWaitIdle(queue),"Wait presentation");
    }
    std::vector<std::uint8_t> capture(){
        finish();if(width<=0||height<=0||(SDL_GetWindowFlags(window)&SDL_WINDOW_MINIMIZED))return {};
        const VkDeviceSize bytes=VkDeviceSize(width)*height*4;
        if(!readback||readback->gpu.size<bytes)readback=buffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,false);
        begin(upload_command);VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={unsigned(width),unsigned(height),1};
        vk.vkCmdCopyImageToBuffer(upload_command,output.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback->gpu.buffer,1,&region);
        VkMemoryBarrier2 host_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};host_read.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;host_read.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;host_read.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;host_read.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo visibility{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};visibility.memoryBarrierCount=1;visibility.pMemoryBarriers=&host_read;vk.vkCmdPipelineBarrier2(upload_command,&visibility);submit_upload();
        std::vector<std::uint8_t> result(std::size_t(width)*height*4);const std::size_t stride=std::size_t(width)*4;
        for(int y=0;y<height;++y)std::memcpy(result.data()+std::size_t(y)*stride,static_cast<std::uint8_t*>(readback->mapped)+std::size_t(height-1-y)*stride,stride);
        return result;
    }
    void set_graphics(const GraphicsSettings& s){
        if(!valid_graphics(s))throw std::runtime_error("Invalid graphics settings");
        wait();
        if(s.fullscreen!=bool(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN))sdl(SDL_SetWindowFullscreen(window,s.fullscreen));
        const bool reset=s.samples!=settings.samples||s.render_scale!=settings.render_scale||s.temporal_aa!=settings.temporal_aa||s.enhanced!=settings.enhanced;
        const bool filtering=std::min(s.anisotropy,max_aniso)!=settings.anisotropy;
        settings=s;settings.anisotropy=std::min(s.anisotropy,max_aniso);select_samples();
        if(filtering)for(auto& t:textures)if(t->mipmaps){
            if(t.use_count()>1){
                auto replacement=texture_resource(t->storage);
                replacement->levels=t->levels;replacement->mipmaps=t->mipmaps;replacement->flip=t->flip;
                replacement->sampler=sampler(true);replacement->descriptor=descriptor(draw_set_layout);
                write_texture_descriptor(*replacement);t=std::move(replacement);
            }else{vk.vkDestroySampler(device,t->sampler,nullptr);t->sampler=sampler(true);write_texture_descriptor(*t);}
        }
        if(reset){for(auto* i:{&world_color,&world_depth,&world_normal,&world_motion,&world_previous_normal,&world_ms_color,&world_ms_depth,&world_ms_normal,&world_ms_motion,&world_ms_previous_normal,&world_artist_mask})destroy_image(*i);history=false;previous_draws.clear();effects->reset_history();}
        resize();
    }
    void cleanup()noexcept{
        if(device&&vk.vkDeviceWaitIdle)vk.vkDeviceWaitIdle(device);
        if(effects){std::cerr<<"Vulkan execution: acceleration builds="<<effects->acceleration_builds()<<", ray-query frames="<<effects->ray_query_frames()<<'\n';effects.reset();}
        draws.clear();previous_draws.clear();next_draws.clear();meshes.clear();textures.clear();white.reset();uniforms.reset();upload_staging.reset();readback.reset();context.reset();
        if(device){destroy_targets();for(auto [key,p]:pipelines)vk.vkDestroyPipeline(device,p,nullptr);if(final_pipeline)vk.vkDestroyPipeline(device,final_pipeline,nullptr);for(auto m:{raster_vert,raster_frag,raster_ui_frag,raster_mask_frag,final_vert,final_frag})if(m)vk.vkDestroyShaderModule(device,m,nullptr);if(frame_sampler)vk.vkDestroySampler(device,frame_sampler,nullptr);if(draw_layout)vk.vkDestroyPipelineLayout(device,draw_layout,nullptr);if(copy_layout)vk.vkDestroyPipelineLayout(device,copy_layout,nullptr);if(descriptor_pool)vk.vkDestroyDescriptorPool(device,descriptor_pool,nullptr);if(draw_set_layout)vk.vkDestroyDescriptorSetLayout(device,draw_set_layout,nullptr);if(copy_set_layout)vk.vkDestroyDescriptorSetLayout(device,copy_set_layout,nullptr);for(auto s:complete)vk.vkDestroySemaphore(device,s,nullptr);if(acquired)vk.vkDestroySemaphore(device,acquired,nullptr);if(fence)vk.vkDestroyFence(device,fence,nullptr);if(pool)vk.vkDestroyCommandPool(device,pool,nullptr);if(swapchain)vk.vkDestroySwapchainKHR(device,swapchain,nullptr);vk.vkDestroyDevice(device,nullptr);device=VK_NULL_HANDLE;}
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
void VulkanRenderer::reset_history(){impl_->history=false;impl_->previous_draws.clear();impl_->effects->reset_history();}
const char* VulkanRenderer::backend_name()const noexcept{return "Vulkan";}
bool VulkanRenderer::ray_tracing_available()const noexcept{return impl_->ray;}
} // namespace yami
