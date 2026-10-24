#include "vulkan_effects.hpp"
#include "yami_vulkan_shaders.hpp"
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <yami_ffx_vulkan.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace yami {
namespace {
void checked(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation)+": Vulkan result "+std::to_string(result));
}
void ffx(FfxErrorCode result, const char* operation) {
    if (result != FFX_OK) throw std::runtime_error(std::string(operation)+": FidelityFX error "+std::to_string(result));
}
#ifndef NDEBUG
void fsr_message(FfxMsgType type, const wchar_t* message) {
    std::string text; for (; *message; ++message) text+=*message<128?char(*message):'?';
    std::cerr<<"FSR 3.1 "<<(type==FFX_MESSAGE_TYPE_ERROR?"error: ":"warning: ")<<text<<'\n';
}
#endif
FfxResource ffx_image(const VulkanImage& image, VkImageUsageFlags usage, FfxResourceStates state) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType=VK_IMAGE_TYPE_2D; info.format=image.format; info.extent={image.extent.width,image.extent.height,1};
    info.mipLevels=1; info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT; info.usage=usage;
    return ffxGetResourceVK(image.image,ffxGetImageResourceDescriptionVK(image.image,info),nullptr,state);
}
// Yaman's skinned mesh spans 13.80 model units x model scale 4.3 = 59.3 world
// units (game/data/models/yaman); a ~1.5 m child protagonist gives ~.025 m/unit.
constexpr float metres_per_unit=.025f;
Mat4 inverse(const Mat4& input) {
    double a[4][8]{};
    for (int r=0;r<4;++r) for (int c=0;c<4;++c) { a[r][c]=input.values[c*4+r]; a[r][c+4]=r==c; }
    for (int c=0;c<4;++c) {
        int pivot=c; for (int r=c+1;r<4;++r) if (std::abs(a[r][c])>std::abs(a[pivot][c])) pivot=r;
        if (std::abs(a[pivot][c])<1e-15) throw std::runtime_error("Vulkan effects: singular camera matrix");
        for (int k=0;k<8;++k) std::swap(a[c][k],a[pivot][k]);
        const double scale=a[c][c]; for (double& v:a[c]) v/=scale;
        for (int r=0;r<4;++r) if (r!=c) { const double f=a[r][c]; for (int k=0;k<8;++k) a[r][k]-=f*a[c][k]; }
    }
    Mat4 result; for (int r=0;r<4;++r) for (int c=0;c<4;++c) result.values[c*4+r]=float(a[r][c+4]);
    return result;
}
bool same(VkExtent2D a,VkExtent2D b) { return a.width==b.width && a.height==b.height; }
struct alignas(16) Parameters {
    Mat4 inverse_vp, current_vp, previous_vp;
    std::array<float,4> camera,extent,jitter,strengths,post,temporal;
    Mat4 inverse_previous_vp;
    // ray_basis_x[3] (rayBasisX.w, otherwise unused): f.fog, the raster fogged the frame's ray surfaces.
    std::array<float,4> ray_basis_x,ray_basis_y,ray_basis_z;
    std::array<float,4> previous_ray_basis_x,previous_ray_basis_y,previous_ray_basis_z;
    std::array<float,4> previous_camera;
};
static_assert(sizeof(Parameters)==464);
bool camera_ray_basis(Parameters& p,const Mat4& view,const Mat4& projection) {
    const auto& v=view.values; const auto& projection_values=projection.values;
    if(v[3]!=0 || v[7]!=0 || v[11]!=0 || v[15]!=1 ||
       projection_values[12]!=0 || projection_values[13]!=0 || projection_values[15]!=0) return false;
    for(float value:v) if(!std::isfinite(value)) return false;
    for(float value:projection_values) if(!std::isfinite(value)) return false;
    for(float value:p.current_vp.values) if(!std::isfinite(value)) return false;
    for(int i=0;i<3;++i) if(!std::isfinite(p.camera[i])) return false;
    // These factors put the ideal camera at zero in clip X/Y/W. Avoid
    // cancelling the rounded VP translation against a large world camera.
    const auto& m=p.current_vp.values;
    const double a[3]{m[0],m[4],m[8]},b[3]{m[1],m[5],m[9]},c[3]{m[3],m[7],m[11]};
    const double columns[3][3]{
        {b[1]*c[2]-b[2]*c[1],b[2]*c[0]-b[0]*c[2],b[0]*c[1]-b[1]*c[0]},
        {c[1]*a[2]-c[2]*a[1],c[2]*a[0]-c[0]*a[2],c[0]*a[1]-c[1]*a[0]},
        {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}};
    const double determinant=a[0]*columns[0][0]+a[1]*columns[0][1]+a[2]*columns[0][2];
    if(!std::isfinite(determinant) || determinant==0) return false;
    std::array<float,4>* basis[3]{&p.ray_basis_x,&p.ray_basis_y,&p.ray_basis_z};
    for(int column=0;column<3;++column) for(int row=0;row<3;++row) {
        const double value=columns[column][row]/determinant;
        if(!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max()) return false;
        (*basis[column])[row]=float(value);
    }
    return true;
}
struct alignas(16) RayMaterial {
    std::uint64_t vertices,indices;
    std::array<float,4> uv,color,diffuse;
    std::array<std::uint32_t,4> material;
    std::array<float,4> misc;
    std::uint64_t previous_vertices,unused;
    std::array<std::array<float,4>,3> previous_model; // object-to-world rows
};
static_assert(sizeof(RayMaterial)==160);
}
struct VulkanEffects::Impl {
    VulkanContext& c;
    struct Buffer { VulkanBuffer gpu; void* mapped=nullptr; };
    struct Structure { VkAccelerationStructureKHR handle=VK_NULL_HANDLE; Buffer storage; VkDeviceAddress address=0; };
    struct Blas {
        Structure as;
        std::uint64_t revision=0;
        std::uint32_t vertices=0,indices=0;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    };
    Buffer uniform,materials,instances,scratch;
    Structure tlas;
    std::uint32_t tlas_count=0;
    std::uint32_t top_size_count=0;
    VkAccelerationStructureBuildSizesInfoKHR top_sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    std::unordered_map<std::uint64_t,Blas> cache;
    // shadow_offset: Sun pass second output (effects.frag shadowOffset), written only for the midpoint selector.
    VulkanImage effects,reflected,sun,shadow_offset,history[2],geometry[2],base[2],reflection[2],output;
    // FSR render-resolution inputs, its shared dilation resources, and display-resolution output.
    // fsr_illumination: reactive-mask history (fsr_inputs.frag), written [fsr_ping], previous [1-fsr_ping].
    // fsr_reactive also serves frame interpolation as its shading-change mask.
    VulkanImage fsr_color,fsr_motion,fsr_reactive,fsr_illumination[2],fsr_dilated_depth,fsr_dilated_motion,fsr_previous_depth,upscaled;
    unsigned fsr_ping=0;
    VkDescriptorSetLayout fsr_inputs_layout=VK_NULL_HANDLE;
    VkPipelineLayout fsr_inputs_pipeline_layout=VK_NULL_HANDLE;
    VkPipeline fsr_inputs_pipeline=VK_NULL_HANDLE;
    VkDescriptorSet fsr_inputs_set=VK_NULL_HANDLE;
    // Temporal filter of the FSR input's per-sample Sun visibility (sun_taa.comp): RG32F (visibility, clip W)
    // history written [sun_ping], previous [1-sun_ping]; valid while the previous frame wrote it, at sun_jitter.
    VulkanImage sun_history[2];
    unsigned sun_ping=0;
    bool sun_history_valid=false;
    std::array<float,2> sun_jitter{};
    VkDescriptorSetLayout sun_taa_layout=VK_NULL_HANDLE;
    VkPipelineLayout sun_taa_pipeline_layout=VK_NULL_HANDLE;
    VkPipeline sun_taa_pipeline=VK_NULL_HANDLE;
    VkDescriptorSet sun_taa_set=VK_NULL_HANDLE;
    std::unique_ptr<FfxFsr3UpscalerContext> fsr;
    std::vector<std::byte> fsr_scratch;
    VkExtent2D fsr_render{},fsr_display{};
    bool fsr_history=false;
    // Frame generation (IKV fsr_vk.cpp Context): optical flow + interpolation on their own backend, the
    // shared-resource context owning the optical-flow vector/scene-change outputs, and per-frame state.
    FfxInterface generation{};
    std::vector<std::byte> generation_scratch;
    std::unique_ptr<FfxOpticalflowContext> flow;
    std::unique_ptr<FfxFrameInterpolationContext> interpolation;
    std::uint32_t flow_effect=0;
    bool flow_effect_created=false;
    std::array<FfxResourceInternal,2> flow_output{{{-1},{-1}}}; // -1: never created, DestroyResource ignores it
    VkFormat generation_format=VK_FORMAT_UNDEFINED;
    // Frame generation's midpoint Sun selector (sun_midpoint.comp): this frame's Sun factor (R32F), the splatted
    // midpoint one (R32UI float bits) and the R8 selector frame interpolation reads, all kept in GENERAL.
    // midpoint: this render() wrote midpoint_select.
    VulkanImage sun_now,sun_mid,midpoint_select;
    VkDescriptorSetLayout midpoint_layout=VK_NULL_HANDLE;
    VkPipelineLayout midpoint_pipeline_layout=VK_NULL_HANDLE;
    std::array<VkPipeline,2> midpoint_pipeline{}; // splat, select
    VkDescriptorSet midpoint_set=VK_NULL_HANDLE;
    bool midpoint=false;
    // ready: this render() upscaled and may interpolate; chain: the previous render() was interpolated.
    bool fsr_reset=true,generation_ready=false,generation_chain=false,generation_continuous=false;
    std::uint64_t generation_frame=0;
    float frame_ms=1000.f/60,camera_near=0,camera_far=0,camera_fov=0;
    std::chrono::steady_clock::time_point fsr_time{};
    VkSampler sampler=VK_NULL_HANDLE,nearest_sampler=VK_NULL_HANDLE;
    VkDescriptorPool pool=VK_NULL_HANDLE,ray_pool=VK_NULL_HANDLE;
    std::uint32_t ray_capacity=0;
    VkDescriptorSetLayout effect_layout=VK_NULL_HANDLE,ray_layout=VK_NULL_HANDLE,temporal_layout=VK_NULL_HANDLE,display_layout=VK_NULL_HANDLE;
    VkPipelineLayout effect_pipeline_layout=VK_NULL_HANDLE,ray_pipeline_layout=VK_NULL_HANDLE,temporal_pipeline_layout=VK_NULL_HANDLE,display_pipeline_layout=VK_NULL_HANDLE;
    VkPipeline effect_pipeline=VK_NULL_HANDLE,ray_pipeline=VK_NULL_HANDLE,temporal_pipeline=VK_NULL_HANDLE,display_pipeline=VK_NULL_HANDLE;
    VkDescriptorSet effect_set=VK_NULL_HANDLE,ray_set=VK_NULL_HANDLE,temporal_set=VK_NULL_HANDLE,display_set=VK_NULL_HANDLE;
    VkDescriptorSetLayout effect_ms_layout=VK_NULL_HANDLE,ray_ms_layout=VK_NULL_HANDLE;
    VkPipelineLayout effect_ms_pipeline_layout=VK_NULL_HANDLE,ray_ms_pipeline_layout=VK_NULL_HANDLE;
    VkPipeline effect_ms_pipeline=VK_NULL_HANDLE,ray_ms_pipeline=VK_NULL_HANDLE;
    VkDescriptorSet effect_ms_set=VK_NULL_HANDLE,ray_ms_set=VK_NULL_HANDLE;
    VkDescriptorSetLayout sun_layout=VK_NULL_HANDLE;
    VkPipelineLayout sun_pipeline_layout=VK_NULL_HANDLE;
    VkPipeline sun_pipeline=VK_NULL_HANDLE;
    VkPipeline classify_pipeline=VK_NULL_HANDLE,classify_ms_pipeline=VK_NULL_HANDLE;
    VkDescriptorSet sun_set=VK_NULL_HANDLE;
    // History-unsafe RT pixels (shaders/effects_rt.glsl): set 1 holds the queue (indirect dispatch/draw
    // arguments) and their state after sample 0, sized for the render extent. effects_rt.comp finishes
    // their samples, effects_resolve draws the results into effects/reflected. Index: ms.
    Buffer queue,pending;
    static constexpr VkDeviceSize pending_stride=96; // std430 sizeof(Pending)
    VkDescriptorSetLayout pending_layout=VK_NULL_HANDLE;
    VkDescriptorPool pending_pool=VK_NULL_HANDLE;
    VkDescriptorSet pending_set=VK_NULL_HANDLE;
    std::array<VkPipeline,2> pending_pipeline{},resolve_pipeline{};
    bool valid=false,clear_cache=false;
    unsigned ping=0;
    std::uint64_t frame_index=0,build_count=0,query_count=0;
    GraphicsSettings previous_settings{};
    Mat4 previous_vp{};
    std::array<float,4> previous_camera{};
    std::array<float,4> previous_ray_basis_x{},previous_ray_basis_y{},previous_ray_basis_z{};

