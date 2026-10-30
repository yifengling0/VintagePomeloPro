/* SPDX-License-Identifier: MIT
 * Native-only BC resource layer, enabled by the HAP native-FEX WSI after
 * capability checks. Ordinary Wine WSI and NativeBuffer display stay unchanged.
 */
#include <limits>
#include "native_bc_spirv.h"
#include "native_bc_sizes.h"
namespace nativebc {
struct Format {int id;VkFormat backing;uint32_t blockBytes,outputBytes;bool compressed;};
static Format format(VkFormat f) {
    switch(f) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:return {0,VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK,8,8,true};
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:return {0,VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK,8,8,true};
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:return {1,VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,8,8,true};
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:return {1,VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK,8,8,true};
    case VK_FORMAT_BC2_UNORM_BLOCK:return {2,VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,16,16,true};
    case VK_FORMAT_BC2_SRGB_BLOCK:return {2,VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK,16,16,true};
    case VK_FORMAT_BC3_UNORM_BLOCK:return {3,VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,16,16,true};
    case VK_FORMAT_BC3_SRGB_BLOCK:return {3,VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK,16,16,true};
    case VK_FORMAT_BC4_UNORM_BLOCK:return {4,VK_FORMAT_R8_UNORM,8,1,false};
    case VK_FORMAT_BC4_SNORM_BLOCK:return {5,VK_FORMAT_R8_SNORM,8,1,false};
    case VK_FORMAT_BC5_UNORM_BLOCK:return {6,VK_FORMAT_R8G8_UNORM,16,2,false};
    case VK_FORMAT_BC5_SNORM_BLOCK:return {7,VK_FORMAT_R8G8_SNORM,16,2,false};
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:return {8,VK_FORMAT_R16G16B16A16_SFLOAT,16,8,false};
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:return {9,VK_FORMAT_R16G16B16A16_SFLOAT,16,8,false};
    case VK_FORMAT_BC7_UNORM_BLOCK:return {10,VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,16,16,true};
    case VK_FORMAT_BC7_SRGB_BLOCK:return {10,VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK,16,16,true};
    default:return {-1,f,0,0,false};
    }
}
struct Chain {
    std::vector<std::unique_ptr<uint8_t[]>> blocks;void* head=nullptr;bool valid=true;
    explicit Chain(const void* input) {
        VkBaseOutStructure* previous=nullptr;unsigned count=0;
        for(auto node=static_cast<const VkBaseInStructure*>(input);node;node=node->pNext) {
            size_t bytes=native_bc_struct_size(node->sType);
            if(!bytes||++count>128){valid=false;return;}
            auto storage=std::make_unique<uint8_t[]>(bytes);memcpy(storage.get(),node,bytes);
            auto copy=reinterpret_cast<VkBaseOutStructure*>(storage.get());copy->pNext=nullptr;
            if(previous)previous->pNext=copy;else head=copy;previous=copy;blocks.push_back(std::move(storage));
        }
    }
    template<class T>T* find(VkStructureType type){for(auto n=static_cast<VkBaseOutStructure*>(head);n;n=n->pNext)if(n->sType==type)return reinterpret_cast<T*>(n);return nullptr;}
    template<class T>T* prepend(VkStructureType type) {
        auto storage=std::make_unique<uint8_t[]>(sizeof(T));memset(storage.get(),0,sizeof(T));
        auto node=reinterpret_cast<T*>(storage.get());node->sType=type;node->pNext=head;head=node;blocks.push_back(std::move(storage));return node;
    }
};
static std::map<VkPhysicalDevice,bool> supported;
static bool support(VkPhysicalDevice physical) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto known=supported.find(physical);if(known!=supported.end())return known->second;
    auto instance=physicalInstance(physical);
    VkPhysicalDeviceBufferDeviceAddressFeatures address{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&address};
    instanceProc<PFN_vkGetPhysicalDeviceFeatures2>(instance,"vkGetPhysicalDeviceFeatures2")(physical,&features);
    bool ok=!features.features.textureCompressionBC&&features.features.textureCompressionETC2&&address.bufferDeviceAddress;
    uint32_t count=0;auto families=instanceProc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(instance,"vkGetPhysicalDeviceQueueFamilyProperties");
    families(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> queues(count);families(physical,&count,queues.data());
    // Meta work needs compute. Do not expose the emulation on devices where a
    // valid transfer-only command pool could reach an unsupported dispatch.
    for(const auto& q:queues)if((q.queueFlags&VK_QUEUE_TRANSFER_BIT)&&!(q.queueFlags&VK_QUEUE_COMPUTE_BIT))ok=false;
    for(int f=VK_FORMAT_BC1_RGB_UNORM_BLOCK;ok&&f<=VK_FORMAT_BC7_SRGB_BLOCK;f++) {
        auto desc=format(static_cast<VkFormat>(f));if(desc.id<0){ok=false;break;}
        VkFormatProperties p{};instanceProc<PFN_vkGetPhysicalDeviceFormatProperties>(instance,"vkGetPhysicalDeviceFormatProperties")(physical,desc.backing,&p);
        const VkFormatFeatureFlags need=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        ok=(p.optimalTilingFeatures&need)==need;
    }
    supported[physical]=ok;fprintf(stderr,"GNU_NATIVE_BC capability_ready=%u mode=lossy-etc2 retained_original_blocks=1\n",ok);return ok;
}
struct DeviceInfo {
    Chain chain;VkDeviceCreateInfo info;VkPhysicalDeviceFeatures enabled{};bool valid;std::vector<const char*> extensions;
    DeviceInfo(VkPhysicalDevice physical,const VkDeviceCreateInfo& src):chain(src.pNext),info(src),valid(chain.valid) {
        if(!support(physical))return;
        info.pNext=chain.head;
        if(src.pEnabledFeatures){enabled=*src.pEnabledFeatures;enabled.textureCompressionBC=VK_FALSE;enabled.textureCompressionETC2=VK_TRUE;info.pEnabledFeatures=&enabled;}
        auto base=chain.find<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        if(base) {
            base->features.textureCompressionBC=VK_FALSE;base->features.textureCompressionETC2=VK_TRUE;
        } else if(!src.pEnabledFeatures){enabled.textureCompressionETC2=VK_TRUE;info.pEnabledFeatures=&enabled;}
        // Unity may request Vulkan 1.1 even on a 1.2 physical device. Enable
        // the promoted extension explicitly and use its KHR command alias;
        // core 1.2 entry points may legally be null in such a device.
        for(unsigned i=0;i<src.enabledExtensionCount;i++)extensions.push_back(src.ppEnabledExtensionNames[i]);
        if(std::none_of(extensions.begin(),extensions.end(),[](const char* s){return !strcmp(s,VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);}))extensions.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        info.enabledExtensionCount=uint32_t(extensions.size());info.ppEnabledExtensionNames=extensions.data();
        if(auto v12=chain.find<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES))v12->bufferDeviceAddress=VK_TRUE;
        else {auto bda=chain.find<VkPhysicalDeviceBufferDeviceAddressFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
            if(!bda)bda=chain.prepend<VkPhysicalDeviceBufferDeviceAddressFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);bda->bufferDeviceAddress=VK_TRUE;}
        info.pNext=chain.head;
    }
};
static bool active(VkDevice device){std::lock_guard<std::recursive_mutex> lock(stateMutex);auto d=devices.find(device);return d!=devices.end()&&support(d->second.physical);}
static PFN_vkGetBufferDeviceAddress addressProc(VkDevice d) {
    auto proc=deviceProc<PFN_vkGetBufferDeviceAddress>(d,"vkGetBufferDeviceAddress");
    return proc?proc:deviceProc<PFN_vkGetBufferDeviceAddress>(d,"vkGetBufferDeviceAddressKHR");
}
struct MemoryStats{unsigned allocations=0,failures=0;VkDeviceSize peak=0;};
static std::map<VkDevice,MemoryStats> allocationStats;
static void reportMemory(VkDevice d,VkResult rc,VkDeviceSize requested) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto& s=allocationStats[d];
    if(rc==VK_SUCCESS)++s.allocations;else ++s.failures;
    VkDeviceSize live=0;for(const auto& m:mappings)if(m.second.owner==d)live+=m.second.bytes;
    s.peak=std::max(s.peak,live);
    if(rc!=VK_SUCCESS||s.allocations<=4||s.allocations%64==0)
        fprintf(stderr,"GNU_NATIVE_BC memory requested=%" PRIu64 " result=%d allocations=%u live_bytes=%" PRIu64 " peak_bytes=%" PRIu64 " failures=%u display_readbacks=0\n",requested,rc,s.allocations,live,s.peak,s.failures);
}
struct Level {uint32_t width,height,bx,by;VkDeviceSize offset,layerBytes;};
struct Image {VkDevice device;VkFormat original;Format desc;VkBuffer blocks=VK_NULL_HANDLE;VkMemoryRequirements req{};
    VkDeviceSize blockOffset=0;VkDeviceMemory memory=VK_NULL_HANDLE;std::vector<Level> levels;uint32_t layers;};
