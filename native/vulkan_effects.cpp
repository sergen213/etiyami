#include "vulkan_effects.hpp"
#include "yami_vulkan_shaders.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace yami {
namespace {
void checked(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation)+": Vulkan result "+std::to_string(result));
}
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
};
static_assert(sizeof(RayMaterial)==96);
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
    VulkanImage effects,reflected,history[2],geometry[2],base[2],reflection[2],output;
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
        allocation.memoryTypeIndex=memory_type(requirements.memoryTypeBits,host?(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT):VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checked(c.vk.vkAllocateMemory(c.device,&allocation,nullptr,&b.gpu.memory),"effects allocate buffer");
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
    void image(VulkanImage& image,VkExtent2D extent,VkFormat format=VK_FORMAT_R16G16B16A16_SFLOAT) {
        if(image.image && same(image.extent,extent) && image.format==format) return;
        destroy(image);
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType=VK_IMAGE_TYPE_2D; info.format=format; info.extent={extent.width,extent.height,1};
        info.mipLevels=1; info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
    VkDescriptorSetLayout layout(bool ray,bool temporal,std::uint32_t source_capacity=0,bool display=false,bool ms=false) {
        std::array<VkDescriptorSetLayoutBinding,12> bindings{};
        const bool artist=!temporal&&!display;
        const unsigned base_count=temporal?12:ray?8:5,count=base_count+(ms?3:0)+(artist?1:0);
        for(unsigned i=0;i<count;++i) {
            bindings[i].binding=i; bindings[i].descriptorCount=1; bindings[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[i].descriptorType=i==3?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        if(!ray && !temporal && !display) bindings[4].binding=7;
        if(ms) for(unsigned i=base_count;i<base_count+3;++i) bindings[i].binding=8+i-base_count;
        if(artist) bindings[count-1].binding=11;
        if(ray) { bindings[4].descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; bindings[5].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[6].descriptorCount=source_capacity; }
        std::array<VkDescriptorBindingFlags,12> binding_flags{}; if(ray) binding_flags[6]=VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO}; flags.bindingCount=count; flags.pBindingFlags=binding_flags.data();
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; info.bindingCount=count; info.pBindings=bindings.data(); info.pNext=ray?&flags:nullptr;
        VkDescriptorSetLayout result{}; checked(c.vk.vkCreateDescriptorSetLayout(c.device,&info,nullptr,&result),"effects descriptor layout"); return result;
    }
    VkPipelineLayout pipeline_layout(VkDescriptorSetLayout set) {
        VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; info.setLayoutCount=1; info.pSetLayouts=&set;
        VkPipelineLayout result{}; checked(c.vk.vkCreatePipelineLayout(c.device,&info,nullptr,&result),"effects pipeline layout"); return result;
    }
    VkShaderModule shader(std::span<const std::uint32_t> words) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize=words.size_bytes(); info.pCode=words.data();
        VkShaderModule result{}; checked(c.vk.vkCreateShaderModule(c.device,&info,nullptr,&result),"effects shader module"); return result;
    }
    VkPipeline pipeline(VkPipelineLayout layout,std::span<const std::uint32_t> code,unsigned attachments) {
        VkShaderModule vertex=shader(spirv::vulkan_final_vert),fragment=VK_NULL_HANDLE;
        VkPipeline result{};
        try {
            fragment=shader(code);
            VkPipelineShaderStageCreateInfo stages[2]{};
            for(int i=0;i<2;++i) { stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT; stages[i].module=i?fragment:vertex; stages[i].pName="main"; }
            VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount=1; viewport.scissorCount=1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode=VK_POLYGON_MODE_FILL; raster.lineWidth=1;
            VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
            std::array<VkPipelineColorBlendAttachmentState,4> blend{}; for(auto& a:blend) a.colorWriteMask=15;
            VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blending.attachmentCount=attachments; blending.pAttachments=blend.data();
            VkDynamicState dynamics[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount=2; dynamic.pDynamicStates=dynamics;
            std::array<VkFormat,4> formats{VK_FORMAT_R16G16B16A16_SFLOAT,
                attachments==4?VK_FORMAT_R32G32B32A32_SFLOAT:VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT};
            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.colorAttachmentCount=attachments; rendering.pColorAttachmentFormats=formats.data();
            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; info.pNext=&rendering;
            info.stageCount=2; info.pStages=stages; info.pVertexInputState=&input; info.pInputAssemblyState=&assembly; info.pViewportState=&viewport;
            info.pRasterizationState=&raster; info.pMultisampleState=&samples; info.pColorBlendState=&blending; info.pDynamicState=&dynamic; info.layout=layout;
            checked(c.vk.vkCreateGraphicsPipelines(c.device,VK_NULL_HANDLE,1,&info,nullptr,&result),"effects graphics pipeline");
        } catch (...) { if(fragment) c.vk.vkDestroyShaderModule(c.device,fragment,nullptr); c.vk.vkDestroyShaderModule(c.device,vertex,nullptr); throw; }
        c.vk.vkDestroyShaderModule(c.device,fragment,nullptr); c.vk.vkDestroyShaderModule(c.device,vertex,nullptr); return result;
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
        std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,28},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,4}}};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool_info.maxSets=4; pool_info.poolSizeCount=std::uint32_t(sizes.size()); pool_info.pPoolSizes=sizes.data();
        checked(c.vk.vkCreateDescriptorPool(c.device,&pool_info,nullptr,&pool),"effects descriptor pool");
        effect_layout=layout(false,false); temporal_layout=layout(false,true); display_layout=layout(false,false,0,true);
        effect_ms_layout=layout(false,false,0,false,true);
        effect_pipeline_layout=pipeline_layout(effect_layout); temporal_pipeline_layout=pipeline_layout(temporal_layout); display_pipeline_layout=pipeline_layout(display_layout);
        effect_ms_pipeline_layout=pipeline_layout(effect_ms_layout);
        effect_pipeline=pipeline(effect_pipeline_layout,spirv::effects_frag,2);
        effect_ms_pipeline=pipeline(effect_ms_pipeline_layout,spirv::effects_ms_frag,2);
        temporal_pipeline=pipeline(temporal_pipeline_layout,spirv::temporal_frag,4);
        display_pipeline=pipeline(display_pipeline_layout,spirv::display_frag,1);
        effect_set=descriptor(effect_layout,pool); temporal_set=descriptor(temporal_layout,pool); display_set=descriptor(display_layout,pool);
        effect_ms_set=descriptor(effect_ms_layout,pool);
    }
    void destroy_ray_pipeline() noexcept {
        if(ray_pipeline) c.vk.vkDestroyPipeline(c.device,ray_pipeline,nullptr);
        if(ray_ms_pipeline) c.vk.vkDestroyPipeline(c.device,ray_ms_pipeline,nullptr);
        if(ray_pipeline_layout) c.vk.vkDestroyPipelineLayout(c.device,ray_pipeline_layout,nullptr);
        if(ray_ms_pipeline_layout) c.vk.vkDestroyPipelineLayout(c.device,ray_ms_pipeline_layout,nullptr);
        if(ray_pool) c.vk.vkDestroyDescriptorPool(c.device,ray_pool,nullptr);
        if(ray_layout) c.vk.vkDestroyDescriptorSetLayout(c.device,ray_layout,nullptr);
        if(ray_ms_layout) c.vk.vkDestroyDescriptorSetLayout(c.device,ray_ms_layout,nullptr);
        ray_pipeline=VK_NULL_HANDLE; ray_pipeline_layout=VK_NULL_HANDLE;
        ray_pool=VK_NULL_HANDLE; ray_layout=VK_NULL_HANDLE; ray_set=VK_NULL_HANDLE;
        ray_ms_pipeline=VK_NULL_HANDLE; ray_ms_pipeline_layout=VK_NULL_HANDLE;
        ray_ms_layout=VK_NULL_HANDLE; ray_ms_set=VK_NULL_HANDLE;
        ray_capacity=0;
    }
    void ensure_ray_capacity(std::size_t count) {
        if(count<=ray_capacity) return;
        const auto& limits=c.properties.limits;
        const auto available=[](std::uint32_t limit,std::uint32_t fixed) { return limit>fixed?limit-fixed:0u; };
        const auto maximum=std::min({available(limits.maxPerStageDescriptorSamplers,8),
            available(limits.maxDescriptorSetSamplers,8),available(limits.maxPerStageDescriptorSampledImages,8),
            available(limits.maxDescriptorSetSampledImages,8),available(limits.maxPerStageResources,11)});
        if(count>maximum) throw std::runtime_error("Vulkan ray-query artwork table requires "+std::to_string(count)+
            " textures, but this device's descriptor limits allow "+std::to_string(maximum));
        auto capacity=std::max(ray_capacity,1u);
        while(capacity<count) capacity=capacity<=maximum/2?capacity*2:maximum;
        // The renderer completed the previous frame before entering this pass.
        // Grow only before recording any effects/AS commands; unrelated temporal
        // descriptor sets stay alive in their own pool.
        destroy_ray_pipeline();
        std::array<VkDescriptorPoolSize,4> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,capacity*2+13},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2},
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,2}}};
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets=2; info.poolSizeCount=std::uint32_t(sizes.size()); info.pPoolSizes=sizes.data();
        checked(c.vk.vkCreateDescriptorPool(c.device,&info,nullptr,&ray_pool),"effects grow ray descriptor pool");
        ray_layout=layout(true,false,capacity); ray_pipeline_layout=pipeline_layout(ray_layout);
        ray_pipeline=pipeline(ray_pipeline_layout,spirv::effects_rt_frag,2);
        ray_ms_layout=layout(true,false,capacity,false,true);
        ray_ms_pipeline_layout=pipeline_layout(ray_ms_layout);
        ray_ms_pipeline=pipeline(ray_ms_pipeline_layout,spirv::effects_rt_ms_frag,2);
        ray_ms_set=descriptor(ray_ms_layout,ray_pool);
        ray_set=descriptor(ray_layout,ray_pool); ray_capacity=capacity;
    }
    void acceleration_barrier(VkCommandBuffer cmd,bool trace=false) {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask=VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR; b.srcAccessMask=VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        b.dstStageMask=trace?VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        b.dstAccessMask=VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR|(trace?0:VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.memoryBarrierCount=1; dependency.pMemoryBarriers=&b; c.vk.vkCmdPipelineBarrier2(cmd,&dependency);
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
                c.vk.vkCmdBuildAccelerationStructuresKHR(f.command,1,&build,&ranges); ++build_count; acceleration_barrier(f.command);
                blas.revision=mesh.revision; blas.vertices=mesh.vertex_count; blas.indices=mesh.index_count;
            }
            if(instance.texture_index>=f.textures.size()) throw std::runtime_error("Vulkan ray-query missing material texture");
            VkAccelerationStructureInstanceKHR gpu{};
            for(int r=0;r<3;++r) for(int col=0;col<4;++col) gpu.transform.matrix[r][col]=instance.model.values[col*4+r];
            gpu.instanceCustomIndex=instance_index; gpu.mask=255;
            // Candidate shader honors original culling and alpha tests; never opaque.
            gpu.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR|
                VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR; gpu.accelerationStructureReference=blas.as.address;
            // Original GL_CW / Vulkan CW after projection Y-flip is opposite
            // ray-space default facing. Mirrored models reverse that base flip.
            const auto& m=instance.model.values;
            const float determinant=m[0]*(m[5]*m[10]-m[9]*m[6])-
                m[4]*(m[1]*m[10]-m[9]*m[2])+m[8]*(m[1]*m[6]-m[5]*m[2]);
            if(determinant<0) gpu.flags^=VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
            gpu_instances[instance_index]=gpu;
            gpu_materials[instance_index]={mesh.vertices.address,mesh.indices.address,instance.uv_transform,instance.color,instance.diffuse_light,
                {instance.texture_index,instance.alpha_function,std::uint32_t(instance.alpha_test),std::uint32_t(instance.flip_vertical)},
                {instance.alpha_reference,float(instance.cull),float(instance.cull_face),float(instance.lighting)}};
            ++instance_index;
        }
        bool update=tlas.handle && tlas_count==count && tlas.storage.gpu.size>=top_sizes.accelerationStructureSize;
        structure(tlas,top_sizes.accelerationStructureSize,VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
        top_build.mode=update?VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR:VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        top_build.srcAccelerationStructure=update?tlas.handle:VK_NULL_HANDLE; top_build.dstAccelerationStructure=tlas.handle; top_build.scratchData.deviceAddress=scratch_at;
        VkAccelerationStructureBuildRangeInfoKHR range{}; range.primitiveCount=count; const auto* ranges=&range;
        c.vk.vkCmdBuildAccelerationStructuresKHR(f.command,1,&top_build,&ranges); ++build_count; tlas_count=count; acceleration_barrier(f.command,true);
    }
    void descriptors(const VulkanEffectsFrame& f,VkDescriptorSet set,bool ray,bool temporal,unsigned old,bool display=false,bool ms=false) {
        // Each pass owns a descriptor set: recorded temporal descriptors must
        // never be overwritten with display inputs before command submission.
        std::array<VkDescriptorImageInfo,12> images{};
        const auto source=[&](unsigned binding,const VulkanImage& image,VkSampler filter) {
            images[binding]={filter,image.view,image.layout};
        };
        if(display) {
            source(0,base[old],nearest_sampler); source(1,history[old],nearest_sampler);
            source(2,reflection[old],nearest_sampler); source(4,*f.color,sampler);
        } else {
            source(0,temporal?effects:*f.color,nearest_sampler);
            source(1,*f.depth,nearest_sampler); source(2,*f.normal,nearest_sampler);
            if(temporal) {
                source(4,history[old],nearest_sampler); source(5,geometry[old],nearest_sampler);
                source(6,*f.color,nearest_sampler); source(7,reflected,nearest_sampler);
                source(8,base[old],nearest_sampler); source(9,reflection[old],nearest_sampler);
                // Optional metadata supports direct effects fixtures/static
                // callers; temporal.w disables integer reads when absent.
                source(10,f.motion?*f.motion:*f.normal,nearest_sampler);
                source(11,f.previous_normal?*f.previous_normal:*f.normal,nearest_sampler);
            } else {
                source(7,f.previous_normal?*f.previous_normal:*f.normal,nearest_sampler);
                if(ms) {
                    source(8,*f.ms_motion,nearest_sampler); source(9,*f.ms_normal,nearest_sampler);
                    source(10,*f.ms_previous_normal,nearest_sampler);
                }
                // Even a dynamically guarded descriptor must match the shader's
                // floating sampled type and selected MS/non-MS image view.
                source(11,f.artist_mask?*f.artist_mask:ms?*f.ms_color:*f.color,nearest_sampler);
            }
        }
        VkDescriptorBufferInfo ubo{uniform.gpu.buffer,0,sizeof(Parameters)},table{materials.gpu.buffer,0,materials.gpu.size};
        std::array<VkWriteDescriptorSet,12> writes{};
        const bool artist=!temporal&&!display;
        const unsigned base_count=temporal?12:ray?8:5,count=base_count+(ms?3:0)+(artist?1:0);
        for(unsigned i=0;i<count;++i) {
            writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=set;
            writes[i].dstBinding=artist&&i==count-1?11:ms && i>=base_count?8+i-base_count:(!ray && !temporal && !display && i==4)?7:i; writes[i].descriptorCount=1;
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
    void pass(VkCommandBuffer cmd,std::span<VulkanImage*> targets,VkPipeline pipeline,VkPipelineLayout layout,VkDescriptorSet set) {
        std::array<VkRenderingAttachmentInfo,4> attachments{};
        for(std::size_t i=0;i<targets.size();++i) {
            transition(cmd,*targets[i],VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            attachments[i].sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; attachments[i].imageView=targets[i]->view;
            attachments[i].imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; attachments[i].loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachments[i].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkExtent2D extent=targets[0]->extent;
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO}; info.renderArea.extent=extent; info.layerCount=1; info.colorAttachmentCount=std::uint32_t(targets.size()); info.pColorAttachments=attachments.data();
        c.vk.vkCmdBeginRendering(cmd,&info);
        VkViewport viewport{0,0,float(extent.width),float(extent.height),0,1}; VkRect2D scissor{{0,0},extent};
        c.vk.vkCmdSetViewport(cmd,0,1,&viewport); c.vk.vkCmdSetScissor(cmd,0,1,&scissor);
        c.vk.vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        c.vk.vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);
        c.vk.vkCmdDraw(cmd,3,1,0,0); c.vk.vkCmdEndRendering(cmd);
        for(auto* target:targets) transition(cmd,*target,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    const VulkanImage& render(const VulkanEffectsFrame& f) {
        if(!f.color || !f.depth || !f.normal || !f.extent.width || !f.extent.height || !f.output_extent.width || !f.output_extent.height) throw std::runtime_error("Vulkan effects: invalid world targets");
        if(!f.settings.enhanced) { valid=false; return *f.color; }
        const bool ms=f.ms_motion && f.ms_normal && f.ms_previous_normal;
        if(ms&&!f.artist_mask&&!f.ms_color)throw std::runtime_error("Vulkan effects: MS authored-mask fallback requires retained float world color");
        bool rt=c.ray_query && f.settings.ray_tracing && !f.instances.empty() &&
            (f.settings.shadows>0 || f.settings.ambient_occlusion>0 || f.settings.reflections>0 || f.settings.indirect_lighting>0);
        if(rt) ensure_ray_capacity(f.textures.size());
        if(!same(effects.extent,f.extent) || !same(output.extent,f.output_extent)) valid=false;
        image(effects,f.extent); image(reflected,f.extent); image(output,f.output_extent);
        for(unsigned i=0;i<2;++i) {
            image(history[i],f.output_extent); image(geometry[i],f.output_extent,VK_FORMAT_R32G32B32A32_SFLOAT);
            image(base[i],f.output_extent,VK_FORMAT_R32G32B32A32_SFLOAT); image(reflection[i],f.output_extent);
            for(auto* target:{&history[i],&geometry[i],&base[i],&reflection[i]}) initialize_history(f.command,*target);
        }
        if(clear_cache) { for(auto& entry:cache) destroy(entry.second.as); cache.clear(); destroy(tlas); tlas_count=0; clear_cache=false; }
        if(rt) build_scene(f);
        Parameters p{}; p.current_vp=multiply(f.projection,f.view); p.inverse_vp=inverse(p.current_vp);
        Mat4 inverse_view=inverse(f.view); p.camera={inverse_view.values[12],inverse_view.values[13],inverse_view.values[14],0};
        p.camera[3]=camera_ray_basis(p,f.view,f.projection)?1.f:0.f;
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
        p.jitter={f.jitter[0]/f.extent.width,f.jitter[1]/f.extent.height,0,0};
        p.strengths={f.settings.ambient_occlusion,f.settings.reflections,rt?f.settings.shadows:0,rt?f.settings.indirect_lighting:0};
        // Sharpening is a persisted OpenGL/GLES setting, not a Vulkan effect.
        p.post={f.settings.exposure,f.settings.bloom,f.artist_mask?1.f:0.f,f.settings.roughness};
        const bool history_eligible=valid&&f.history_valid&&(f.settings.temporal_aa||rt);
        p.temporal={history_eligible?1.f:0.f,f.settings.temporal_aa?1.f:0.f,float(frame_index%4096),f.motion&&f.previous_normal?1.f:0.f};
        p.previous_vp=history_eligible?f.previous_view_projection:p.current_vp;
        p.inverse_previous_vp=history_eligible?inverse(f.previous_view_projection):p.inverse_vp;
        p.previous_ray_basis_x=history_eligible?previous_ray_basis_x:p.ray_basis_x;
        p.previous_ray_basis_y=history_eligible?previous_ray_basis_y:p.ray_basis_y;
        p.previous_ray_basis_z=history_eligible?previous_ray_basis_z:p.ray_basis_z;
        p.previous_camera=history_eligible?previous_camera:p.camera;
        std::memcpy(uniform.mapped,&p,sizeof p);
        const VkDescriptorSet effect_descriptor=rt?(ms?ray_ms_set:ray_set):(ms?effect_ms_set:effect_set);
        const VkPipeline effect_shader=rt?(ms?ray_ms_pipeline:ray_pipeline):(ms?effect_ms_pipeline:effect_pipeline);
        const VkPipelineLayout effect_pass_layout=rt?(ms?ray_ms_pipeline_layout:ray_pipeline_layout):(ms?effect_ms_pipeline_layout:effect_pipeline_layout);
        descriptors(f,effect_descriptor,rt,false,ping,false,ms);
        VulkanImage* effect_targets[]{&effects,&reflected}; pass(f.command,effect_targets,effect_shader,effect_pass_layout,effect_descriptor);
        if(rt) ++query_count;
        descriptors(f,temporal_set,false,true,ping);
        unsigned next=1-ping; VulkanImage* temporal_targets[]{&history[next],&geometry[next],&base[next],&reflection[next]};
        pass(f.command,temporal_targets,temporal_pipeline,temporal_pipeline_layout,temporal_set);
        descriptors(f,display_set,false,false,next,true);
        VulkanImage* display_targets[]{&output}; pass(f.command,display_targets,display_pipeline,display_pipeline_layout,display_set);
        ping=next; valid=true; previous_settings=f.settings; previous_vp=p.current_vp;
        previous_camera=p.camera; previous_ray_basis_x=p.ray_basis_x;
        previous_ray_basis_y=p.ray_basis_y; previous_ray_basis_z=p.ray_basis_z; ++frame_index;
        return output;
    }
    void cleanup() noexcept {
        for(auto& entry:cache) destroy(entry.second.as);
        destroy(tlas); destroy(scratch); destroy(instances); destroy(materials); destroy(uniform);
        destroy(effects); destroy(reflected); destroy(output);
        for(auto& i:history) destroy(i); for(auto& i:geometry) destroy(i);
        for(auto& i:base) destroy(i); for(auto& i:reflection) destroy(i);
        destroy_ray_pipeline();
        for(auto p:{effect_pipeline,effect_ms_pipeline,temporal_pipeline,display_pipeline}) if(p) c.vk.vkDestroyPipeline(c.device,p,nullptr);
        for(auto p:{effect_pipeline_layout,effect_ms_pipeline_layout,temporal_pipeline_layout,display_pipeline_layout}) if(p) c.vk.vkDestroyPipelineLayout(c.device,p,nullptr);
        if(pool) c.vk.vkDestroyDescriptorPool(c.device,pool,nullptr);
        for(auto l:{effect_layout,effect_ms_layout,temporal_layout,display_layout}) if(l) c.vk.vkDestroyDescriptorSetLayout(c.device,l,nullptr);
        if(sampler) c.vk.vkDestroySampler(c.device,sampler,nullptr);
        if(nearest_sampler) c.vk.vkDestroySampler(c.device,nearest_sampler,nullptr);
    }
};
VulkanEffects::VulkanEffects(VulkanContext& c):impl_(std::make_unique<Impl>(c)) {}
VulkanEffects::~VulkanEffects()=default;
const VulkanImage& VulkanEffects::render(const VulkanEffectsFrame& f) { return impl_->render(f); }
void VulkanEffects::reset_history() noexcept { impl_->valid=false; impl_->clear_cache=true; }
std::uint64_t VulkanEffects::acceleration_builds() const noexcept { return impl_->build_count; }
std::uint64_t VulkanEffects::ray_query_frames() const noexcept { return impl_->query_count; }
}