    explicit Impl(VulkanContext& context):c(context) {
        try { initialize(); } catch (...) { cleanup(); throw; }
    }
    ~Impl() { cleanup(); }
    std::uint32_t memory_type(std::uint32_t bits,VkMemoryPropertyFlags properties) {
        for (std::uint32_t i=0;i<c.memory.memoryTypeCount;++i)
            if ((bits&(1u<<i)) && (c.memory.memoryTypes[i].propertyFlags&properties)==properties) return i;
        throw std::runtime_error("Vulkan effects: required GPU memory type unavailable");
    }
    void destroy(Buffer& b) noexcept {
        if(b.mapped) c.vk.vkUnmapMemory(c.device,b.gpu.memory);
        if(b.gpu.buffer) c.vk.vkDestroyBuffer(c.device,b.gpu.buffer,nullptr);
        if(b.gpu.memory) c.vk.vkFreeMemory(c.device,b.gpu.memory,nullptr);
        b={};
    }
    // host: CPU-written and mapped (the effects uniform and ray instance/material tables), preferring mappable VRAM.
    void reserve(Buffer& b,VkDeviceSize size,VkBufferUsageFlags usage,bool host=false) {
        if(b.gpu.size>=size) return;
        destroy(b);
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size=std::max<VkDeviceSize>(size,256); info.usage=usage;
        checked(c.vk.vkCreateBuffer(c.device,&info,nullptr,&b.gpu.buffer),"effects create buffer");
        VkMemoryRequirements requirements{}; c.vk.vkGetBufferMemoryRequirements(c.device,b.gpu.buffer,&requirements);
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags=(usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)?VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT:0;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext=flags.flags?&flags:nullptr; allocation.allocationSize=requirements.size;
        if(host) checked(allocate_host_memory(c,allocation,requirements.memoryTypeBits,host_upload_memory,host_memory,b.gpu.memory),"effects allocate buffer");
        else {
            allocation.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            checked(c.vk.vkAllocateMemory(c.device,&allocation,nullptr,&b.gpu.memory),"effects allocate buffer");
        }
        checked(c.vk.vkBindBufferMemory(c.device,b.gpu.buffer,b.gpu.memory,0),"effects bind buffer");
        b.gpu.size=info.size;
        if(host) checked(c.vk.vkMapMemory(c.device,b.gpu.memory,0,VK_WHOLE_SIZE,0,&b.mapped),"effects map buffer");
        if(flags.flags) { VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; address.buffer=b.gpu.buffer; b.gpu.address=c.vk.vkGetBufferDeviceAddress(c.device,&address); }
    }
    void destroy(Structure& as) noexcept {
        if(as.handle) c.vk.vkDestroyAccelerationStructureKHR(c.device,as.handle,nullptr);
        destroy(as.storage); as={};
    }
    void structure(Structure& as,VkDeviceSize size,VkAccelerationStructureTypeKHR type) {
        if(as.handle && as.storage.gpu.size>=size) return;
        destroy(as);
        reserve(as.storage,size,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        VkAccelerationStructureCreateInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        info.buffer=as.storage.gpu.buffer; info.size=as.storage.gpu.size; info.type=type;
        checked(c.vk.vkCreateAccelerationStructureKHR(c.device,&info,nullptr,&as.handle),"effects create acceleration structure");
        VkAccelerationStructureDeviceAddressInfoKHR address{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR}; address.accelerationStructure=as.handle;
        as.address=c.vk.vkGetAccelerationStructureDeviceAddressKHR(c.device,&address);
    }
    void destroy(VulkanImage& image) noexcept {
        if(image.view) c.vk.vkDestroyImageView(c.device,image.view,nullptr);
        if(image.image) c.vk.vkDestroyImage(c.device,image.image,nullptr);
        if(image.memory) c.vk.vkFreeMemory(c.device,image.memory,nullptr);
        image={};
    }
    void image(VulkanImage& image,VkExtent2D extent,VkFormat format=VK_FORMAT_R16G16B16A16_SFLOAT,
               VkImageUsageFlags usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
        if(image.image && same(image.extent,extent) && image.format==format) return;
        destroy(image);
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType=VK_IMAGE_TYPE_2D; info.format=format; info.extent={extent.width,extent.height,1};
        info.mipLevels=1; info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=usage;
        checked(c.vk.vkCreateImage(c.device,&info,nullptr,&image.image),"effects create image");
        VkMemoryRequirements requirements{}; c.vk.vkGetImageMemoryRequirements(c.device,image.image,&requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize=requirements.size;
        allocation.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checked(c.vk.vkAllocateMemory(c.device,&allocation,nullptr,&image.memory),"effects allocate image");
        checked(c.vk.vkBindImageMemory(c.device,image.image,image.memory,0),"effects bind image");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=image.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D;
        view.format=info.format; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        checked(c.vk.vkCreateImageView(c.device,&view,nullptr,&image.view),"effects create view");
        image.extent=extent; image.format=info.format;
    }
    void transition(VkCommandBuffer cmd,VulkanImage& image,VkImageLayout layout) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask=image.layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_MEMORY_READ_BIT;
        b.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.dstAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_MEMORY_READ_BIT;
        b.oldLayout=image.layout; b.newLayout=layout; b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.image=image.image; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.imageMemoryBarrierCount=1; dependency.pImageMemoryBarriers=&b;
        c.vk.vkCmdPipelineBarrier2(cmd,&dependency); image.layout=layout;
    }
    void initialize_history(VkCommandBuffer cmd,VulkanImage& image) {
        if(image.layout!=VK_IMAGE_LAYOUT_UNDEFINED) return;
        transition(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue zero{}; VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        c.vk.vkCmdClearColorImage(cmd,image.image,image.layout,&zero,1,&range);
        transition(cmd,image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    VkDescriptorSetLayout layout(bool ray,bool temporal,std::uint32_t source_capacity=0,bool display=false,bool ms=false,bool source_cache=true) {
        std::array<VkDescriptorSetLayoutBinding,14> bindings{};
        const unsigned ray_count=ray?(source_cache?2:1):0;
        const unsigned base_count=temporal?13:display?7:ray?8:5,count=base_count+(ms?3:0)+ray_count;
        for(unsigned i=0;i<count;++i) {
            bindings[i].binding=i; bindings[i].descriptorCount=1; bindings[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
            // Ray sets also serve effects_rt.comp and effects_resolve.vert (Parameters).
            if(ray) bindings[i].stageFlags|=VK_SHADER_STAGE_COMPUTE_BIT|(i==3?VK_SHADER_STAGE_VERTEX_BIT:0);
            bindings[i].descriptorType=i==3?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        if(!ray && !temporal && !display) bindings[4].binding=7;
        if(ms) for(unsigned i=base_count;i<base_count+3;++i) bindings[i].binding=8+i-base_count;
        if(ray) {
            bindings[count-ray_count].binding=12;
            if(source_cache) bindings[count-1].binding=13;
            bindings[4].descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            bindings[5].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[6].descriptorCount=source_capacity;
        }
        std::array<VkDescriptorBindingFlags,14> binding_flags{}; if(ray) binding_flags[6]=VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO}; flags.bindingCount=count; flags.pBindingFlags=binding_flags.data();
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; info.bindingCount=count; info.pBindings=bindings.data(); info.pNext=ray?&flags:nullptr;
        VkDescriptorSetLayout result{}; checked(c.vk.vkCreateDescriptorSetLayout(c.device,&info,nullptr,&result),"effects descriptor layout"); return result;
    }
    VkPipelineLayout pipeline_layout(VkDescriptorSetLayout set,VkDescriptorSetLayout second=VK_NULL_HANDLE) {
        const VkDescriptorSetLayout sets[]{set,second};
        VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; info.setLayoutCount=second?2:1; info.pSetLayouts=sets;
        VkPipelineLayout result{}; checked(c.vk.vkCreatePipelineLayout(c.device,&info,nullptr,&result),"effects pipeline layout"); return result;
    }
    VkShaderModule shader(std::span<const std::uint32_t> words) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize=words.size_bytes(); info.pCode=words.data();
        VkShaderModule result{}; checked(c.vk.vkCreateShaderModule(c.device,&info,nullptr,&result),"effects shader module"); return result;
    }
    VkPipeline pipeline(VkPipelineLayout layout,std::span<const std::uint32_t> code,unsigned attachments,VkFormat target_format=VK_FORMAT_R16G16B16A16_SFLOAT,
                        VkFormat second_format=VK_FORMAT_R16G16B16A16_SFLOAT,std::span<const std::uint32_t> vertex_code=spirv::vulkan_final_vert,
                        VkPrimitiveTopology topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                        VkFormat third_format=VK_FORMAT_R32G32B32A32_SFLOAT,VkFormat fourth_format=VK_FORMAT_R16G16B16A16_SFLOAT) {
        VkShaderModule vertex=shader(vertex_code),fragment=VK_NULL_HANDLE;
        VkPipeline result{};
        try {
            fragment=shader(code);
            VkPipelineShaderStageCreateInfo stages[2]{};
            for(int i=0;i<2;++i) { stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT; stages[i].module=i?fragment:vertex; stages[i].pName="main"; }
            VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology=topology;
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount=1; viewport.scissorCount=1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode=VK_POLYGON_MODE_FILL; raster.lineWidth=1;
            VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
            std::array<VkPipelineColorBlendAttachmentState,4> blend{}; for(auto& a:blend) a.colorWriteMask=15;
            VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blending.attachmentCount=attachments; blending.pAttachments=blend.data();
            VkDynamicState dynamics[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount=2; dynamic.pDynamicStates=dynamics;
            std::array<VkFormat,4> formats{target_format,second_format,third_format,fourth_format};
            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.colorAttachmentCount=attachments; rendering.pColorAttachmentFormats=formats.data();
            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; info.pNext=&rendering;
            info.stageCount=2; info.pStages=stages; info.pVertexInputState=&input; info.pInputAssemblyState=&assembly; info.pViewportState=&viewport;
            info.pRasterizationState=&raster; info.pMultisampleState=&samples; info.pColorBlendState=&blending; info.pDynamicState=&dynamic; info.layout=layout;
            checked(c.vk.vkCreateGraphicsPipelines(c.device,VK_NULL_HANDLE,1,&info,nullptr,&result),"effects graphics pipeline");
        } catch (...) { if(fragment) c.vk.vkDestroyShaderModule(c.device,fragment,nullptr); c.vk.vkDestroyShaderModule(c.device,vertex,nullptr); throw; }
        c.vk.vkDestroyShaderModule(c.device,fragment,nullptr); c.vk.vkDestroyShaderModule(c.device,vertex,nullptr); return result;
    }
    VkPipeline compute_pipeline(VkPipelineLayout layout,std::span<const std::uint32_t> code) {
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader(code),"main",nullptr};
        info.layout=layout;
        VkPipeline result{};
        const VkResult created=c.vk.vkCreateComputePipelines(c.device,VK_NULL_HANDLE,1,&info,nullptr,&result);
        c.vk.vkDestroyShaderModule(c.device,info.stage.module,nullptr);
        checked(created,"effects compute pipeline"); return result;
    }
    // Set 1 must name the current pending buffer; reserve() replaces it when the render extent grows.
    void write_pending_set() {
        const VkDescriptorBufferInfo buffers[]{{queue.gpu.buffer,0,VK_WHOLE_SIZE},{pending.gpu.buffer,0,VK_WHOLE_SIZE}};
        std::array<VkWriteDescriptorSet,2> writes{};
        for(std::uint32_t i=0;i<2;++i) {
            writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=pending_set; writes[i].dstBinding=i;
            writes[i].descriptorCount=1; writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo=&buffers[i];
        }
        c.vk.vkUpdateDescriptorSets(c.device,2,writes.data(),0,nullptr);
    }
    VkDescriptorSet descriptor(VkDescriptorSetLayout layout,VkDescriptorPool source_pool) {
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; info.descriptorPool=source_pool; info.descriptorSetCount=1; info.pSetLayouts=&layout;
        VkDescriptorSet set{}; checked(c.vk.vkAllocateDescriptorSets(c.device,&info,&set),"effects allocate descriptor"); return set;
    }
    void initialize() {
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; info.magFilter=VK_FILTER_LINEAR; info.minFilter=VK_FILTER_LINEAR;
        info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST; info.addressModeU=info.addressModeV=info.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; info.maxLod=0;
        checked(c.vk.vkCreateSampler(c.device,&info,nullptr,&sampler),"effects sampler");
        info.magFilter=info.minFilter=VK_FILTER_NEAREST;
        checked(c.vk.vkCreateSampler(c.device,&info,nullptr,&nearest_sampler),"effects depth sampler");
        reserve(uniform,sizeof(Parameters),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,true);
        std::array<VkDescriptorPoolSize,3> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,48},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,7},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,4}}};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool_info.maxSets=7; pool_info.poolSizeCount=std::uint32_t(sizes.size()); pool_info.pPoolSizes=sizes.data();
        checked(c.vk.vkCreateDescriptorPool(c.device,&pool_info,nullptr,&pool),"effects descriptor pool");
        effect_layout=layout(false,false); temporal_layout=layout(false,true); display_layout=layout(false,false,0,true);
        effect_ms_layout=layout(false,false,0,false,true);
        effect_pipeline_layout=pipeline_layout(effect_layout); temporal_pipeline_layout=pipeline_layout(temporal_layout); display_pipeline_layout=pipeline_layout(display_layout);
        effect_ms_pipeline_layout=pipeline_layout(effect_ms_layout);
        effect_pipeline=pipeline(effect_pipeline_layout,spirv::effects_frag,2);
        effect_ms_pipeline=pipeline(effect_ms_pipeline_layout,spirv::effects_ms_frag,2);
        temporal_pipeline=pipeline(temporal_pipeline_layout,spirv::temporal_frag,4,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R32G32B32A32_SINT);
        display_pipeline=pipeline(display_pipeline_layout,spirv::display_frag,1);
        effect_set=descriptor(effect_layout,pool); temporal_set=descriptor(temporal_layout,pool); display_set=descriptor(display_layout,pool);
        effect_ms_set=descriptor(effect_ms_layout,pool);
        // FSR inputs: raster colour, depth and motion metadata plus Parameters (binding 3), the
        // current temporal gain/geometry/reflection and Sun that compose the lit radiance (4..7),
        // the previous reactive-mask history (8) and the filtered per-sample Sun (9).
        std::array<VkDescriptorSetLayoutBinding,10> inputs{};
        for(std::uint32_t i=0;i<10;++i) inputs[i]={i,i==3?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo inputs_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; inputs_info.bindingCount=10; inputs_info.pBindings=inputs.data();
        checked(c.vk.vkCreateDescriptorSetLayout(c.device,&inputs_info,nullptr,&fsr_inputs_layout),"FSR inputs descriptor layout");
        fsr_inputs_pipeline_layout=pipeline_layout(fsr_inputs_layout);
        // Lit colour, UV motion, reactive history (log2 illumination, linear depth, log2 artwork luminance: half
        // keeps clip W to 2^-11 relative, far under the 2% same-surface test, up to 65504 > the 30000 far plane;
        // 8 bytes/texel like the old RG32F) and the R8 reactive mask.
        fsr_inputs_pipeline=pipeline(fsr_inputs_pipeline_layout,spirv::fsr_inputs_frag,4,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16G16_SFLOAT,
            spirv::vulkan_final_vert,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R8_UNORM);
        fsr_inputs_set=descriptor(fsr_inputs_layout,pool);
        // Sun TAA: Sun pass output, depth, motion metadata (samplers), Parameters (3), previous history (4) and
        // this frame's history (5, storage); previous jitter and history validity as push constants.
        std::array<VkDescriptorSetLayoutBinding,6> sun_taa_bindings{};
        for(std::uint32_t i=0;i<6;++i)
            sun_taa_bindings[i]={i,i==3?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:i==5?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        const VkDescriptorSetLayoutCreateInfo sun_taa_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,6,sun_taa_bindings.data()};
        checked(c.vk.vkCreateDescriptorSetLayout(c.device,&sun_taa_info,nullptr,&sun_taa_layout),"Sun TAA descriptor layout");
        const VkPushConstantRange sun_taa_push{VK_SHADER_STAGE_COMPUTE_BIT,0,3*sizeof(float)};
        const VkPipelineLayoutCreateInfo sun_taa_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,1,&sun_taa_layout,1,&sun_taa_push};
        checked(c.vk.vkCreatePipelineLayout(c.device,&sun_taa_layout_info,nullptr,&sun_taa_pipeline_layout),"Sun TAA pipeline layout");
        sun_taa_pipeline=compute_pipeline(sun_taa_pipeline_layout,spirv::sun_taa_comp);
        sun_taa_set=descriptor(sun_taa_layout,pool);
        // Midpoint Sun selector (sun_midpoint.comp, both dispatches): gain, geometry, raster colour, Parameters (3),
        // Sun, shadow offset and reflection (8) sampled; sun_now, sun_mid and the selector (6, 7, 9) as storage.
        // Frame generation only: the selector is written formatless, a feature the device enables only for FSR.
        if(c.fsr) {
            std::array<VkDescriptorSetLayoutBinding,10> midpoint_bindings{};
            for(std::uint32_t i=0;i<10;++i)
                midpoint_bindings[i]={i,i==3?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:i==6||i==7||i==9?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            const VkDescriptorSetLayoutCreateInfo midpoint_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,10,midpoint_bindings.data()};
            checked(c.vk.vkCreateDescriptorSetLayout(c.device,&midpoint_info,nullptr,&midpoint_layout),"midpoint Sun descriptor layout");
            midpoint_pipeline_layout=pipeline_layout(midpoint_layout);
            midpoint_pipeline[0]=compute_pipeline(midpoint_pipeline_layout,spirv::sun_midpoint_comp);
            midpoint_pipeline[1]=compute_pipeline(midpoint_pipeline_layout,spirv::sun_midpoint_select_comp);
            midpoint_set=descriptor(midpoint_layout,pool);
        }
        const auto pending_stages=VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT;
        const std::array<VkDescriptorSetLayoutBinding,2> pending_bindings{{{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,pending_stages,nullptr},
            {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,pending_stages,nullptr}}};
        VkDescriptorSetLayoutCreateInfo pending_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; pending_info.bindingCount=2; pending_info.pBindings=pending_bindings.data();
        checked(c.vk.vkCreateDescriptorSetLayout(c.device,&pending_info,nullptr,&pending_layout),"effects pending descriptor layout");
        const VkDescriptorPoolSize pending_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
        VkDescriptorPoolCreateInfo pending_pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pending_pool_info.maxSets=1; pending_pool_info.poolSizeCount=1; pending_pool_info.pPoolSizes=&pending_size;
        checked(c.vk.vkCreateDescriptorPool(c.device,&pending_pool_info,nullptr,&pending_pool),"effects pending descriptor pool");
        pending_set=descriptor(pending_layout,pending_pool);
        reserve(queue,7*sizeof(std::uint32_t),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        reserve(pending,1,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        write_pending_set();
        if(c.fsr && !yami_ffx_load_vulkan(c.vk.vkGetInstanceProcAddr,c.instance)) {
            std::cerr<<"FSR 3.1 unavailable: Vulkan instance functions missing\n"; c.fsr=false;
        }
    }
    void destroy_fsr() noexcept {
        // No device wait: every call follows the core's previous-frame fence (class contract),
        // and a device-wide wait would race the renderer's frame-generation present thread.
        if(flow_effect_created) {
            for(auto& r:flow_output) generation.fpDestroyResource(&generation,r,flow_effect);
            generation.fpDestroyBackendContext(&generation,flow_effect);
            flow_effect_created=false; flow_output={{{-1},{-1}}};
        }
        if(interpolation) {
            if(const FfxErrorCode result=ffxFrameInterpolationContextDestroy(interpolation.get()); result!=FFX_OK) std::cerr<<"Frame interpolation destroy: FidelityFX error "<<result<<'\n';
            interpolation.reset();
        }
        if(flow) {
            if(const FfxErrorCode result=ffxOpticalflowContextDestroy(flow.get()); result!=FFX_OK) std::cerr<<"Optical flow destroy: FidelityFX error "<<result<<'\n';
            flow.reset();
        }
        if(fsr) {
            if(const FfxErrorCode result=ffxFsr3UpscalerContextDestroy(fsr.get()); result!=FFX_OK) std::cerr<<"FSR context destroy: FidelityFX error "<<result<<'\n';
            fsr.reset();
        }
        for(auto* i:{&fsr_color,&fsr_motion,&fsr_reactive,&fsr_illumination[0],&fsr_illumination[1],&fsr_dilated_depth,&fsr_dilated_motion,&fsr_previous_depth,&upscaled,
                     &sun_now,&sun_mid,&midpoint_select,&sun_history[0],&sun_history[1]}) destroy(*i);
        fsr_history=sun_history_valid=false; generation_format=VK_FORMAT_UNDEFINED; generation_ready=generation_chain=midpoint=false; generation_frame=0;
    }
    FfxInterface backend(std::vector<std::byte>& scratch,VkDeviceContext& device,std::size_t contexts) {
        // Zeroed scratch: the VK backend refuses a nonzero stale reference count.
        scratch.assign(ffxGetScratchMemorySizeVK(c.physical,contexts),std::byte{0});
        FfxInterface result{};
        ffx(ffxGetInterfaceVK(&result,ffxGetDeviceVK(&device),scratch.data(),scratch.size(),contexts),"FSR Vulkan backend");
        yami_ffx_clamp_capabilities(result);
        return result;
    }
    // generation_format != UNDEFINED also creates FSR 3.1.4 frame generation (IKV fsr_vk.cpp:155-191): optical flow
    // and frame interpolation at display size on a second backend, plus a shared-resource context owning the
    // optical-flow outputs. Interpolation reuses this upscaler's dilated depth/motion and previous depth.
    void create_fsr(VkCommandBuffer cmd,VkExtent2D render,VkExtent2D display,VkFormat generation_format_request) {
        destroy_fsr();
        // The backend keeps this pointer until each effect's backend context exists.
        VkDeviceContext device{c.device,c.physical,yami_ffx_device_proc};
        FfxFsr3UpscalerContextDescription description{};
        description.backendInterface=backend(fsr_scratch,device,FFX_FSR3UPSCALER_CONTEXT_COUNT);
        description.maxRenderSize={render.width,render.height}; description.maxUpscaleSize={display.width,display.height};
        // FSR resolves the composed lit HDR radiance; tone mapping and bloom follow it.
        description.flags=FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
#ifndef NDEBUG
        description.flags|=FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING; description.fpMessage=fsr_message;
#endif
        auto context=std::make_unique<FfxFsr3UpscalerContext>();
        ffx(ffxFsr3UpscalerContextCreate(context.get(),&description),"FSR context");
        fsr=std::move(context); fsr_render=render; fsr_display=display;
        // FSR clears its shared resources with vkCmdClearColorImage, so they need TRANSFER_DST.
        constexpr auto storage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image(fsr_color,render); image(fsr_motion,render,VK_FORMAT_R16G16_SFLOAT); image(fsr_reactive,render,VK_FORMAT_R8_UNORM);
        // Both history images start cleared and shader-readable: the first frame binds the unwritten one.
        for(auto& i:fsr_illumination) { image(i,render,VK_FORMAT_R16G16B16A16_SFLOAT); initialize_history(cmd,i); }
        // Sun TAA history, likewise (RG32F: clip W stays exact at any range, and R32G32_SFLOAT storage is core
        // Vulkan while R16G16 storage needs an optional feature).
        for(auto& i:sun_history) { image(i,render,VK_FORMAT_R32G32_SFLOAT,storage); initialize_history(cmd,i); }
        image(fsr_dilated_depth,render,VK_FORMAT_R32_SFLOAT,storage); image(fsr_dilated_motion,render,VK_FORMAT_R16G16_SFLOAT,storage);
        image(fsr_previous_depth,render,VK_FORMAT_R32_UINT,storage); image(upscaled,display,VK_FORMAT_R16G16B16A16_SFLOAT,storage);
        // Shared FSR resources are declared UNORDERED_ACCESS (GENERAL) and returned there every dispatch.
        for(auto* i:{&fsr_dilated_depth,&fsr_dilated_motion,&fsr_previous_depth}) transition(cmd,*i,VK_IMAGE_LAYOUT_GENERAL);
        if(generation_format_request==VK_FORMAT_UNDEFINED) return;
        // ponytail: the SDK's optical-flow/interpolation create has no release path on failure (upstream); the error ends the session like any FSR failure.
        generation=backend(generation_scratch,device,3);
        FfxOpticalflowContextDescription flow_description{};
        flow_description.backendInterface=generation; flow_description.resolution={display.width,display.height};
        auto flow_context=std::make_unique<FfxOpticalflowContext>();
        ffx(ffxOpticalflowContextCreate(flow_context.get(),&flow_description),"optical flow context");
        flow=std::move(flow_context);
        FfxFrameInterpolationContextDescription interpolation_description{};
        interpolation_description.backendInterface=generation;
        interpolation_description.maxRenderSize={render.width,render.height}; interpolation_description.displaySize={display.width,display.height};
        interpolation_description.backBufferFormat=interpolation_description.previousInterpolationSourceFormat=ffxGetSurfaceFormatVK(generation_format_request);
        auto interpolation_context=std::make_unique<FfxFrameInterpolationContext>();
        ffx(ffxFrameInterpolationContextCreate(interpolation_context.get(),&interpolation_description),"frame interpolation context");
        interpolation=std::move(interpolation_context);
        ffx(generation.fpCreateBackendContext(&generation,FFX_EFFECT_SHAREDRESOURCES,nullptr,&flow_effect),"optical flow shared-resource context");
        flow_effect_created=true;
        FfxOpticalflowSharedResourceDescriptions outputs{};
        ffx(ffxOpticalflowGetSharedResourceDescriptions(flow.get(),&outputs),"optical flow resource descriptions");
        ffx(generation.fpCreateResource(&generation,&outputs.opticalFlowVector,flow_effect,&flow_output[0]),"optical flow vector");
        ffx(generation.fpCreateResource(&generation,&outputs.opticalFlowSCD,flow_effect,&flow_output[1]),"optical flow scene change");
        generation_format=generation_format_request;
    }
    void destroy_ray_pipeline() noexcept {
        if(ray_pipeline) c.vk.vkDestroyPipeline(c.device,ray_pipeline,nullptr);
        if(ray_ms_pipeline) c.vk.vkDestroyPipeline(c.device,ray_ms_pipeline,nullptr);
        if(sun_pipeline) c.vk.vkDestroyPipeline(c.device,sun_pipeline,nullptr);
        if(classify_pipeline) c.vk.vkDestroyPipeline(c.device,classify_pipeline,nullptr);
        if(classify_ms_pipeline) c.vk.vkDestroyPipeline(c.device,classify_ms_pipeline,nullptr);
        for(auto& p:{&pending_pipeline,&resolve_pipeline}) for(auto& pipeline:*p) {
            if(pipeline) c.vk.vkDestroyPipeline(c.device,pipeline,nullptr);
            pipeline=VK_NULL_HANDLE;
        }
        if(ray_pipeline_layout) c.vk.vkDestroyPipelineLayout(c.device,ray_pipeline_layout,nullptr);
        if(ray_ms_pipeline_layout) c.vk.vkDestroyPipelineLayout(c.device,ray_ms_pipeline_layout,nullptr);
        if(sun_pipeline_layout) c.vk.vkDestroyPipelineLayout(c.device,sun_pipeline_layout,nullptr);
        if(ray_pool) c.vk.vkDestroyDescriptorPool(c.device,ray_pool,nullptr);
        if(ray_layout) c.vk.vkDestroyDescriptorSetLayout(c.device,ray_layout,nullptr);
        if(ray_ms_layout) c.vk.vkDestroyDescriptorSetLayout(c.device,ray_ms_layout,nullptr);
        if(sun_layout) c.vk.vkDestroyDescriptorSetLayout(c.device,sun_layout,nullptr);
        ray_pipeline=VK_NULL_HANDLE; ray_pipeline_layout=VK_NULL_HANDLE;
        ray_pool=VK_NULL_HANDLE; ray_layout=VK_NULL_HANDLE; ray_set=VK_NULL_HANDLE;
        ray_ms_pipeline=VK_NULL_HANDLE; ray_ms_pipeline_layout=VK_NULL_HANDLE;
        ray_ms_layout=VK_NULL_HANDLE; ray_ms_set=VK_NULL_HANDLE;
        sun_pipeline=VK_NULL_HANDLE; sun_pipeline_layout=VK_NULL_HANDLE;
        sun_layout=VK_NULL_HANDLE; sun_set=VK_NULL_HANDLE;
        classify_pipeline=VK_NULL_HANDLE; classify_ms_pipeline=VK_NULL_HANDLE;
        ray_capacity=0;
    }
    void ensure_ray_capacity(std::size_t count) {
        if(count<=ray_capacity) return;
        const auto& limits=c.properties.limits;
        const auto available=[](std::uint32_t limit,std::uint32_t fixed) { return limit>fixed?limit-fixed:0u; };
        const auto maximum=std::min({available(limits.maxPerStageDescriptorSamplers,10),
            available(limits.maxDescriptorSetSamplers,10),available(limits.maxPerStageDescriptorSampledImages,10),
            available(limits.maxDescriptorSetSampledImages,10),available(limits.maxPerStageResources,15)});
        if(count>maximum) throw std::runtime_error("Vulkan ray-query artwork table requires "+std::to_string(count)+
            " textures, but this device's descriptor limits allow "+std::to_string(maximum));
        auto capacity=std::max(ray_capacity,1u);
        while(capacity<count) capacity=capacity<=maximum/2?capacity*2:maximum;
        // The renderer completed the previous frame before entering this pass.
        // Grow only before recording any effects/AS commands; unrelated temporal
        // descriptor sets stay alive in their own pool.
        destroy_ray_pipeline();
        std::array<VkDescriptorPoolSize,4> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,capacity*3+23},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,3},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3},
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,3}}};
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets=3; info.poolSizeCount=std::uint32_t(sizes.size()); info.pPoolSizes=sizes.data();
        checked(c.vk.vkCreateDescriptorPool(c.device,&info,nullptr,&ray_pool),"effects grow ray descriptor pool");
        ray_layout=layout(true,false,capacity); ray_pipeline_layout=pipeline_layout(ray_layout,pending_layout);
        ray_pipeline=pipeline(ray_pipeline_layout,spirv::effects_rt_frag,2);
        ray_ms_layout=layout(true,false,capacity,false,true);
        ray_ms_pipeline_layout=pipeline_layout(ray_ms_layout,pending_layout);
        ray_ms_pipeline=pipeline(ray_ms_pipeline_layout,spirv::effects_rt_ms_frag,2);
        for(unsigned ms=0;ms<2;++ms) {
            const VkPipelineLayout ray_pass=ms?ray_ms_pipeline_layout:ray_pipeline_layout;
            pending_pipeline[ms]=compute_pipeline(ray_pass,spirv::effects_rt_comp);
            resolve_pipeline[ms]=pipeline(ray_pass,spirv::effects_resolve_frag,2,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT,
                spirv::effects_resolve_vert,VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
        }
        ray_ms_set=descriptor(ray_ms_layout,ray_pool);
        classify_pipeline=pipeline(ray_pipeline_layout,spirv::effects_classify_frag,1,VK_FORMAT_R8G8B8A8_UNORM);
        classify_ms_pipeline=pipeline(ray_ms_pipeline_layout,spirv::effects_classify_ms_frag,1,VK_FORMAT_R8G8B8A8_UNORM);
        sun_layout=layout(true,false,capacity,false,false,false);
        sun_pipeline_layout=pipeline_layout(sun_layout);
        // Sun visibility and, for frame generation's midpoint selector, the shadow offset (RGBA16F).
        sun_pipeline=pipeline(sun_pipeline_layout,spirv::effects_sun_frag,2,VK_FORMAT_R8G8B8A8_UNORM);
        sun_set=descriptor(sun_layout,ray_pool);
        ray_set=descriptor(ray_layout,ray_pool); ray_capacity=capacity;
    }
    void memory_barrier(VkCommandBuffer cmd,VkPipelineStageFlags2 src,VkAccessFlags2 src_access,VkPipelineStageFlags2 dst,VkAccessFlags2 dst_access) {
        const VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,nullptr,src,src_access,dst,dst_access};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.memoryBarrierCount=1; dependency.pMemoryBarriers=&b; c.vk.vkCmdPipelineBarrier2(cmd,&dependency);
    }
    // trace: the TLAS is read by effects fragment passes and effects_rt.comp.
    void acceleration_barrier(VkCommandBuffer cmd,bool trace=false) {
        memory_barrier(cmd,VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            trace?VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR|(trace?0:VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR));
    }
    VkDeviceAddress scratch_address(VkDeviceSize needed) {
        const auto alignment=std::max<std::uint32_t>(c.scratch_alignment,1);
        reserve(scratch,needed+alignment,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        return (scratch.gpu.address+alignment-1)/alignment*alignment;
    }
    void build_scene(const VulkanEffectsFrame& f) {
        // Determine the largest scratch requirement BEFORE recording any build:
        // growing it afterward would destroy storage referenced by earlier commands.
        VkDeviceSize needed=0;
        for(const auto& instance:f.instances) {
            const auto& mesh=instance.mesh;
            if(!mesh.vertex_count || mesh.index_count<3 || !mesh.vertices.address || !mesh.indices.address) throw std::runtime_error("Vulkan ray-query invalid mesh");
            auto& blas=cache[mesh.key];
            if(blas.vertices!=mesh.vertex_count || blas.indices!=mesh.index_count) {
                destroy(blas.as);
                VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR}; geometry.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                auto& triangles=geometry.geometry.triangles; triangles.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                triangles.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT; triangles.vertexStride=sizeof(Vertex); triangles.maxVertex=mesh.vertex_count-1; triangles.indexType=VK_INDEX_TYPE_UINT32;
                VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR}; build.type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                build.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR|VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR; build.geometryCount=1; build.pGeometries=&geometry;
                std::uint32_t primitives=mesh.index_count/3;
                c.vk.vkGetAccelerationStructureBuildSizesKHR(c.device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&build,&primitives,&blas.sizes);
                blas.vertices=mesh.vertex_count; blas.indices=mesh.index_count;
            }
            needed=std::max({needed,blas.sizes.buildScratchSize,blas.sizes.updateScratchSize});
        }
        reserve(instances,std::max<std::size_t>(f.instances.size(),1)*sizeof(VkAccelerationStructureInstanceKHR),VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,true);
        reserve(materials,std::max<std::size_t>(f.instances.size(),1)*sizeof(RayMaterial),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true);
        VkAccelerationStructureGeometryKHR top_geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR}; top_geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
        top_geometry.geometry.instances.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR; top_geometry.geometry.instances.data.deviceAddress=instances.gpu.address;
        VkAccelerationStructureBuildGeometryInfoKHR top_build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR}; top_build.type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        top_build.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR|VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR; top_build.geometryCount=1; top_build.pGeometries=&top_geometry;
        std::uint32_t count=std::uint32_t(f.instances.size());
        if(top_size_count!=count) {
            c.vk.vkGetAccelerationStructureBuildSizesKHR(c.device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&top_build,&count,&top_sizes);
            top_size_count=count;
        }
        needed=std::max({needed,top_sizes.buildScratchSize,top_sizes.updateScratchSize});
        const VkDeviceAddress scratch_at=scratch_address(needed);
        std::uint32_t instance_index=0;
        auto* gpu_instances=static_cast<VkAccelerationStructureInstanceKHR*>(instances.mapped);
        auto* gpu_materials=static_cast<RayMaterial*>(materials.mapped);
        for(const auto& instance:f.instances) {
            const auto& mesh=instance.mesh;
            auto& blas=cache[mesh.key];
            if(!blas.as.handle || blas.revision!=mesh.revision || blas.vertices!=mesh.vertex_count || blas.indices!=mesh.index_count) {
                VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR}; geometry.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                auto& triangles=geometry.geometry.triangles; triangles.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                triangles.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT; triangles.vertexData.deviceAddress=mesh.vertices.address;
                triangles.vertexStride=sizeof(Vertex); triangles.maxVertex=mesh.vertex_count-1; triangles.indexType=VK_INDEX_TYPE_UINT32; triangles.indexData.deviceAddress=mesh.indices.address;
                VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR}; build.type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                build.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR|VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR; build.geometryCount=1; build.pGeometries=&geometry;
                std::uint32_t primitives=mesh.index_count/3;
                bool update=blas.as.handle!=VK_NULL_HANDLE;
                structure(blas.as,blas.sizes.accelerationStructureSize,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
                build.mode=update?VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR:VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
                build.srcAccelerationStructure=update?blas.as.handle:VK_NULL_HANDLE; build.dstAccelerationStructure=blas.as.handle; build.scratchData.deviceAddress=scratch_at;
                VkAccelerationStructureBuildRangeInfoKHR range{}; range.primitiveCount=primitives; const auto* ranges=&range;
                c.vk.vkCmdBuildAccelerationStructuresKHR(f.command,1,&build,&ranges); ++build_count; ++(update?c.blas_updates:c.blas_builds); acceleration_barrier(f.command);
                blas.revision=mesh.revision; blas.vertices=mesh.vertex_count; blas.indices=mesh.index_count;
            }
            if(instance.texture_index>=f.textures.size()) throw std::runtime_error("Vulkan ray-query missing material texture");
            VkAccelerationStructureInstanceKHR gpu{};
            for(int r=0;r<3;++r) for(int col=0;col<4;++col) gpu.transform.matrix[r][col]=instance.model.values[col*4+r];
            // Every ray culls back faces (effects.frag): exported GL_BACK culling keeps the instance facing, GL_FRONT
            // reverses it, no culling disables it, and GL_FRONT_AND_BACK is never hit (mask 0). Facing culling and
            // rayQueryGetIntersectionFrontFaceEXT share one determination, so hits match the former candidate-loop test.
            const bool cull_back=instance.cull && instance.cull_face==0x405,cull_front=instance.cull && instance.cull_face==0x404;
            gpu.instanceCustomIndex=instance_index; gpu.mask=instance.cull && instance.cull_face==0x408?0:255;
            // Only alpha-tested instances need the candidate shader; the rest are opaque.
            gpu.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR|(cull_back||cull_front?0:VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR)|
                (instance.alpha_test?0:VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR); gpu.accelerationStructureReference=blas.as.address;
            // Original GL_CW / Vulkan CW after projection Y-flip is opposite
            // ray-space default facing. Mirrored models reverse that base flip.
            const auto& m=instance.model.values;
            const float determinant=m[0]*(m[5]*m[10]-m[9]*m[6])-
                m[4]*(m[1]*m[10]-m[9]*m[2])+m[8]*(m[1]*m[6]-m[5]*m[2]);
            if((determinant<0)!=cull_front) gpu.flags^=VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
            gpu_instances[instance_index]=gpu;
            gpu_materials[instance_index]={mesh.vertices.address,mesh.indices.address,instance.uv_transform,instance.color,instance.diffuse_light,
                {instance.texture_index,instance.alpha_function,std::uint32_t(instance.alpha_test),std::uint32_t(instance.flip_vertical)},
                {instance.alpha_reference,float(instance.cull),float(instance.cull_face),float(instance.lighting)},instance.previous_vertices.address,0,{}};
            for(int r=0;r<3;++r) for(int col=0;col<4;++col) gpu_materials[instance_index].previous_model[r][col]=instance.previous_model.values[col*4+r];
            ++instance_index;
        }
        bool update=tlas.handle && tlas_count==count && tlas.storage.gpu.size>=top_sizes.accelerationStructureSize;
        structure(tlas,top_sizes.accelerationStructureSize,VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
        top_build.mode=update?VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR:VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        top_build.srcAccelerationStructure=update?tlas.handle:VK_NULL_HANDLE; top_build.dstAccelerationStructure=tlas.handle; top_build.scratchData.deviceAddress=scratch_at;
        VkAccelerationStructureBuildRangeInfoKHR range{}; range.primitiveCount=count; const auto* ranges=&range;
        c.vk.vkCmdBuildAccelerationStructuresKHR(f.command,1,&top_build,&ranges); ++build_count; ++(update?c.tlas_updates:c.tlas_builds); tlas_count=count; acceleration_barrier(f.command,true);
    }
    void descriptors(const VulkanEffectsFrame& f,VkDescriptorSet set,bool ray,bool temporal,unsigned old,bool display=false,bool ms=false,bool sun_pass=false,bool sun_active=false,bool upscaled_base=false) {
        // Each pass owns a descriptor set: recorded temporal descriptors must
        // never be overwritten with display inputs before command submission.
        std::array<VkDescriptorImageInfo,14> images{};
        const auto source=[&](unsigned binding,const VulkanImage& image,VkSampler filter) {
            images[binding]={filter,image.view,image.layout};
        };
        if(display) {
            source(0,upscaled_base?upscaled:base[old],nearest_sampler); source(1,history[old],nearest_sampler);
            source(2,reflection[old],nearest_sampler); source(4,*f.color,sampler);
            source(5,geometry[old],nearest_sampler);
            source(6,sun_active?sun:effects,nearest_sampler);
        } else {
            source(0,temporal?effects:sun_pass?reflected:*f.color,nearest_sampler);
            source(1,*f.depth,nearest_sampler); source(2,*f.normal,nearest_sampler);
            if(temporal) {
                source(4,history[old],nearest_sampler); source(5,geometry[old],nearest_sampler);
                source(6,*f.color,nearest_sampler); source(7,reflected,nearest_sampler);
                source(8,base[old],nearest_sampler); source(9,reflection[old],nearest_sampler);
                // Optional metadata supports direct effects fixtures/static
                // callers; temporal.w disables integer reads when absent.
                source(10,f.motion?*f.motion:*f.normal,nearest_sampler);
                source(11,f.previous_normal?*f.previous_normal:*f.normal,nearest_sampler);
                // Disabled Sun still needs a floating sampled-image descriptor.
                source(12,sun_active?sun:effects,nearest_sampler);
            } else {
                source(7,f.previous_normal?*f.previous_normal:*f.normal,nearest_sampler);
                if(ms) {
                    source(8,*f.ms_motion,nearest_sampler); source(9,*f.ms_normal,nearest_sampler);
                    source(10,*f.ms_previous_normal,nearest_sampler);
                }
                if(ray) {
                    source(12,f.motion?*f.motion:*f.normal,nearest_sampler);
                    if(!sun_pass) images[13]={nearest_sampler,sun.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                }
            }
        }
        VkDescriptorBufferInfo ubo{uniform.gpu.buffer,0,sizeof(Parameters)},table{materials.gpu.buffer,0,materials.gpu.size};
        std::array<VkWriteDescriptorSet,14> writes{};
        const unsigned ray_count=ray?(sun_pass?1:2):0;
        const unsigned base_count=temporal?13:display?7:ray?8:5,count=base_count+(ms?3:0)+ray_count;
        for(unsigned i=0;i<count;++i) {
            writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=set;
            writes[i].dstBinding=ray&&i>=count-ray_count?12+i-(count-ray_count):ms&&i>=base_count&&i<base_count+3?8+i-base_count:(!ray&&!temporal&&!display&&i==4)?7:i; writes[i].descriptorCount=1;
            writes[i].descriptorType=i==3?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            if(i==3) writes[i].pBufferInfo=&ubo;
            else writes[i].pImageInfo=&images[writes[i].dstBinding];
        }
        VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR}; as.accelerationStructureCount=1; as.pAccelerationStructures=&tlas.handle;
        if(ray) {
            writes[4].descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; writes[4].pImageInfo=nullptr; writes[4].pNext=&as;
            writes[5].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[5].pImageInfo=nullptr; writes[5].pBufferInfo=&table;
            writes[6].descriptorCount=std::uint32_t(f.textures.size()); writes[6].pImageInfo=f.textures.data();
        }
        c.vk.vkUpdateDescriptorSets(c.device,count,writes.data(),0,nullptr);
    }
    // indirect: draw effects_resolve points from the pending queue over the loaded targets instead of a full-screen triangle.
    void pass(VkCommandBuffer cmd,std::span<VulkanImage*> targets,VkPipeline pipeline,VkPipelineLayout layout,VkDescriptorSet set,VkBuffer indirect=VK_NULL_HANDLE) {
        std::array<VkRenderingAttachmentInfo,4> attachments{};
        for(std::size_t i=0;i<targets.size();++i) {
            transition(cmd,*targets[i],VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            attachments[i].sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; attachments[i].imageView=targets[i]->view;
            attachments[i].imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; attachments[i].loadOp=indirect?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachments[i].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkExtent2D extent=targets[0]->extent;
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO}; info.renderArea.extent=extent; info.layerCount=1; info.colorAttachmentCount=std::uint32_t(targets.size()); info.pColorAttachments=attachments.data();
        c.vk.vkCmdBeginRendering(cmd,&info);
        VkViewport viewport{0,0,float(extent.width),float(extent.height),0,1}; VkRect2D scissor{{0,0},extent};
        c.vk.vkCmdSetViewport(cmd,0,1,&viewport); c.vk.vkCmdSetScissor(cmd,0,1,&scissor);
        c.vk.vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        c.vk.vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);
        if(indirect) c.vk.vkCmdDrawIndirect(cmd,indirect,3*sizeof(std::uint32_t),1,0);
        else c.vk.vkCmdDraw(cmd,3,1,0,0);
        c.vk.vkCmdEndRendering(cmd);
        for(auto* target:targets) transition(cmd,*target,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    const VulkanImage& render(const VulkanEffectsFrame& f) {
        if(!f.color || !f.depth || !f.normal || !f.extent.width || !f.extent.height || !f.output_extent.width || !f.output_extent.height) throw std::runtime_error("Vulkan effects: invalid world targets");
        // Interpolation continues only from a frame that interpolated immediately before this one.
        generation_continuous=std::exchange(generation_chain,false); generation_ready=midpoint=false;
        if(!f.settings.enhanced) { valid=false; destroy_fsr(); return *f.color; }
        const bool ms=f.ms_motion && f.ms_normal && f.ms_previous_normal;
        bool rt=c.ray_query && f.settings.ray_tracing && !f.instances.empty() &&
            (f.settings.shadows>0 || f.settings.ambient_occlusion>0 || f.settings.reflections>0 || f.settings.indirect_lighting>0);
        if(rt) ensure_ray_capacity(f.textures.size());
        const bool sun_active=rt&&f.settings.shadows>0;
        if(!same(effects.extent,f.extent) || !same(output.extent,f.output_extent)) valid=false;
        image(effects,f.extent); image(reflected,f.extent); image(output,f.output_extent);
        // FSR replaces artwork AA/upscaling; RT lighting history stays in the temporal pass.
        if(!c.fsr || !f.settings.temporal_aa || !f.motion) destroy_fsr();
        else if(!fsr || !same(fsr_render,f.extent) || !same(fsr_display,f.output_extent) || generation_format!=f.generation_format)
            create_fsr(f.command,f.extent,f.output_extent,f.generation_format);
        if(rt) { image(sun,f.extent,VK_FORMAT_R8G8B8A8_UNORM); image(shadow_offset,f.extent); }
        // Vulkan [0,1] perspective (w=-z): z_clip=m10*z+m14, so near=m14/m10 and far=m14/(m10+1).
        const auto& projection=f.projection.values;
        const float near_plane=projection[14]/projection[10],far_plane=projection[14]/(projection[10]+1);
        const float fov=2*std::atan(1/std::abs(projection[5]));
        // FSR's depth model is perspective-only; an orthographic world frame keeps the native base.
        const bool upscale=fsr && projection[11]!=0 && std::isfinite(near_plane) && std::isfinite(far_plane) &&
            std::isfinite(fov) && near_plane>0 && far_plane>near_plane;
        // Lighting denoise runs before FSR at render size when FSR upscales, else at output size for display.
        // A size switch reallocates (and clears) both history pairs and drops history this frame.
        const VkExtent2D lighting=upscale?f.extent:f.output_extent;
        if(!same(history[0].extent,lighting)) valid=false;
        for(unsigned i=0;i<2;++i) {
            image(history[i],lighting); image(geometry[i],lighting,VK_FORMAT_R32G32B32A32_SINT);
            image(base[i],lighting,VK_FORMAT_R32G32B32A32_SFLOAT); image(reflection[i],lighting);
            for(auto* target:{&history[i],&geometry[i],&base[i],&reflection[i]}) initialize_history(f.command,*target);
        }
        if(clear_cache) { for(auto& entry:cache) destroy(entry.second.as); cache.clear(); destroy(tlas); tlas_count=0; clear_cache=false; }
        if(rt) { gpu_timestamp(c,f.command,GpuAcceleration,false); build_scene(f); gpu_timestamp(c,f.command,GpuAcceleration,true); }
        Parameters p{}; p.current_vp=multiply(f.projection,f.view); p.inverse_vp=inverse(p.current_vp);
        Mat4 inverse_view=inverse(f.view); p.camera={inverse_view.values[12],inverse_view.values[13],inverse_view.values[14],0};
        p.camera[3]=camera_ray_basis(p,f.view,f.projection)?1.f:0.f;
        p.ray_basis_x[3]=f.fog?1.f:0.f;
        float camera_distance=0; for(int i=0;i<3;++i) camera_distance+=(p.camera[i]-previous_camera[i])*(p.camera[i]-previous_camera[i]);
        float camera_change=0; for(int i=0;i<12;++i) camera_change=std::max(camera_change,std::abs(p.current_vp.values[i]-previous_vp.values[i]));
        if(camera_distance>400 || camera_change>.75f) valid=false;
        if(f.settings.temporal_aa!=previous_settings.temporal_aa || f.settings.render_scale!=previous_settings.render_scale ||
            f.settings.samples!=previous_settings.samples || f.settings.anisotropy!=previous_settings.anisotropy ||
            f.settings.ray_tracing!=previous_settings.ray_tracing || f.settings.enhanced!=previous_settings.enhanced ||
            f.settings.roughness!=previous_settings.roughness || f.settings.ambient_occlusion!=previous_settings.ambient_occlusion ||
            f.settings.reflections!=previous_settings.reflections || f.settings.shadows!=previous_settings.shadows || f.settings.indirect_lighting!=previous_settings.indirect_lighting) valid=false;
        p.extent={float(f.extent.width),float(f.extent.height),float(f.output_extent.width),float(f.output_extent.height)};
        // Raster jitter is in source-resolution UV units. Temporal output and
        // its native-sized history are resolved onto the unjittered pixel grid.
        // jitter.w: the Sun pass writes shadow offsets for frame generation's midpoint selector (lighting grid
        // == Sun grid == render size under FSR).
        midpoint=sun_active && upscale && interpolation;
        p.jitter={f.jitter[0]/f.extent.width,f.jitter[1]/f.extent.height,0,midpoint?1.f:0.f};
        p.strengths={f.settings.ambient_occlusion,f.settings.reflections,rt?f.settings.shadows:0,rt?f.settings.indirect_lighting:0};
        // post.z: the display pass reads FSR's upscaled lit radiance instead of composing it.
        p.post={f.settings.exposure,f.settings.bloom,upscale?1.f:0.f,f.settings.roughness};
        const bool history_eligible=valid&&f.history_valid&&(f.settings.temporal_aa||rt);
        p.temporal={history_eligible?1.f:0.f,f.settings.temporal_aa?1.f:0.f,float(frame_index%4096),f.motion&&f.previous_normal?1.f:0.f};
        p.previous_vp=history_eligible?f.previous_view_projection:p.current_vp;
        p.inverse_previous_vp=history_eligible?inverse(f.previous_view_projection):p.inverse_vp;
        p.previous_ray_basis_x=history_eligible?previous_ray_basis_x:p.ray_basis_x;
        p.previous_ray_basis_y=history_eligible?previous_ray_basis_y:p.ray_basis_y;
        p.previous_ray_basis_z=history_eligible?previous_ray_basis_z:p.ray_basis_z;
        p.previous_camera=history_eligible?previous_camera:p.camera;
        // jitter.z: fsr_inputs.frag may compare against the previous FSR frame's reactive history
        // (exactly when FSR keeps its own history, d.reset below is false).
        p.jitter[2]=upscale&&history_eligible&&fsr_history?1.f:0.f;
        std::memcpy(uniform.mapped,&p,sizeof p);
        const VkDescriptorSet effect_descriptor=rt?(ms?ray_ms_set:ray_set):(ms?effect_ms_set:effect_set);
        const VkPipeline effect_shader=rt?(ms?ray_ms_pipeline:ray_pipeline):(ms?effect_ms_pipeline:effect_pipeline);
        const VkPipelineLayout effect_pass_layout=rt?(ms?ray_ms_pipeline_layout:ray_pipeline_layout):(ms?effect_ms_pipeline_layout:effect_pipeline_layout);
        descriptors(f,effect_descriptor,rt,false,ping,false,ms);
        // effects.frag queues only stochastic (diffuse or rough reflection), history-unsafe pixels.
        const bool queued=rt && (p.strengths[0]>0 || p.strengths[3]>0 || (p.strengths[1]>0 && p.post[3]!=0));
        if(rt) {
            if(queued) {
                // Each render pixel queues at most once, so the render extent bounds the state buffer.
                // reserve() only reallocates to a strictly larger size. Compare sizes, not handles: a driver can
                // hand the destroyed VkBuffer's handle value straight back (RADV frees/mallocs handles), leaving set 1 on freed memory.
                const VkDeviceSize previous_size=pending.gpu.size;
                reserve(pending,VkDeviceSize(f.extent.width)*f.extent.height*pending_stride,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                if(pending.gpu.size!=previous_size) write_pending_set();
                const std::uint32_t empty[7]{0,1,1, 0,1,0,0}; // no workgroups; no points, one instance
                c.vk.vkCmdUpdateBuffer(f.command,queue.gpu.buffer,0,sizeof empty,empty);
                memory_barrier(f.command,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            }
            // Classification, raw FX and resolve share this layout, so set 1 stays bound across their set-0 binds.
            c.vk.vkCmdBindDescriptorSets(f.command,VK_PIPELINE_BIND_POINT_GRAPHICS,effect_pass_layout,1,1,&pending_set,0,nullptr);
            // The same set is immutable across classification and raw FX.
            // Binding 13 is unused by classification, then reads its completed
            // scratch image; the later Sun pass overwrites it only after raw FX.
            VulkanImage* classify_targets[]{&sun};
            gpu_timestamp(c,f.command,GpuClassify,false);
            pass(f.command,classify_targets,ms?classify_ms_pipeline:classify_pipeline,effect_pass_layout,effect_descriptor);
            gpu_timestamp(c,f.command,GpuClassify,true);
        }
        VulkanImage* effect_targets[]{&effects,&reflected};
        gpu_timestamp(c,f.command,GpuEffects,false); pass(f.command,effect_targets,effect_shader,effect_pass_layout,effect_descriptor);
        if(queued) {
            // Queued state and indirect arguments -> compute; its final values -> the resolve points.
            memory_barrier(f.command,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            const VkDescriptorSet sets[]{effect_descriptor,pending_set};
            c.vk.vkCmdBindPipeline(f.command,VK_PIPELINE_BIND_POINT_COMPUTE,pending_pipeline[ms]);
            c.vk.vkCmdBindDescriptorSets(f.command,VK_PIPELINE_BIND_POINT_COMPUTE,effect_pass_layout,0,2,sets,0,nullptr);
            c.vk.vkCmdDispatchIndirect(f.command,queue.gpu.buffer,0);
            memory_barrier(f.command,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            pass(f.command,effect_targets,resolve_pipeline[ms],effect_pass_layout,effect_descriptor,queue.gpu.buffer);
        }
        gpu_timestamp(c,f.command,GpuEffects,true);
        if(rt) ++query_count;
        if(sun_active) {
            descriptors(f,sun_set,true,false,ping,false,false,true);
            VulkanImage* sun_targets[]{&sun,&shadow_offset};
            gpu_timestamp(c,f.command,GpuSun,false); pass(f.command,sun_targets,sun_pipeline,sun_pipeline_layout,sun_set); gpu_timestamp(c,f.command,GpuSun,true);
        }
        descriptors(f,temporal_set,false,true,ping,false,false,false,sun_active);
        unsigned next=1-ping; VulkanImage* temporal_targets[]{&history[next],&geometry[next],&base[next],&reflection[next]};
        gpu_timestamp(c,f.command,GpuTemporal,false); pass(f.command,temporal_targets,temporal_pipeline,temporal_pipeline_layout,temporal_set); gpu_timestamp(c,f.command,GpuTemporal,true);
        if(upscale) {
            gpu_timestamp(c,f.command,GpuFsrInputs,false);
            // The FSR input's per-sample Sun visibility, temporally filtered before FSR (sun_taa.comp). Its previous
            // history counts only if the previous frame wrote it; p.jitter.z resets it with FSR's own history.
            VulkanImage& filtered_sun=sun_history[sun_ping];
            if(sun_active) {
                VulkanImage& previous_sun=sun_history[1-sun_ping];
                const std::array<VkDescriptorImageInfo,6> taa_images{{{nearest_sampler,sun.view,sun.layout},
                    {nearest_sampler,f.depth->view,f.depth->layout},{nearest_sampler,f.motion->view,f.motion->layout},{},
                    {nearest_sampler,previous_sun.view,previous_sun.layout},{VK_NULL_HANDLE,filtered_sun.view,VK_IMAGE_LAYOUT_GENERAL}}};
                const VkDescriptorBufferInfo taa_ubo{uniform.gpu.buffer,0,sizeof(Parameters)};
                std::array<VkWriteDescriptorSet,6> taa_writes{};
                for(std::uint32_t i=0;i<6;++i) taa_writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,sun_taa_set,i,0,1,
                    i==3?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:i==5?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    i==3?nullptr:&taa_images[i],i==3?&taa_ubo:nullptr,nullptr};
                c.vk.vkUpdateDescriptorSets(c.device,6,taa_writes.data(),0,nullptr);
                // transition(): ALL_COMMANDS barriers, so the Sun pass's writes and the previous frame's reads of this
                // image precede the dispatch, and its writes precede the FSR inputs pass reading it.
                transition(f.command,filtered_sun,VK_IMAGE_LAYOUT_GENERAL);
                const float history_constants[3]{sun_jitter[0],sun_jitter[1],sun_history_valid?1.f:0.f};
                c.vk.vkCmdBindPipeline(f.command,VK_PIPELINE_BIND_POINT_COMPUTE,sun_taa_pipeline);
                c.vk.vkCmdBindDescriptorSets(f.command,VK_PIPELINE_BIND_POINT_COMPUTE,sun_taa_pipeline_layout,0,1,&sun_taa_set,0,nullptr);
                c.vk.vkCmdPushConstants(f.command,sun_taa_pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof history_constants,history_constants);
                c.vk.vkCmdDispatch(f.command,(f.extent.width+7)/8,(f.extent.height+7)/8,1);
                transition(f.command,filtered_sun,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                sun_ping=1-sun_ping; sun_jitter={p.jitter[0],p.jitter[1]};
            }
            sun_history_valid=sun_active;
            // FSR input colour is composed from the temporal outputs this frame just wrote (display.frag's inputs);
            // binding 8 is the previous reactive history while the pass writes the other one.
            VulkanImage& previous_illumination=fsr_illumination[1-fsr_ping];
            std::array<VkDescriptorImageInfo,10> inputs{{{nearest_sampler,f.color->view,f.color->layout},
                {nearest_sampler,f.depth->view,f.depth->layout},{nearest_sampler,f.motion->view,f.motion->layout},{},
                {nearest_sampler,history[next].view,history[next].layout},{nearest_sampler,geometry[next].view,geometry[next].layout},
                {nearest_sampler,reflection[next].view,reflection[next].layout},
                {nearest_sampler,(sun_active?sun:effects).view,(sun_active?sun:effects).layout},
                {nearest_sampler,previous_illumination.view,previous_illumination.layout},
                {nearest_sampler,filtered_sun.view,filtered_sun.layout}}};
            VkDescriptorBufferInfo ubo{uniform.gpu.buffer,0,sizeof(Parameters)};
            std::array<VkWriteDescriptorSet,10> writes{};
            for(std::uint32_t i=0;i<10;++i) {
                writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=fsr_inputs_set; writes[i].dstBinding=i; writes[i].descriptorCount=1;
                writes[i].descriptorType=i==3?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                if(i==3) writes[i].pBufferInfo=&ubo; else writes[i].pImageInfo=&inputs[i];
            }
            c.vk.vkUpdateDescriptorSets(c.device,10,writes.data(),0,nullptr);
            VulkanImage* input_targets[]{&fsr_color,&fsr_motion,&fsr_illumination[fsr_ping],&fsr_reactive};
            pass(f.command,input_targets,fsr_inputs_pipeline,fsr_inputs_pipeline_layout,fsr_inputs_set); gpu_timestamp(c,f.command,GpuFsrInputs,true);
            fsr_ping=1-fsr_ping;
            // ffxGetResourceVK states are the layouts the images are actually in; FSR restores them.
            if(f.depth->layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) throw std::runtime_error("FSR depth input must be shader-read-only");
            gpu_timestamp(c,f.command,GpuFsr,false); transition(f.command,upscaled,VK_IMAGE_LAYOUT_GENERAL);
            constexpr auto read=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            constexpr auto storage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            const auto now=std::chrono::steady_clock::now();
            FfxFsr3UpscalerDispatchDescription d{};
            d.commandList=ffxGetCommandListVK(f.command);
            d.color=ffx_image(fsr_color,read,FFX_RESOURCE_STATE_COMPUTE_READ);
            d.depth=ffx_image(*f.depth,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,FFX_RESOURCE_STATE_COMPUTE_READ);
            d.motionVectors=ffx_image(fsr_motion,read,FFX_RESOURCE_STATE_COMPUTE_READ);
            // Shading that changed without matching motion (moving shadows on static ground): the reactive mask cuts the
            // history weight and the same mask as transparency & composition removes FSR's locks, which otherwise keep
            // a faint trail of the old shadow on high-contrast texture.
            d.reactive=ffx_image(fsr_reactive,read,FFX_RESOURCE_STATE_COMPUTE_READ);
            d.transparencyAndComposition=d.reactive;
            d.dilatedDepth=ffx_image(fsr_dilated_depth,storage,FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            d.dilatedMotionVectors=ffx_image(fsr_dilated_motion,storage,FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            d.reconstructedPrevNearestDepth=ffx_image(fsr_previous_depth,storage,FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            d.output=ffx_image(upscaled,storage,FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            // Pixel jitter and UV motion (previous - unjittered current) share the raster convention.
            d.jitterOffset={f.jitter[0],f.jitter[1]};
            d.motionVectorScale={float(f.extent.width),float(f.extent.height)};
            d.renderSize={f.extent.width,f.extent.height}; d.upscaleSize={f.output_extent.width,f.output_extent.height};
            d.enableSharpening=f.settings.sharpen>0; d.sharpness=f.settings.sharpen;
            d.frameTimeDelta=fsr_history?std::chrono::duration<float,std::milli>(now-fsr_time).count():1000.f/60;
            d.preExposure=1; d.reset=!history_eligible || !fsr_history;
            d.cameraNear=near_plane; d.cameraFar=far_plane; d.cameraFovAngleVertical=fov;
            d.viewSpaceToMetersFactor=metres_per_unit;
            ffx(ffxFsr3UpscalerContextDispatch(fsr.get(),&d),"FSR dispatch");
            transition(f.command,upscaled,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); gpu_timestamp(c,f.command,GpuFsr,true);
            fsr_time=now; fsr_reset=d.reset;
            frame_ms=std::clamp(d.frameTimeDelta,.1f,1000.f); camera_near=near_plane; camera_far=far_plane; camera_fov=fov;
        }
        generation_ready=upscale && interpolation;
        fsr_history=upscale;
        descriptors(f,display_set,false,false,next,true,false,false,sun_active,upscale);
        VulkanImage* display_targets[]{&output};
        gpu_timestamp(c,f.command,GpuDisplay,false); pass(f.command,display_targets,display_pipeline,display_pipeline_layout,display_set);
        if(midpoint) midpoint_selector(f,next);
        gpu_timestamp(c,f.command,GpuDisplay,true);
        ping=next; valid=true; previous_settings=f.settings; previous_vp=p.current_vp;
        previous_camera=p.camera; previous_ray_basis_x=p.ray_basis_x;
        previous_ray_basis_y=p.ray_basis_y; previous_ray_basis_z=p.ray_basis_z; ++frame_index;
        return output;
    }
    // Frame generation's midpoint Sun selector (inside the --gpu-timings display row): splat this frame's Sun factor
    // halfway towards each shadow's previous position, then mark per lighting texel whether its midpoint state is
    // this frame's (1) or the previous frame's (0).
    void midpoint_selector(const VulkanEffectsFrame& f,unsigned next) {
        constexpr auto storage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image(sun_now,f.extent,VK_FORMAT_R32_SFLOAT,storage); image(sun_mid,f.extent,VK_FORMAT_R32_UINT,storage);
        image(midpoint_select,f.extent,VK_FORMAT_R8_UNORM,storage);
        for(auto* i:{&sun_now,&sun_mid,&midpoint_select}) if(i->layout!=VK_IMAGE_LAYOUT_GENERAL) transition(f.command,*i,VK_IMAGE_LAYOUT_GENERAL);
        // The previous frame's reads (selector pass, interpolation) -> this clear and these stores; the clear -> the splat's atomics.
        memory_barrier(f.command,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_CLEAR_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        VkClearColorValue unlit{}; unlit.uint32[0]=0x3f800000u; // 1.0f
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        c.vk.vkCmdClearColorImage(f.command,sun_mid.image,VK_IMAGE_LAYOUT_GENERAL,&unlit,1,&range);
        memory_barrier(f.command,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        const std::array<VkDescriptorImageInfo,10> images{{{nearest_sampler,history[next].view,history[next].layout},
            {nearest_sampler,geometry[next].view,geometry[next].layout},{nearest_sampler,f.color->view,f.color->layout},{},
            {nearest_sampler,sun.view,sun.layout},{nearest_sampler,shadow_offset.view,shadow_offset.layout},
            {VK_NULL_HANDLE,sun_now.view,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,sun_mid.view,VK_IMAGE_LAYOUT_GENERAL},
            {nearest_sampler,reflection[next].view,reflection[next].layout},{VK_NULL_HANDLE,midpoint_select.view,VK_IMAGE_LAYOUT_GENERAL}}};
        const VkDescriptorBufferInfo ubo{uniform.gpu.buffer,0,sizeof(Parameters)};
        std::array<VkWriteDescriptorSet,10> writes{};
        for(std::uint32_t i=0;i<10;++i) writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,midpoint_set,i,0,1,
            i==3?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:i==6||i==7||i==9?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            i==3?nullptr:&images[i],i==3?&ubo:nullptr,nullptr};
        c.vk.vkUpdateDescriptorSets(c.device,10,writes.data(),0,nullptr);
        c.vk.vkCmdBindDescriptorSets(f.command,VK_PIPELINE_BIND_POINT_COMPUTE,midpoint_pipeline_layout,0,1,&midpoint_set,0,nullptr);
        for(unsigned pass_index=0;pass_index<2;++pass_index) {
            c.vk.vkCmdBindPipeline(f.command,VK_PIPELINE_BIND_POINT_COMPUTE,midpoint_pipeline[pass_index]);
            c.vk.vkCmdDispatch(f.command,(f.extent.width+7)/8,(f.extent.height+7)/8,1);
            // Splat stores/atomics -> selector loads; selector stores -> frame interpolation's reads.
            memory_barrier(f.command,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                pass_index?VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT:VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                pass_index?VK_ACCESS_2_MEMORY_READ_BIT:VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        }
    }
    // IKV fsr_vk.cpp:218-242: optical flow on the HUD-less source, then frame interpolation reusing this
    // frame's FSR dilated depth/motion and previous depth, with the upscaler's camera and frame time.
    bool interpolate(VkCommandBuffer cmd,VulkanImage& source,VulkanImage& target) {
        if(!generation_ready) throw std::logic_error("Frame interpolation requires this frame's FSR upscale");
        if(source.format!=generation_format || target.format!=generation_format || !same(source.extent,fsr_display) || !same(target.extent,fsr_display))
            throw std::runtime_error("Frame interpolation images must match the display size and generation format");
        generation_ready=false;
        transition(cmd,source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); transition(cmd,target,VK_IMAGE_LAYOUT_GENERAL);
        const bool reset=fsr_reset || !generation_continuous;
        constexpr auto storage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const auto vector=generation.fpGetResource(&generation,flow_output[0]);
        const auto scene_change=generation.fpGetResource(&generation,flow_output[1]);
        const auto color=ffx_image(source,VulkanEffects::generation_source_usage,FFX_RESOURCE_STATE_COMPUTE_READ);
        FfxOpticalflowDispatchDescription flow_dispatch{};
        flow_dispatch.commandList=ffxGetCommandListVK(cmd); flow_dispatch.color=color;
        flow_dispatch.opticalFlowVector=vector; flow_dispatch.opticalFlowSCD=scene_change;
        flow_dispatch.reset=reset; flow_dispatch.backbufferTransferFunction=FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        ffx(ffxOpticalflowContextDispatch(flow.get(),&flow_dispatch),"optical flow dispatch");
        // The SDK's FSR3 component passes the same parameters when its upscaler produced the dilated inputs.
        FfxFrameInterpolationDispatchDescription d{};
        d.commandList=flow_dispatch.commandList;
        d.displaySize={fsr_display.width,fsr_display.height}; d.renderSize={fsr_render.width,fsr_render.height};
        d.currentBackBuffer=color; d.output=ffx_image(target,VulkanEffects::generation_target_usage,FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        d.interpolationRect={0,0,int(fsr_display.width),int(fsr_display.height)};
        d.opticalFlowVector=vector; d.opticalFlowSceneChangeDetection=scene_change; d.opticalFlowBlockSize=8;
        d.opticalFlowScale={1.f/float(fsr_display.width),1.f/float(fsr_display.height)};
        d.cameraNear=camera_near; d.cameraFar=camera_far; d.cameraFovAngleVertical=camera_fov;
        d.viewSpaceToMetersFactor=metres_per_unit;
        d.frameTimeDelta=frame_ms; d.reset=reset; d.backBufferTransferFunction=FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        d.frameID=++generation_frame;
        d.dilatedDepth=ffx_image(fsr_dilated_depth,storage,FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        d.dilatedMotionVectors=ffx_image(fsr_dilated_motion,storage,FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        d.reconstructedPrevDepth=ffx_image(fsr_previous_depth,storage,FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        // Local SDK patch (third_party/fidelityfx-sdk README item 7): where the same surface's shading changed between the
        // two real frames (a moving shadow), interpolation takes this frame's colour instead of cross-fading two shadows.
        // This frame's FSR reactive mask says exactly that; it stays shader-read-only from the FSR inputs pass.
        d.shadingChange=ffx_image(fsr_reactive,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            FFX_RESOURCE_STATE_COMPUTE_READ);
        // Same patch: where it applies, each surface point takes the previous or this frame's game-vector sample,
        // whichever holds its RT Sun state halfway between the frames; without a selector it takes this frame's.
        if(midpoint) d.midpointSelect=ffx_image(midpoint_select,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        ffx(ffxFrameInterpolationDispatch(interpolation.get(),&d),"frame interpolation dispatch");
        generation_chain=true;
        return !reset;
    }
    void cleanup() noexcept {
        for(auto& entry:cache) destroy(entry.second.as);
        destroy(tlas); destroy(scratch); destroy(instances); destroy(materials); destroy(uniform); destroy(queue); destroy(pending);
        destroy_fsr();
        destroy(effects); destroy(reflected); destroy(sun); destroy(shadow_offset); destroy(output);
        for(auto& i:history) destroy(i); for(auto& i:geometry) destroy(i);
        for(auto& i:base) destroy(i); for(auto& i:reflection) destroy(i);
        destroy_ray_pipeline();
        for(auto p:{effect_pipeline,effect_ms_pipeline,temporal_pipeline,display_pipeline,fsr_inputs_pipeline,midpoint_pipeline[0],midpoint_pipeline[1],sun_taa_pipeline}) if(p) c.vk.vkDestroyPipeline(c.device,p,nullptr);
        for(auto p:{effect_pipeline_layout,effect_ms_pipeline_layout,temporal_pipeline_layout,display_pipeline_layout,fsr_inputs_pipeline_layout,
                    midpoint_pipeline_layout,sun_taa_pipeline_layout}) if(p) c.vk.vkDestroyPipelineLayout(c.device,p,nullptr);
        if(pool) c.vk.vkDestroyDescriptorPool(c.device,pool,nullptr);
        for(auto l:{effect_layout,effect_ms_layout,temporal_layout,display_layout,fsr_inputs_layout,midpoint_layout,sun_taa_layout}) if(l) c.vk.vkDestroyDescriptorSetLayout(c.device,l,nullptr);
        if(pending_pool) c.vk.vkDestroyDescriptorPool(c.device,pending_pool,nullptr);
        if(pending_layout) c.vk.vkDestroyDescriptorSetLayout(c.device,pending_layout,nullptr);
        if(sampler) c.vk.vkDestroySampler(c.device,sampler,nullptr);
        if(nearest_sampler) c.vk.vkDestroySampler(c.device,nearest_sampler,nullptr);
    }
};
VulkanEffects::VulkanEffects(VulkanContext& c):impl_(std::make_unique<Impl>(c)) {}
VulkanEffects::~VulkanEffects()=default;
const VulkanImage& VulkanEffects::render(const VulkanEffectsFrame& f) { return impl_->render(f); }
void VulkanEffects::reset_history() noexcept { impl_->valid=false; impl_->clear_cache=true; impl_->generation_chain=false; }
std::uint64_t VulkanEffects::acceleration_builds() const noexcept { return impl_->build_count; }
std::uint64_t VulkanEffects::ray_query_frames() const noexcept { return impl_->query_count; }
bool VulkanEffects::frame_generation() const noexcept { return impl_->generation_ready; }
bool VulkanEffects::interpolate(VkCommandBuffer command,VulkanImage& source,VulkanImage& target) { return impl_->interpolate(command,source,target); }
}