static std::map<VkImage,Image> images;
struct MemoryInfo {
    Chain chain;VkMemoryAllocateInfo info;bool valid;
    MemoryInfo(VkDevice device,const VkMemoryAllocateInfo& src):chain(src.pNext),info(src),valid(chain.valid) {
        std::lock_guard<std::recursive_mutex> lock(stateMutex);
        if(!active(device))return;
        auto flags=chain.find<VkMemoryAllocateFlagsInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO);
        if(!flags)flags=chain.prepend<VkMemoryAllocateFlagsInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO);
        flags->flags|=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        if(auto dedicated=chain.find<VkMemoryDedicatedAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO))
            if(images.count(dedicated->image))dedicated->image=VK_NULL_HANDLE;
        info.pNext=chain.head;
    }
};
static void VKAPI_CALL getFeatures(VkPhysicalDevice p,VkPhysicalDeviceFeatures* out) {
    instanceProc<PFN_vkGetPhysicalDeviceFeatures>(physicalInstance(p),"vkGetPhysicalDeviceFeatures")(p,out);
    if(support(p))out->textureCompressionBC=VK_TRUE;
}
static void VKAPI_CALL getFeatures2(VkPhysicalDevice p,VkPhysicalDeviceFeatures2* out) {
    instanceProc<PFN_vkGetPhysicalDeviceFeatures2>(physicalInstance(p),"vkGetPhysicalDeviceFeatures2")(p,out);
    if(support(p))out->features.textureCompressionBC=VK_TRUE;
}
static VkFormatFeatureFlags featureMask(VkFormatFeatureFlags f) {
    return (f&(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_BLIT_SRC_BIT))|
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
}
static void VKAPI_CALL getFormat(VkPhysicalDevice p,VkFormat f,VkFormatProperties* out) {
    auto desc=format(f);bool use=desc.id>=0&&support(p);
    instanceProc<PFN_vkGetPhysicalDeviceFormatProperties>(physicalInstance(p),"vkGetPhysicalDeviceFormatProperties")(p,use?desc.backing:f,out);
    if(use){out->linearTilingFeatures=0;out->bufferFeatures=0;out->optimalTilingFeatures=featureMask(out->optimalTilingFeatures);}
}
static void VKAPI_CALL getFormat2(VkPhysicalDevice p,VkFormat f,VkFormatProperties2* out) {
    auto desc=format(f);bool use=desc.id>=0&&support(p);
    instanceProc<PFN_vkGetPhysicalDeviceFormatProperties2>(physicalInstance(p),"vkGetPhysicalDeviceFormatProperties2")(p,use?desc.backing:f,out);
    if(use){out->formatProperties.linearTilingFeatures=0;out->formatProperties.bufferFeatures=0;out->formatProperties.optimalTilingFeatures=featureMask(out->formatProperties.optimalTilingFeatures);
        for(auto n=static_cast<VkBaseOutStructure*>(out->pNext);n;n=n->pNext)if(n->sType==VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
            auto props=reinterpret_cast<VkFormatProperties3*>(n);props->linearTilingFeatures=0;props->bufferFeatures=0;props->optimalTilingFeatures=out->formatProperties.optimalTilingFeatures;
        }}
}
static bool allowed(VkImageType type,VkImageTiling tiling,VkImageUsageFlags usage,VkImageCreateFlags flags) {
    const VkImageUsageFlags usages=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageCreateFlags allowedFlags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT|VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    return type==VK_IMAGE_TYPE_2D&&tiling==VK_IMAGE_TILING_OPTIMAL&&!(usage&~usages)&&!(flags&~allowedFlags);
}
static VkResult VKAPI_CALL getImageFormat(VkPhysicalDevice p,VkFormat f,VkImageType type,VkImageTiling tiling,VkImageUsageFlags usage,VkImageCreateFlags flags,VkImageFormatProperties* out) {
    auto desc=format(f);bool use=desc.id>=0&&support(p);
    if(use&&!allowed(type,tiling,usage,flags))return VK_ERROR_FORMAT_NOT_SUPPORTED;
    auto rc=instanceProc<PFN_vkGetPhysicalDeviceImageFormatProperties>(physicalInstance(p),"vkGetPhysicalDeviceImageFormatProperties")(p,use?desc.backing:f,type,tiling,usage|(use?VK_IMAGE_USAGE_TRANSFER_DST_BIT:0),flags,out);
    if(use&&rc==VK_SUCCESS)out->sampleCounts=VK_SAMPLE_COUNT_1_BIT;
    return rc;
}
static VkResult VKAPI_CALL getImageFormat2(VkPhysicalDevice p,const VkPhysicalDeviceImageFormatInfo2* src,VkImageFormatProperties2* out) {
    auto desc=format(src->format);if(desc.id<0||!support(p))return instanceProc<PFN_vkGetPhysicalDeviceImageFormatProperties2>(physicalInstance(p),"vkGetPhysicalDeviceImageFormatProperties2")(p,src,out);
    if(!allowed(src->type,src->tiling,src->usage,src->flags))return VK_ERROR_FORMAT_NOT_SUPPORTED;
    Chain chain(src->pNext);if(!chain.valid)return VK_ERROR_FORMAT_NOT_SUPPORTED;
    std::vector<VkFormat> formats;
    for(auto node=static_cast<VkBaseOutStructure*>(chain.head);node;node=node->pNext) {
        if(node->sType==VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            auto list=reinterpret_cast<VkImageFormatListCreateInfo*>(node);
            for(unsigned i=0;i<list->viewFormatCount;i++){auto view=format(list->pViewFormats[i]);if(view.id!=desc.id)return VK_ERROR_FORMAT_NOT_SUPPORTED;formats.push_back(view.backing);}
            list->pViewFormats=formats.data();
        } else if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO) {
            // The companion native receiver cannot import compressed images.
            if(reinterpret_cast<VkPhysicalDeviceExternalImageFormatInfo*>(node)->handleType)return VK_ERROR_FORMAT_NOT_SUPPORTED;
        } else if(node->sType==VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO) {
            if(reinterpret_cast<VkImageViewUsageCreateInfo*>(node)->usage&~(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT))return VK_ERROR_FORMAT_NOT_SUPPORTED;
        } else return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    auto info=*src;info.format=desc.backing;info.usage|=VK_IMAGE_USAGE_TRANSFER_DST_BIT;info.pNext=chain.head;
    auto rc=instanceProc<PFN_vkGetPhysicalDeviceImageFormatProperties2>(physicalInstance(p),"vkGetPhysicalDeviceImageFormatProperties2")(p,&info,out);
    if(rc==VK_SUCCESS)out->imageFormatProperties.sampleCounts=VK_SAMPLE_COUNT_1_BIT;
    return rc;
}
static VkResult VKAPI_CALL createImage(VkDevice device,const VkImageCreateInfo* src,const VkAllocationCallbacks* alloc,VkImage* out) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto desc=format(src->format);
    if(desc.id<0||!active(device))return deviceProc<PFN_vkCreateImage>(device,"vkCreateImage")(device,src,alloc,out);
    if(!allowed(src->imageType,src->tiling,src->usage,src->flags)||src->samples!=VK_SAMPLE_COUNT_1_BIT||!src->extent.width||!src->extent.height||src->extent.depth!=1||!src->mipLevels||src->mipLevels>32||!src->arrayLayers)return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkImageCreateInfo info=*src;info.format=desc.backing;info.usage|=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};std::vector<VkFormat> formats;
    if(src->pNext) {
        auto node=static_cast<const VkBaseInStructure*>(src->pNext);
        if(node->sType!=VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO||node->pNext)return VK_ERROR_FORMAT_NOT_SUPPORTED;
        auto input=reinterpret_cast<const VkImageFormatListCreateInfo*>(node);list=*input;
        for(unsigned i=0;i<input->viewFormatCount;i++){auto view=format(input->pViewFormats[i]);if(view.id!=desc.id)return VK_ERROR_FORMAT_NOT_SUPPORTED;formats.push_back(view.backing);}
        list.pViewFormats=formats.data();info.pNext=&list;
    }
    Image image{};image.device=device;image.original=src->format;image.desc=desc;image.layers=src->arrayLayers;
    VkDeviceSize size=0;
    for(unsigned mip=0;mip<src->mipLevels;mip++) {
        uint32_t w=std::max(1u,src->extent.width>>mip),h=std::max(1u,src->extent.height>>mip),bx=(w+3)/4,by=(h+3)/4;
        VkDeviceSize layer=VkDeviceSize(bx)*by*desc.blockBytes;
        if(layer>UINT64_MAX/src->arrayLayers||size>UINT64_MAX-layer*src->arrayLayers)return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        image.levels.push_back({w,h,bx,by,size,layer});size+=layer*src->arrayLayers;
    }
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.size=size;
    buffer.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    buffer.sharingMode=src->sharingMode;buffer.queueFamilyIndexCount=src->queueFamilyIndexCount;buffer.pQueueFamilyIndices=src->pQueueFamilyIndices;
    auto rc=deviceProc<PFN_vkCreateBuffer>(device,"vkCreateBuffer")(device,&buffer,nullptr,&image.blocks);if(rc!=VK_SUCCESS)return rc;
    rc=deviceProc<PFN_vkCreateImage>(device,"vkCreateImage")(device,&info,alloc,out);
    if(rc!=VK_SUCCESS){deviceProc<PFN_vkDestroyBuffer>(device,"vkDestroyBuffer")(device,image.blocks,nullptr);return rc;}
    VkMemoryRequirements backing{},shadow{};
    deviceProc<PFN_vkGetImageMemoryRequirements>(device,"vkGetImageMemoryRequirements")(device,*out,&backing);
    deviceProc<PFN_vkGetBufferMemoryRequirements>(device,"vkGetBufferMemoryRequirements")(device,image.blocks,&shadow);
    VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};VkMemoryRequirements2 req2{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,&dedicated};
    VkImageMemoryRequirementsInfo2 query{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,nullptr,*out};
    deviceProc<PFN_vkGetImageMemoryRequirements2>(device,"vkGetImageMemoryRequirements2")(device,&query,&req2);
    image.blockOffset=(backing.size+shadow.alignment-1)&~(shadow.alignment-1);
    image.req={image.blockOffset+shadow.size,std::max(backing.alignment,shadow.alignment),backing.memoryTypeBits&shadow.memoryTypeBits};
    if(dedicated.requiresDedicatedAllocation||image.req.size<image.blockOffset||!image.req.memoryTypeBits) {
        deviceProc<PFN_vkDestroyImage>(device,"vkDestroyImage")(device,*out,alloc);deviceProc<PFN_vkDestroyBuffer>(device,"vkDestroyBuffer")(device,image.blocks,nullptr);return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    static unsigned reports=0;
    if(reports++<24||reports%128==0)fprintf(stderr,"GNU_NATIVE_BC image format=%u backing=%u size=%ux%u mips=%u layers=%u backing_bytes=%" PRIu64 " original_bytes=%" PRIu64 " total=%" PRIu64 "\n",src->format,desc.backing,src->extent.width,src->extent.height,src->mipLevels,src->arrayLayers,backing.size,shadow.size,image.req.size);
    images.emplace(*out,std::move(image));return VK_SUCCESS;
}
static void VKAPI_CALL destroyImage(VkDevice device,VkImage image,const VkAllocationCallbacks* alloc) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto it=images.find(image);
    if(it!=images.end()){deviceProc<PFN_vkDestroyBuffer>(device,"vkDestroyBuffer")(device,it->second.blocks,nullptr);images.erase(it);}
    deviceProc<PFN_vkDestroyImage>(device,"vkDestroyImage")(device,image,alloc);
}
static void VKAPI_CALL getRequirements(VkDevice device,VkImage image,VkMemoryRequirements* out) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto it=images.find(image);
    if(it!=images.end())*out=it->second.req;else deviceProc<PFN_vkGetImageMemoryRequirements>(device,"vkGetImageMemoryRequirements")(device,image,out);
}
static void VKAPI_CALL getRequirements2(VkDevice device,const VkImageMemoryRequirementsInfo2* info,VkMemoryRequirements2* out) {
    deviceProc<PFN_vkGetImageMemoryRequirements2>(device,"vkGetImageMemoryRequirements2")(device,info,out);
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto it=images.find(info->image);
    if(it!=images.end()){out->memoryRequirements=it->second.req;for(auto n=static_cast<VkBaseOutStructure*>(out->pNext);n;n=n->pNext)
        if(n->sType==VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS){auto d=reinterpret_cast<VkMemoryDedicatedRequirements*>(n);d->requiresDedicatedAllocation=VK_FALSE;d->prefersDedicatedAllocation=VK_FALSE;}}
}
static VkResult VKAPI_CALL bindImage(VkDevice device,VkImage image,VkDeviceMemory memory,VkDeviceSize offset) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto it=images.find(image);
    auto rc=deviceProc<PFN_vkBindImageMemory>(device,"vkBindImageMemory")(device,image,memory,offset);
    if(rc==VK_SUCCESS&&it!=images.end()) {
        rc=deviceProc<PFN_vkBindBufferMemory>(device,"vkBindBufferMemory")(device,it->second.blocks,memory,offset+it->second.blockOffset);
        if(rc==VK_SUCCESS)it->second.memory=memory;
    }return rc;
}
static VkResult VKAPI_CALL bindImages2(VkDevice d,uint32_t count,const VkBindImageMemoryInfo* infos) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);bool emulated=false;
    for(unsigned i=0;i<count;i++)emulated|=images.count(infos[i].image)!=0;
    if(!emulated)return deviceProc<PFN_vkBindImageMemory2>(d,"vkBindImageMemory2")(d,count,infos);
    for(unsigned i=0;i<count;i++) {
        if(infos[i].pNext&&images.count(infos[i].image))return VK_ERROR_FEATURE_NOT_PRESENT;
        auto rc=images.count(infos[i].image)?bindImage(d,infos[i].image,infos[i].memory,infos[i].memoryOffset):deviceProc<PFN_vkBindImageMemory2>(d,"vkBindImageMemory2")(d,1,&infos[i]);
        if(rc!=VK_SUCCESS)return rc;
    }return VK_SUCCESS;
}
static VkResult VKAPI_CALL createView(VkDevice d,const VkImageViewCreateInfo* src,const VkAllocationCallbacks* a,VkImageView* out) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto info=*src;auto it=images.find(info.image);
    if(it!=images.end()){auto desc=format(info.format);if(desc.id!=it->second.desc.id)return VK_ERROR_FORMAT_NOT_SUPPORTED;info.format=desc.backing;}
    return deviceProc<PFN_vkCreateImageView>(d,"vkCreateImageView")(d,&info,a,out);
}
struct Scratch {VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkDeviceSize bytes=0;VkDeviceAddress address=0;};
struct ReplayLayout {
    VkDevice device;VkPipelineLayout handle;
    ~ReplayLayout(){deviceProc<PFN_vkDestroyPipelineLayout>(device,"vkDestroyPipelineLayout")(device,handle,nullptr);}
};
static std::map<VkPipelineLayout,std::shared_ptr<ReplayLayout>> replayLayouts;
static VkResult VKAPI_CALL createLayout(VkDevice d,const VkPipelineLayoutCreateInfo* src,const VkAllocationCallbacks* a,VkPipelineLayout* out) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto create=deviceProc<PFN_vkCreatePipelineLayout>(d,"vkCreatePipelineLayout");
    auto rc=create(d,src,a,out);if(rc!=VK_SUCCESS||!active(d)||!src->pushConstantRangeCount)return rc;
    // Push-constant compatibility depends on ranges alone. Keep an owned
    // compatible layout alive for recorded commands, even after the app's
    // descriptor layout/pipeline layout has been destroyed.
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.pushConstantRangeCount=src->pushConstantRangeCount;info.pPushConstantRanges=src->pPushConstantRanges;
    VkPipelineLayout handle=VK_NULL_HANDLE;rc=create(d,&info,nullptr,&handle);
    if(rc!=VK_SUCCESS){deviceProc<PFN_vkDestroyPipelineLayout>(d,"vkDestroyPipelineLayout")(d,*out,a);*out=VK_NULL_HANDLE;return rc;}
    auto replay=std::make_shared<ReplayLayout>();replay->device=d;replay->handle=handle;replayLayouts[*out]=std::move(replay);return rc;
}
static void VKAPI_CALL destroyLayout(VkDevice d,VkPipelineLayout handle,const VkAllocationCallbacks* a) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);replayLayouts.erase(handle);
    deviceProc<PFN_vkDestroyPipelineLayout>(d,"vkDestroyPipelineLayout")(d,handle,a);
}
struct Push {std::shared_ptr<ReplayLayout> layout;VkShaderStageFlags stages;uint32_t offset;std::vector<uint8_t> bytes;};
struct Command {VkDevice device;VkCommandPool pool;VkPipeline pipeline=VK_NULL_HANDLE;std::vector<Push> pushes;std::vector<Scratch> scratch;VkResult error=VK_SUCCESS;};
struct Tools {VkShaderModule shader=VK_NULL_HANDLE;VkPipelineLayout layout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;};
static std::map<VkCommandBuffer,Command> commands;
static std::map<VkDevice,Tools> tools;
static void error(Command& c,VkResult result,const char* operation) {
    if(c.error==VK_SUCCESS){c.error=result;fprintf(stderr,"GNU_NATIVE_BC command_error=%d operation=%s\n",result,operation);}
}
static void clear(Command& c) {
    for(auto& s:c.scratch) {
        if(s.buffer)deviceProc<PFN_vkDestroyBuffer>(c.device,"vkDestroyBuffer")(c.device,s.buffer,nullptr);
        if(s.memory)deviceProc<PFN_vkFreeMemory>(c.device,"vkFreeMemory")(c.device,s.memory,nullptr);
    }
    c.scratch.clear();c.pushes.clear();c.pipeline=VK_NULL_HANDLE;c.error=VK_SUCCESS;
}
static VkResult VKAPI_CALL allocateCommands(VkDevice device,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out) {
    auto rc=nativeAllocateCommandBuffers(device,info,out);
    if(rc==VK_SUCCESS){std::lock_guard<std::recursive_mutex> lock(stateMutex);for(unsigned i=0;i<info->commandBufferCount;i++)commands.emplace(out[i],Command{device,info->commandPool});}
    return rc;
}
static void VKAPI_CALL freeCommands(VkDevice device,VkCommandPool pool,uint32_t count,const VkCommandBuffer* handles) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    for(unsigned i=0;i<count;i++){auto it=commands.find(handles[i]);if(it!=commands.end()){clear(it->second);commands.erase(it);}}
    nativeFreeCommandBuffers(device,pool,count,handles);
}
static void VKAPI_CALL destroyPool(VkDevice device,VkCommandPool pool,const VkAllocationCallbacks* alloc) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    for(auto it=commands.begin();it!=commands.end();)if(it->second.device==device&&it->second.pool==pool){clear(it->second);it=commands.erase(it);}else ++it;
    nativeDestroyCommandPool(device,pool,alloc);
}
static VkResult VKAPI_CALL resetPool(VkDevice device,VkCommandPool pool,VkCommandPoolResetFlags flags) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto rc=deviceProc<PFN_vkResetCommandPool>(device,"vkResetCommandPool")(device,pool,flags);
    if(rc==VK_SUCCESS)for(auto& pair:commands)if(pair.second.device==device&&pair.second.pool==pool)clear(pair.second);
    return rc;
}
static VkResult VKAPI_CALL begin(VkCommandBuffer cb,const VkCommandBufferBeginInfo* info) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto& c=commands.at(cb);
    auto rc=deviceProc<PFN_vkBeginCommandBuffer>(c.device,"vkBeginCommandBuffer")(cb,info);if(rc==VK_SUCCESS)clear(c);return rc;
}
static VkResult VKAPI_CALL reset(VkCommandBuffer cb,VkCommandBufferResetFlags flags) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto& c=commands.at(cb);
    auto rc=deviceProc<PFN_vkResetCommandBuffer>(c.device,"vkResetCommandBuffer")(cb,flags);if(rc==VK_SUCCESS)clear(c);return rc;
}
static VkResult VKAPI_CALL end(VkCommandBuffer cb) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto& c=commands.at(cb);
    auto rc=deviceProc<PFN_vkEndCommandBuffer>(c.device,"vkEndCommandBuffer")(cb);return rc==VK_SUCCESS?c.error:rc;
}
static void VKAPI_CALL bindPipeline(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipeline pipeline) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto& c=commands.at(cb);
    if(point==VK_PIPELINE_BIND_POINT_COMPUTE)c.pipeline=pipeline;
    deviceProc<PFN_vkCmdBindPipeline>(c.device,"vkCmdBindPipeline")(cb,point,pipeline);
}
static void VKAPI_CALL push(VkCommandBuffer cb,VkPipelineLayout layout,VkShaderStageFlags stages,uint32_t offset,uint32_t size,const void* data) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto& c=commands.at(cb);
    // Keep the latest update for every exact key, preserving update order and
    // every shader stage. No fixed-size ring drops live application state.
    auto found=replayLayouts.find(layout);
    if(active(c.device)&&found==replayLayouts.end()){error(c,VK_ERROR_VALIDATION_FAILED_EXT,"untracked-push-layout");return;}
    if(found!=replayLayouts.end()) {
        auto replay=found->second;
        c.pushes.erase(std::remove_if(c.pushes.begin(),c.pushes.end(),[&](const Push& p){return p.layout==replay&&p.stages==stages&&p.offset==offset&&p.bytes.size()==size;}),c.pushes.end());
        Push p{replay,stages,offset,{}};p.bytes.assign(static_cast<const uint8_t*>(data),static_cast<const uint8_t*>(data)+size);c.pushes.push_back(std::move(p));
    }
    deviceProc<PFN_vkCmdPushConstants>(c.device,"vkCmdPushConstants")(cb,layout,stages,offset,size,data);
}
static void VKAPI_CALL execute(VkCommandBuffer cb,uint32_t count,const VkCommandBuffer* secondary) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto& c=commands.at(cb);
    deviceProc<PFN_vkCmdExecuteCommands>(c.device,"vkCmdExecuteCommands")(cb,count,secondary);
    // Vulkan leaves primary binding state undefined after secondary execution.
    c.pipeline=VK_NULL_HANDLE;c.pushes.clear();
}
static bool prepare(Command& c) {
    auto& t=tools[c.device];if(t.pipeline)return true;VkResult rc;
    if(!t.shader) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.codeSize=sizeof(native_bc_spirv);info.pCode=native_bc_spirv;
        rc=deviceProc<PFN_vkCreateShaderModule>(c.device,"vkCreateShaderModule")(c.device,&info,nullptr,&t.shader);
        if(rc!=VK_SUCCESS){error(c,rc,"shader");return false;}
    }
    if(!t.layout) {
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,48};VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        info.pushConstantRangeCount=1;info.pPushConstantRanges=&range;
        rc=deviceProc<PFN_vkCreatePipelineLayout>(c.device,"vkCreatePipelineLayout")(c.device,&info,nullptr,&t.layout);
        if(rc!=VK_SUCCESS){error(c,rc,"pipeline-layout");return false;}
    }
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};info.layout=t.layout;
    info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,t.shader,"main",nullptr};
    rc=deviceProc<PFN_vkCreateComputePipelines>(c.device,"vkCreateComputePipelines")(c.device,VK_NULL_HANDLE,1,&info,nullptr,&t.pipeline);
    if(rc!=VK_SUCCESS){error(c,rc,"compute-pipeline");return false;}return true;
}
static Scratch* scratch(Command& c,VkDeviceSize bytes) {
    for(auto& s:c.scratch)if(s.bytes>=bytes)return &s;
    if(!bytes||bytes>512ull*1024*1024){error(c,VK_ERROR_OUT_OF_DEVICE_MEMORY,"scratch-size");return nullptr;}
    Scratch s{};s.bytes=bytes;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=bytes;bi.usage=VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto rc=deviceProc<PFN_vkCreateBuffer>(c.device,"vkCreateBuffer")(c.device,&bi,nullptr,&s.buffer);
    if(rc!=VK_SUCCESS){error(c,rc,"scratch-buffer");return nullptr;}
    VkMemoryRequirements req{};deviceProc<PFN_vkGetBufferMemoryRequirements>(c.device,"vkGetBufferMemoryRequirements")(c.device,s.buffer,&req);
    VkPhysicalDeviceMemoryProperties memory{};auto d=devices.at(c.device);
    instanceProc<PFN_vkGetPhysicalDeviceMemoryProperties>(d.instance,"vkGetPhysicalDeviceMemoryProperties")(d.physical,&memory);
    uint32_t type=memory.memoryTypeCount;
    for(unsigned i=0;i<memory.memoryTypeCount;i++)if((req.memoryTypeBits&(1u<<i))&&(memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}
    if(type==memory.memoryTypeCount)rc=VK_ERROR_FEATURE_NOT_PRESENT;
    else {
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,nullptr,VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,0};
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,&flags,req.size,type};
        rc=deviceProc<PFN_vkAllocateMemory>(c.device,"vkAllocateMemory")(c.device,&ai,nullptr,&s.memory);
    }
    if(rc==VK_SUCCESS)rc=deviceProc<PFN_vkBindBufferMemory>(c.device,"vkBindBufferMemory")(c.device,s.buffer,s.memory,0);
    if(rc==VK_SUCCESS) {
        VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,nullptr,s.buffer};
        auto address=addressProc(c.device);s.address=address?address(c.device,&info):0;
        if(!s.address)rc=VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if(rc!=VK_SUCCESS){deviceProc<PFN_vkDestroyBuffer>(c.device,"vkDestroyBuffer")(c.device,s.buffer,nullptr);if(s.memory)deviceProc<PFN_vkFreeMemory>(c.device,"vkFreeMemory")(c.device,s.memory,nullptr);error(c,rc,"scratch-memory-address");return nullptr;}
    c.scratch.push_back(s);return &c.scratch.back();
}
static void barrier(VkCommandBuffer cb,Command& c) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
    deviceProc<PFN_vkCmdPipelineBarrier>(c.device,"vkCmdPipelineBarrier")(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&b,0,nullptr,0,nullptr);
}
static bool region(Command& c,const Image& image,const VkImageSubresourceLayers& sub,VkOffset3D offset,VkExtent3D extent) {
    if(sub.mipLevel>=image.levels.size()||!sub.layerCount||sub.baseArrayLayer>=image.layers||sub.layerCount>image.layers-sub.baseArrayLayer||
        sub.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT||offset.x<0||offset.y<0||offset.z||extent.depth!=1||!extent.width||!extent.height||
        (offset.x%4)||(offset.y%4)) {error(c,VK_ERROR_VALIDATION_FAILED_EXT,"invalid-bc-region");return false;}
    auto l=image.levels[sub.mipLevel];
    if(uint32_t(offset.x)>l.width||extent.width>l.width-uint32_t(offset.x)||uint32_t(offset.y)>l.height||extent.height>l.height-uint32_t(offset.y)||
        ((extent.width%4)&&uint32_t(offset.x)+extent.width!=l.width)||((extent.height%4)&&uint32_t(offset.y)+extent.height!=l.height)) {
        error(c,VK_ERROR_VALIDATION_FAILED_EXT,"bc-region-bounds");return false;
    }return true;
}
static VkDeviceSize blockStart(const Image& image,const VkImageSubresourceLayers& sub,VkOffset3D offset,unsigned layer) {
    const auto& l=image.levels[sub.mipLevel];return l.offset+(sub.baseArrayLayer+layer)*l.layerBytes+
        (VkDeviceSize(offset.y/4)*l.bx+unsigned(offset.x/4))*image.desc.blockBytes;
}
static void transcode(VkCommandBuffer cb,Command& c,VkImage handle,Image& image,VkImageLayout layout,
                      VkImageSubresourceLayers sub,VkOffset3D offset,VkExtent3D extent) {
    if(c.error!=VK_SUCCESS||!region(c,image,sub,offset,extent)||!prepare(c))return;
    unsigned bx=(extent.width+3)/4,by=(extent.height+3)/4;
    VkDeviceSize row=VkDeviceSize(bx)*(image.desc.compressed?image.desc.outputBytes:4*image.desc.outputBytes);
    VkDeviceSize slice=row*by*(image.desc.compressed?1:4);
    auto temp=scratch(c,slice*sub.layerCount);if(!temp)return;
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,nullptr,image.blocks};
    auto proc=addressProc(c.device);VkDeviceAddress address=proc?proc(c.device,&addressInfo):0;
    if(!address){error(c,VK_ERROR_FEATURE_NOT_PRESENT,"original-block-address");return;}
    auto t=tools.at(c.device);const auto& l=image.levels[sub.mipLevel];
    struct PC {uint64_t src,dst;uint32_t srcRow,srcSlice,dstRow,dstSlice,bx,by,format,flags;};
    static_assert(sizeof(PC)==48,"GLSL PC ABI");
    PC pc{address+blockStart(image,sub,offset,0),temp->address,l.bx*image.desc.blockBytes,
        uint32_t(l.layerBytes),uint32_t(row),uint32_t(slice),bx,by,uint32_t(image.desc.id),0};
    barrier(cb,c);
    deviceProc<PFN_vkCmdBindPipeline>(c.device,"vkCmdBindPipeline")(cb,VK_PIPELINE_BIND_POINT_COMPUTE,t.pipeline);
    deviceProc<PFN_vkCmdPushConstants>(c.device,"vkCmdPushConstants")(cb,t.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),&pc);
    deviceProc<PFN_vkCmdDispatch>(c.device,"vkCmdDispatch")(cb,(bx+7)/8,(by+7)/8,sub.layerCount);
    barrier(cb,c);
    VkBufferImageCopy copy{};copy.imageSubresource=sub;copy.imageOffset=offset;copy.imageExtent=extent;
    if(!image.desc.compressed){copy.bufferRowLength=bx*4;copy.bufferImageHeight=by*4;}
    deviceProc<PFN_vkCmdCopyBufferToImage>(c.device,"vkCmdCopyBufferToImage")(cb,temp->buffer,handle,layout,1,&copy);
    if(c.pipeline)deviceProc<PFN_vkCmdBindPipeline>(c.device,"vkCmdBindPipeline")(cb,VK_PIPELINE_BIND_POINT_COMPUTE,c.pipeline);
    for(const auto& p:c.pushes)deviceProc<PFN_vkCmdPushConstants>(c.device,"vkCmdPushConstants")(cb,p.layout->handle,p.stages,p.offset,uint32_t(p.bytes.size()),p.bytes.data());
    static unsigned uploads=0;if(++uploads<=4||uploads%256==0)fprintf(stderr,"GNU_NATIVE_BC upload=%u shader=all-bc target=%u display_readbacks=0 cpu_pixel_copies=0\n",uploads,image.desc.backing);
}
static void bufferTransfer(VkCommandBuffer cb,Command& c,VkBuffer buffer,Image& image,const VkBufferImageCopy& r,bool upload) {
    if(!region(c,image,r.imageSubresource,r.imageOffset,r.imageExtent))return;
    const auto& l=image.levels[r.imageSubresource.mipLevel];
    uint32_t bx=(r.imageExtent.width+3)/4,by=(r.imageExtent.height+3)/4;
    VkDeviceSize row=VkDeviceSize((r.bufferRowLength?r.bufferRowLength:r.imageExtent.width)+3)/4*image.desc.blockBytes;
    VkDeviceSize slice=row*(((r.bufferImageHeight?r.bufferImageHeight:r.imageExtent.height)+3)/4);
    std::vector<VkBufferCopy> copies;
    for(unsigned layer=0;layer<r.imageSubresource.layerCount;layer++) {
        VkDeviceSize base=blockStart(image,r.imageSubresource,r.imageOffset,layer);
        for(unsigned y=0;y<by;y++) {
            VkDeviceSize original=base+VkDeviceSize(y)*l.bx*image.desc.blockBytes,input=r.bufferOffset+layer*slice+y*row;
            copies.push_back(upload?VkBufferCopy{input,original,VkDeviceSize(bx)*image.desc.blockBytes}:
                                   VkBufferCopy{original,input,VkDeviceSize(bx)*image.desc.blockBytes});
        }
    }
    barrier(cb,c);deviceProc<PFN_vkCmdCopyBuffer>(c.device,"vkCmdCopyBuffer")(cb,upload?buffer:image.blocks,upload?image.blocks:buffer,uint32_t(copies.size()),copies.data());
}
static void VKAPI_CALL copyBufferToImage(VkCommandBuffer cb,VkBuffer buffer,VkImage handle,VkImageLayout layout,uint32_t count,const VkBufferImageCopy* regions) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto& c=commands.at(cb);auto image=images.find(handle);
    if(image==images.end()){nativeCmdCopyBufferToImage(cb,buffer,handle,layout,count,regions);return;}
    for(unsigned i=0;i<count;i++){bufferTransfer(cb,c,buffer,image->second,regions[i],true);transcode(cb,c,handle,image->second,layout,regions[i].imageSubresource,regions[i].imageOffset,regions[i].imageExtent);}
}
static void VKAPI_CALL copyImageToBuffer(VkCommandBuffer cb,VkImage handle,VkImageLayout layout,VkBuffer buffer,uint32_t count,const VkBufferImageCopy* regions) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto& c=commands.at(cb);auto image=images.find(handle);
    if(image==images.end()){deviceProc<PFN_vkCmdCopyImageToBuffer>(c.device,"vkCmdCopyImageToBuffer")(cb,handle,layout,buffer,count,regions);return;}
    for(unsigned i=0;i<count;i++)bufferTransfer(cb,c,buffer,image->second,regions[i],false);
}
static void VKAPI_CALL copyImage(VkCommandBuffer cb,VkImage source,VkImageLayout sourceLayout,VkImage dest,VkImageLayout destLayout,uint32_t count,const VkImageCopy* regions) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto& c=commands.at(cb);auto src=images.find(source),dst=images.find(dest);
    if(src==images.end()&&dst==images.end()){deviceProc<PFN_vkCmdCopyImage>(c.device,"vkCmdCopyImage")(cb,source,sourceLayout,dest,destLayout,count,regions);return;}
    // Raw compressed-to-compressed copies use original blocks, including
    // reinterpretation among equal-sized BC blocks. Never recompress readback.
    if(src==images.end()||dst==images.end()) {
        // Vulkan specifies extent in source texels. A compressed block maps
        // to one uncompressed texel with the same byte size (spec copies.html,
        // "Copying Between Compressed and Uncompressed Images"). Preserve
        // exact original bits and never route these through lossy ETC2.
        for(unsigned i=0;i<count;i++) {
            const auto& r=regions[i];
            if(src!=images.end()) {
                auto& a=src->second;
                if(!region(c,a,r.srcSubresource,r.srcOffset,r.extent))return;
                const auto& l=a.levels[r.srcSubresource.mipLevel];
                VkBufferImageCopy copy{};copy.bufferOffset=blockStart(a,r.srcSubresource,r.srcOffset,0);
                copy.bufferRowLength=l.bx;copy.bufferImageHeight=l.by;
                copy.imageSubresource=r.dstSubresource;copy.imageOffset=r.dstOffset;
                copy.imageExtent={(r.extent.width+3)/4,(r.extent.height+3)/4,r.extent.depth};
                barrier(cb,c);deviceProc<PFN_vkCmdCopyBufferToImage>(c.device,"vkCmdCopyBufferToImage")(cb,a.blocks,dest,destLayout,1,&copy);
            } else {
                auto& b=dst->second;
                if(r.dstSubresource.mipLevel>=b.levels.size()||r.dstOffset.x<0||r.dstOffset.y<0){error(c,VK_ERROR_VALIDATION_FAILED_EXT,"mixed-copy-bounds");return;}
                const auto& l=b.levels[r.dstSubresource.mipLevel];
                if(unsigned(r.dstOffset.x)>=l.width||unsigned(r.dstOffset.y)>=l.height||
                   r.extent.width>(l.width-unsigned(r.dstOffset.x)+3)/4||r.extent.height>(l.height-unsigned(r.dstOffset.y)+3)/4){error(c,VK_ERROR_VALIDATION_FAILED_EXT,"mixed-copy-extent");return;}
                VkExtent3D extent{std::min(r.extent.width*4,l.width-unsigned(r.dstOffset.x)),std::min(r.extent.height*4,l.height-unsigned(r.dstOffset.y)),r.extent.depth};
                if(!region(c,b,r.dstSubresource,r.dstOffset,extent))return;
                VkBufferImageCopy copy{};copy.bufferOffset=blockStart(b,r.dstSubresource,r.dstOffset,0);
                copy.bufferRowLength=l.bx;copy.bufferImageHeight=l.by;
                copy.imageSubresource=r.srcSubresource;copy.imageOffset=r.srcOffset;copy.imageExtent=r.extent;
                barrier(cb,c);deviceProc<PFN_vkCmdCopyImageToBuffer>(c.device,"vkCmdCopyImageToBuffer")(cb,source,sourceLayout,b.blocks,1,&copy);
                transcode(cb,c,dest,b,destLayout,r.dstSubresource,r.dstOffset,extent);
            }
        }return;
    }
    if(src->second.desc.blockBytes!=dst->second.desc.blockBytes){error(c,VK_ERROR_FORMAT_NOT_SUPPORTED,"copy-block-size");return;}
    for(unsigned i=0;i<count;i++) {
        const auto& r=regions[i];auto& a=src->second;auto& b=dst->second;
        if(!region(c,a,r.srcSubresource,r.srcOffset,r.extent)||!region(c,b,r.dstSubresource,r.dstOffset,r.extent))return;
        if(r.srcSubresource.layerCount!=r.dstSubresource.layerCount){error(c,VK_ERROR_VALIDATION_FAILED_EXT,"copy-layers");return;}
        std::vector<VkBufferCopy> copies;unsigned bx=(r.extent.width+3)/4,by=(r.extent.height+3)/4;
        for(unsigned layer=0;layer<r.srcSubresource.layerCount;layer++)for(unsigned y=0;y<by;y++) {
            VkDeviceSize x=blockStart(a,r.srcSubresource,r.srcOffset,layer)+VkDeviceSize(y)*a.levels[r.srcSubresource.mipLevel].bx*a.desc.blockBytes;
            VkDeviceSize z=blockStart(b,r.dstSubresource,r.dstOffset,layer)+VkDeviceSize(y)*b.levels[r.dstSubresource.mipLevel].bx*b.desc.blockBytes;
            copies.push_back({x,z,VkDeviceSize(bx)*a.desc.blockBytes});
        }
        barrier(cb,c);deviceProc<PFN_vkCmdCopyBuffer>(c.device,"vkCmdCopyBuffer")(cb,a.blocks,b.blocks,uint32_t(copies.size()),copies.data());
        transcode(cb,c,dest,b,destLayout,r.dstSubresource,r.dstOffset,r.extent);
    }
}
static void VKAPI_CALL copyBufferToImage2(VkCommandBuffer cb,const VkCopyBufferToImageInfo2* info) {
    std::vector<VkBufferImageCopy> regions;for(unsigned i=0;i<info->regionCount;i++){const auto& r=info->pRegions[i];regions.push_back({r.bufferOffset,r.bufferRowLength,r.bufferImageHeight,r.imageSubresource,r.imageOffset,r.imageExtent});}
    copyBufferToImage(cb,info->srcBuffer,info->dstImage,info->dstImageLayout,info->regionCount,regions.data());
}
static void VKAPI_CALL copyImageToBuffer2(VkCommandBuffer cb,const VkCopyImageToBufferInfo2* info) {
    std::vector<VkBufferImageCopy> regions;for(unsigned i=0;i<info->regionCount;i++){const auto& r=info->pRegions[i];regions.push_back({r.bufferOffset,r.bufferRowLength,r.bufferImageHeight,r.imageSubresource,r.imageOffset,r.imageExtent});}
    copyImageToBuffer(cb,info->srcImage,info->srcImageLayout,info->dstBuffer,info->regionCount,regions.data());
}
static void VKAPI_CALL copyImage2(VkCommandBuffer cb,const VkCopyImageInfo2* info) {
    std::vector<VkImageCopy> regions;for(unsigned i=0;i<info->regionCount;i++){const auto& r=info->pRegions[i];regions.push_back({r.srcSubresource,r.srcOffset,r.dstSubresource,r.dstOffset,r.extent});}
    copyImage(cb,info->srcImage,info->srcImageLayout,info->dstImage,info->dstImageLayout,info->regionCount,regions.data());
}
static void getDeviceRequirements(VkDevice d,const VkDeviceImageMemoryRequirements* info,VkMemoryRequirements2* out) {
    if(format(info->pCreateInfo->format).id<0||!active(d)){deviceProc<PFN_vkGetDeviceImageMemoryRequirements>(d,"vkGetDeviceImageMemoryRequirements")(d,info,out);return;}
    VkImage image=VK_NULL_HANDLE;auto rc=createImage(d,info->pCreateInfo,nullptr,&image);
    if(rc!=VK_SUCCESS){out->memoryRequirements={};return;}
    VkImageMemoryRequirementsInfo2 q{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,nullptr,image};getRequirements2(d,&q,out);destroyImage(d,image,nullptr);
}
static void finish(VkDevice d) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    allocationStats.erase(d);
    for(auto it=commands.begin();it!=commands.end();)if(it->second.device==d){clear(it->second);it=commands.erase(it);}else ++it;
    for(auto it=replayLayouts.begin();it!=replayLayouts.end();)if(it->second->device==d)it=replayLayouts.erase(it);else ++it;
    for(auto it=images.begin();it!=images.end();)if(it->second.device==d){deviceProc<PFN_vkDestroyBuffer>(d,"vkDestroyBuffer")(d,it->second.blocks,nullptr);it=images.erase(it);}else ++it;
    auto it=tools.find(d);if(it!=tools.end()){
        if(it->second.pipeline)deviceProc<PFN_vkDestroyPipeline>(d,"vkDestroyPipeline")(d,it->second.pipeline,nullptr);
        if(it->second.layout)deviceProc<PFN_vkDestroyPipelineLayout>(d,"vkDestroyPipelineLayout")(d,it->second.layout,nullptr);
        if(it->second.shader)deviceProc<PFN_vkDestroyShaderModule>(d,"vkDestroyShaderModule")(d,it->second.shader,nullptr);tools.erase(it);
    }
}
static PFN_vkVoidFunction intercept(const char* name) {
#define BC_PROC(api,fn) if(!strcmp(name,"vk" #api))return reinterpret_cast<PFN_vkVoidFunction>(fn)
    BC_PROC(GetPhysicalDeviceFeatures,getFeatures);BC_PROC(GetPhysicalDeviceFeatures2,getFeatures2);BC_PROC(GetPhysicalDeviceFeatures2KHR,getFeatures2);
    BC_PROC(GetPhysicalDeviceFormatProperties,getFormat);BC_PROC(GetPhysicalDeviceFormatProperties2,getFormat2);BC_PROC(GetPhysicalDeviceFormatProperties2KHR,getFormat2);
    BC_PROC(GetPhysicalDeviceImageFormatProperties,getImageFormat);BC_PROC(GetPhysicalDeviceImageFormatProperties2,getImageFormat2);BC_PROC(GetPhysicalDeviceImageFormatProperties2KHR,getImageFormat2);
    BC_PROC(CreateImage,createImage);BC_PROC(DestroyImage,destroyImage);BC_PROC(CreateImageView,createView);
    BC_PROC(CreatePipelineLayout,createLayout);BC_PROC(DestroyPipelineLayout,destroyLayout);
    BC_PROC(GetImageMemoryRequirements,getRequirements);BC_PROC(GetImageMemoryRequirements2,getRequirements2);BC_PROC(GetImageMemoryRequirements2KHR,getRequirements2);
    BC_PROC(GetDeviceImageMemoryRequirements,getDeviceRequirements);BC_PROC(GetDeviceImageMemoryRequirementsKHR,getDeviceRequirements);
    BC_PROC(BindImageMemory,bindImage);BC_PROC(BindImageMemory2,bindImages2);BC_PROC(BindImageMemory2KHR,bindImages2);
    BC_PROC(AllocateCommandBuffers,allocateCommands);BC_PROC(FreeCommandBuffers,freeCommands);BC_PROC(DestroyCommandPool,destroyPool);BC_PROC(ResetCommandPool,resetPool);
    BC_PROC(BeginCommandBuffer,begin);BC_PROC(EndCommandBuffer,end);BC_PROC(ResetCommandBuffer,reset);
    BC_PROC(CmdBindPipeline,bindPipeline);BC_PROC(CmdPushConstants,push);BC_PROC(CmdExecuteCommands,execute);
    BC_PROC(CmdCopyBufferToImage,copyBufferToImage);BC_PROC(CmdCopyImageToBuffer,copyImageToBuffer);BC_PROC(CmdCopyImage,copyImage);
    BC_PROC(CmdCopyBufferToImage2,copyBufferToImage2);BC_PROC(CmdCopyBufferToImage2KHR,copyBufferToImage2);
    BC_PROC(CmdCopyImageToBuffer2,copyImageToBuffer2);BC_PROC(CmdCopyImageToBuffer2KHR,copyImageToBuffer2);
    BC_PROC(CmdCopyImage2,copyImage2);BC_PROC(CmdCopyImage2KHR,copyImage2);
#undef BC_PROC
    return nullptr;
}
}
