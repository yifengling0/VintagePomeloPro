#!/usr/bin/env python3
"""Exercise real native-resource helpers and maintenance4 dispatch functions.

The driver is a deterministic capability/requirements stub; this is not GPU
acceptance. Native image copies are tested separately by the signed OHOS probe.
"""
import argparse
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, required=True)
p.add_argument('--build-dir', type=Path, required=True)
a = p.parse_args()
source = a.source.resolve(strict=True)
out = a.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)


def function(file, name):
    text = (source / 'src/venus' / file).read_text()
    start = text.index('static void\n' + name + '(')
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


memory_source = (source / 'src/venus/vkr_device_memory.c').read_text()
allocation_start = memory_source.index('   const uint32_t native_export_handle =')
allocation_end = memory_source.index('   } else\n#endif', allocation_start) + len('   }')
allocation_body = memory_source[allocation_start:allocation_end]
map_start = memory_source.index('   /* Guest cache properties select exportability;')
map_start = memory_source.rindex('#ifdef __OHOS__', 0, map_start)
map_end = memory_source.index('   if (!mem) {', map_start)
allocation_map = memory_source[map_start:map_end]


code = r'''
#define __OHOS__
#define VK_NO_PROTOTYPES
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "vkr_ohos_native_resource.h"
#include "vkr_ohos_native_memory.h"
#define UNUSED __attribute__((unused))
static unsigned calls, req_calls;
static VkExternalMemoryProperties capability;
static VkResult query_result;
static VkBufferCreateFlags buffer_flags;
static VkBufferUsageFlags buffer_usage;
static VkImageCreateInfo wanted_image;
static const VkFormat *wanted_formats;
static bool want_stencil;
static bool expect_native = true;
static uint32_t allocation_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
static VKAPI_ATTR void VKAPI_CALL buffer_query(VkPhysicalDevice physical,
    const VkPhysicalDeviceExternalBufferInfo *info, VkExternalBufferProperties *out)
{
    assert(physical == (VkPhysicalDevice)(uintptr_t)19);
    assert(info->handleType == 0x2000 && info->flags == buffer_flags &&
           info->usage == buffer_usage);
    calls++;
    out->externalMemoryProperties = capability;
}
static VKAPI_ATTR VkResult VKAPI_CALL image_query(VkPhysicalDevice physical,
    const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *out)
{
    assert(physical == (VkPhysicalDevice)(uintptr_t)19);
    assert(info->format == wanted_image.format && info->type == wanted_image.imageType &&
           info->tiling == wanted_image.tiling && info->usage == wanted_image.usage &&
           info->flags == wanted_image.flags);
    const VkPhysicalDeviceExternalImageFormatInfo *external =
        vkr_ohos_resource_find(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
    assert(external && external->handleType == 0x2000);
    const VkImageFormatListCreateInfo *formats =
        vkr_ohos_resource_find(info->pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO);
    assert((formats != NULL) == (wanted_formats != NULL));
    if (formats) assert(formats->viewFormatCount == 2 && formats->pViewFormats == wanted_formats);
    const VkImageStencilUsageCreateInfo *stencil =
        vkr_ohos_resource_find(info->pNext, VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO);
    assert((stencil != NULL) == want_stencil);
    if (stencil) assert(stencil->stencilUsage == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    /* The unrelated create-only structure must not enter the query chain. */
    assert(!vkr_ohos_resource_find(info->pNext, VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR));
    calls++;
    ((VkExternalImageFormatProperties *)out->pNext)->externalMemoryProperties = capability;
    return query_result;
}
struct vkr_physical_device {
    struct { union { VkPhysicalDevice physical_device; } handle; } base;
    struct {
        PFN_vkGetPhysicalDeviceExternalBufferProperties GetPhysicalDeviceExternalBufferProperties;
        PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2;
    } proc_table;
    VkPhysicalDeviceMemoryProperties memory_properties;
    struct vkr_ohos_memory_types ohos_memory_types;
};
struct vkr_device {
    struct vkr_physical_device *physical_device;
    uint32_t ohos_native_export_handle;
    struct vn_device_proc_table {
        PFN_vkGetDeviceBufferMemoryRequirements GetDeviceBufferMemoryRequirements;
        PFN_vkGetDeviceImageMemoryRequirements GetDeviceImageMemoryRequirements;
    } proc_table;
};
struct vkr_context { bool ohos_native_memory; };
struct vn_dispatch_context { struct vkr_context *data; };
struct vn_command_vkGetDeviceBufferMemoryRequirements {
    VkDevice device; const VkDeviceBufferMemoryRequirements *pInfo;
    VkMemoryRequirements2 *pMemoryRequirements;
};
struct vn_command_vkGetDeviceImageMemoryRequirements {
    VkDevice device; const VkDeviceImageMemoryRequirements *pInfo;
    VkMemoryRequirements2 *pMemoryRequirements;
};
static struct vkr_device device;
static struct vkr_device *vkr_device_from_handle(VkDevice h) { assert(h); return &device; }
#define vn_replace_vkGetDeviceBufferMemoryRequirements_args_handle(a) ((void)(a))
#define vn_replace_vkGetDeviceImageMemoryRequirements_args_handle(a) ((void)(a))
static void requirement(VkMemoryRequirements2 *out)
{
    req_calls++;
    out->memoryRequirements = (VkMemoryRequirements){.size=131072, .alignment=65536, .memoryTypeBits=3};
    VkMemoryDedicatedRequirements *dedicated = out->pNext;
    if (dedicated) dedicated->requiresDedicatedAllocation = VK_TRUE;
}
static VKAPI_ATTR void VKAPI_CALL buffer_requirements(VkDevice d,
    const VkDeviceBufferMemoryRequirements *info, VkMemoryRequirements2 *out)
{
    assert(d);
    const VkExternalMemoryBufferCreateInfo *external =
        vkr_ohos_resource_find(info->pCreateInfo->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO);
    assert((external != NULL) == expect_native);
    if (external) assert(external->handleTypes == 0x2000);
    requirement(out);
}
static VKAPI_ATTR void VKAPI_CALL image_requirements(VkDevice d,
    const VkDeviceImageMemoryRequirements *info, VkMemoryRequirements2 *out)
{
    assert(d);
    const VkExternalMemoryImageCreateInfo *external =
        vkr_ohos_resource_find(info->pCreateInfo->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
    assert((external != NULL) == expect_native);
    if (external) assert(external->handleTypes == 0x2000);
    requirement(out);
}
struct allocation_result { VkResult ret; unsigned export_count; uint32_t handles; };
static void allocation_prepare(struct vkr_context *ctx, VkMemoryAllocateInfo *alloc_info,
                               struct allocation_result *args)
{
    struct vkr_device *dev = &device;
    const void *res_info = NULL;
    const uint32_t property_flags = allocation_flags;
    VkExportMemoryAllocateInfo local_export_info;
    VkExportMemoryAllocateInfo *export_info = (void *)vkr_ohos_resource_find(
        alloc_info->pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
#define getpagesize() 4096
''' + allocation_body + r'''
#undef getpagesize
    for (const VkBaseInStructure *p = alloc_info->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO) {
            args->export_count++;
            args->handles = ((const VkExportMemoryAllocateInfo *)p)->handleTypes;
        }
    /* local_export_info may not outlive dispatch; never dereference it
     * after returning, just as the real driver's synchronous create. */
    if (alloc_info->pNext == &local_export_info) alloc_info->pNext = local_export_info.pNext;
}
'''+r'''
struct vkr_device_memory { int unused; } allocation_object;
static unsigned wanted_host_type;
static bool allocation_fail;
static struct vkr_device_memory *vkr_device_memory_create_and_add(
    struct vkr_context *ctx, VkMemoryAllocateInfo *info)
{
    (void)ctx; assert(info->memoryTypeIndex==wanted_host_type);
    return allocation_fail ? NULL : &allocation_object;
}
static void check_allocation_map(struct vkr_physical_device *physical_dev,
                                 struct vkr_context *ctx, uint32_t mem_type_index)
{
    VkMemoryAllocateInfo info={.memoryTypeIndex=mem_type_index};
    VkMemoryAllocateInfo *alloc_info=&info, *args=&info;
'''+allocation_map+r'''
    assert(info.memoryTypeIndex==mem_type_index);
    assert((mem==NULL)==allocation_fail);
}
'''+ function('vkr_buffer.c', 'vkr_ohos_native_buffer_requirements') + '\n' + function('vkr_image.c', 'vkr_ohos_native_image_requirements') + '\n' + function('vkr_buffer.c', 'vkr_dispatch_vkGetDeviceBufferMemoryRequirements') + '\n' + function(
    'vkr_image.c', 'vkr_dispatch_vkGetDeviceImageMemoryRequirements') + r'''
int main(void)
{
    /* Older Vulkan 1.3.204 requires exact dedicated sizes; newer specs
     * allow larger allocations. Preserve either valid guest request. */
    VkMemoryDedicatedAllocateInfo binding = {
        .sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image=(VkImage)(uintptr_t)3};
    VkMemoryAllocateInfo allocation = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=91136, .pNext=&binding};
    assert(vkr_ohos_native_allocation_size(&allocation,4096)==VK_SUCCESS);
    assert(allocation.allocationSize==91136 && allocation.pNext==&binding);
    binding.image=VK_NULL_HANDLE; binding.buffer=(VkBuffer)(uintptr_t)4;
    assert(vkr_ohos_native_allocation_size(&allocation,4096)==VK_SUCCESS);
    assert(allocation.allocationSize==91136);
    binding.buffer=VK_NULL_HANDLE;
    assert(vkr_ohos_native_allocation_size(&allocation,4096)==VK_SUCCESS);
    assert(allocation.allocationSize==94208);
    allocation.allocationSize=UINT64_MAX;
    assert(vkr_ohos_native_allocation_size(&allocation,4096)==VK_ERROR_OUT_OF_DEVICE_MEMORY);
    assert(allocation.allocationSize==UINT64_MAX);
    allocation.allocationSize=65536;
    assert(vkr_ohos_native_allocation_size(&allocation,4096)==VK_SUCCESS);
    assert(allocation.allocationSize==65536);
    assert(vkr_ohos_native_allocation_size(&allocation,0)==VK_ERROR_OUT_OF_DEVICE_MEMORY);
    assert(vkr_ohos_native_allocation_size(&allocation,3072)==VK_ERROR_OUT_OF_DEVICE_MEMORY);
    struct vkr_context native_context = {.ohos_native_memory=true};
    device.ohos_native_export_handle=0x2000;
    VkExportMemoryAllocateInfo export = {.sType=VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    struct allocation_result snapshot = {0};
    allocation=(VkMemoryAllocateInfo){.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=91136, .pNext=&export};
    allocation_prepare(&native_context,&allocation,&snapshot);
    assert(snapshot.ret==VK_SUCCESS && snapshot.export_count==1 && snapshot.handles==0x2000);
    assert(allocation.allocationSize==94208 && allocation.pNext==&export);
    export.handleTypes=1; snapshot=(struct allocation_result){0};
    allocation_prepare(&native_context,&allocation,&snapshot);
    assert(snapshot.ret==VK_ERROR_INVALID_EXTERNAL_HANDLE && export.handleTypes==1);
    export.handleTypes=0x2000; snapshot=(struct allocation_result){0};
    allocation_prepare(&native_context,&allocation,&snapshot);
    assert(snapshot.ret==VK_SUCCESS && snapshot.export_count==1);
    binding.image=(VkImage)(uintptr_t)3;
    allocation.allocationSize=91136; allocation.pNext=&binding;
    snapshot=(struct allocation_result){0};
    allocation_prepare(&native_context,&allocation,&snapshot);
    assert(snapshot.ret==VK_SUCCESS && snapshot.export_count==1 && snapshot.handles==0x2000);
    assert(allocation.allocationSize==91136 && allocation.pNext==&binding);
    native_context.ohos_native_memory=false; allocation.pNext=NULL;
    snapshot=(struct allocation_result){0};
    allocation_prepare(&native_context,&allocation,&snapshot);
    assert(snapshot.ret==VK_SUCCESS && snapshot.export_count==0 && allocation.allocationSize==91136);
    /* Device-private allocations must never acquire the upload fd contract. */
    native_context.ohos_native_memory=true; allocation_flags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    allocation.allocationSize=91136; allocation.pNext=NULL;
    snapshot=(struct allocation_result){0};
    allocation_prepare(&native_context,&allocation,&snapshot);
    assert(snapshot.ret==VK_SUCCESS && !snapshot.export_count && allocation.allocationSize==91136);
    allocation_flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkPhysicalDevice physical = (VkPhysicalDevice)(uintptr_t)19;
    capability = (VkExternalMemoryProperties){
        .externalMemoryFeatures=VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT,
        .compatibleHandleTypes=0xa000};
    VkBufferCreateInfo buffer = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=8192, .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    buffer_flags=buffer.flags; buffer_usage=buffer.usage;
    VkBufferCreateInfo bhost;
    VkExternalMemoryBufferCreateInfo bexternal;
    assert(vkr_ohos_native_buffer_info(physical,NULL,0,&buffer,&bhost,&bexternal)==VK_SUCCESS);
    assert(!calls && !memcmp(&buffer,&bhost,sizeof(buffer)));
    assert(vkr_ohos_native_buffer_info(physical,buffer_query,0x2000,&buffer,&bhost,&bexternal)==VK_SUCCESS);
    assert(bhost.pNext==&bexternal && bexternal.handleTypes==0x2000 && !buffer.pNext);
    /* A matching application contract is preserved rather than duplicated. */
    buffer.pNext=&bexternal; bexternal.pNext=NULL;
    assert(vkr_ohos_native_buffer_info(physical,buffer_query,0x2000,&buffer,&bhost,&bexternal)==VK_SUCCESS);
    assert(bhost.pNext==buffer.pNext);
    bexternal.handleTypes=1;
    assert(vkr_ohos_native_buffer_info(physical,buffer_query,0x2000,&buffer,&bhost,&bexternal)==VK_ERROR_INVALID_EXTERNAL_HANDLE);
    buffer.pNext=NULL;
    capability.externalMemoryFeatures=VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    assert(vkr_ohos_native_buffer_info(physical,buffer_query,0x2000,&buffer,&bhost,&bexternal)==VK_ERROR_FEATURE_NOT_PRESENT);
    capability.externalMemoryFeatures=VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT;
    capability.compatibleHandleTypes=0x8000;
    assert(vkr_ohos_native_buffer_info(physical,buffer_query,0x2000,&buffer,&bhost,&bexternal)==VK_ERROR_FEATURE_NOT_PRESENT);
    capability.compatibleHandleTypes=0xa000;

    VkFormat formats[]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB};
    VkImageSwapchainCreateInfoKHR unrelated={.sType=VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR};
    VkImageFormatListCreateInfo list={.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .pNext=&unrelated, .viewFormatCount=2, .pViewFormats=formats};
    VkImageStencilUsageCreateInfo stencil={.sType=VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO,
        .pNext=&list, .stencilUsage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
    VkImageCreateInfo image={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext=&stencil,
        .imageType=VK_IMAGE_TYPE_2D, .format=VK_FORMAT_R8G8B8A8_SRGB, .extent={64,64,1},
        .mipLevels=7, .arrayLayers=2, .samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL, .usage=VK_IMAGE_USAGE_SAMPLED_BIT,
        .flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT};
    assert(vkr_ohos_native_gpu_image(&image));
    image.tiling=VK_IMAGE_TILING_LINEAR; assert(!vkr_ohos_native_gpu_image(&image));
    image.tiling=VK_IMAGE_TILING_OPTIMAL;
    VkExternalMemoryImageCreateInfo declared={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext=image.pNext,.handleTypes=0x2000};
    image.pNext=&declared; assert(!vkr_ohos_native_gpu_image(&image));
    declared.handleTypes=0; assert(vkr_ohos_native_gpu_image(&image));
    image.pNext=declared.pNext;
    wanted_image=image; wanted_formats=formats; want_stencil=true;
    VkImageCreateInfo ihost;
    VkExternalMemoryImageCreateInfo iexternal;
    unsigned previous=calls;
    assert(vkr_ohos_native_image_info(physical,NULL,0,&image,&ihost,&iexternal)==VK_SUCCESS);
    assert(calls==previous && !memcmp(&image,&ihost,sizeof(image)));
    assert(vkr_ohos_native_image_info(physical,image_query,0x2000,&image,&ihost,&iexternal)==VK_SUCCESS);
    assert(ihost.pNext==&iexternal && iexternal.pNext==&stencil &&
           image.pNext==&stencil && stencil.pNext==&list && list.pNext==&unrelated);
    query_result=VK_ERROR_FORMAT_NOT_SUPPORTED;
    assert(vkr_ohos_native_image_info(physical,image_query,0x2000,&image,&ihost,&iexternal)==query_result);
    query_result=VK_SUCCESS;
    capability.compatibleHandleTypes=0x8000;
    assert(vkr_ohos_native_image_info(physical,image_query,0x2000,&image,&ihost,&iexternal)==VK_ERROR_FORMAT_NOT_SUPPORTED);
    capability.compatibleHandleTypes=0xa000;
    /* Dedicated-only capability is retained, never treated as suballocatable. */
    capability.externalMemoryFeatures|=VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
    struct vkr_physical_device pd={.base.handle.physical_device=physical,
        .proc_table={buffer_query,image_query}};
    pd.memory_properties=(VkPhysicalDeviceMemoryProperties){.memoryTypeCount=2,.memoryHeapCount=1,
        .memoryTypes={{.propertyFlags=7,.heapIndex=0},{.propertyFlags=11,.heapIndex=0}},
        .memoryHeaps={{.size=1024*1024*1024,.flags=VK_MEMORY_HEAP_DEVICE_LOCAL_BIT}}};
    VkPhysicalDeviceMemoryProperties original=pd.memory_properties;
    assert(vkr_ohos_memory_types_init(&pd.memory_properties,&pd.ohos_memory_types));
    assert(pd.memory_properties.memoryTypeCount==3 && pd.memory_properties.memoryHeapCount==1);
    assert(!memcmp(pd.memory_properties.memoryHeaps,original.memoryHeaps,sizeof(original.memoryHeaps)));
    assert(pd.memory_properties.memoryTypes[0].propertyFlags==VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    for(unsigned i=0;i<2;i++) {
        assert(!memcmp(&pd.memory_properties.memoryTypes[i+1],&original.memoryTypes[i],sizeof(VkMemoryType)));
        assert(vkr_ohos_memory_type_to_host(&pd.ohos_memory_types,i+1)==i);
    }
    assert(vkr_ohos_memory_type_to_host(&pd.ohos_memory_types,0)==0);
    for(unsigned fail=0;fail<2;fail++) {
        allocation_fail=fail;
        for(unsigned i=0;i<3;i++) {
            wanted_host_type=i ? i-1 : 0;
            check_allocation_map(&pd,&native_context,i);
        }
    }
    allocation_fail=false;

    assert(vkr_ohos_memory_type_bits(&pd.ohos_memory_types,3)==7);
    assert(vkr_ohos_memory_type_bits(&pd.ohos_memory_types,2)==4);
    assert(vkr_ohos_gpu_image_memory_bits(&pd.memory_properties,&pd.ohos_memory_types,3)==1);
    /* Full 31-type host uses bit31 without shifting a 32-bit count or adding a heap. */
    VkPhysicalDeviceMemoryProperties edge=original; edge.memoryTypeCount=31;
    for(unsigned i=2;i<31;i++)edge.memoryTypes[i]=original.memoryTypes[1];
    struct vkr_ohos_memory_types map;
    assert(vkr_ohos_memory_types_init(&edge,&map) && edge.memoryTypeCount==32);
    assert(vkr_ohos_memory_type_bits(&map,0x7fffffff)==UINT32_MAX);
    assert(vkr_ohos_memory_type_to_host(&map,31)==30);
    assert(!vkr_ohos_memory_types_init(&edge,&map) && !map.enabled);
    edge=original; edge.memoryTypeCount=0;
    assert(!vkr_ohos_memory_types_init(&edge,&map));
    edge=original; edge.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    edge.memoryTypes[1].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_PROTECTED_BIT;
    assert(!vkr_ohos_memory_types_init(&edge,&map));
    edge.memoryTypes[1].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    assert(!vkr_ohos_memory_types_init(&edge,&map));
    edge=original; edge.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    assert(vkr_ohos_memory_types_init(&edge,&map) && map.gpu_host_type==1);
    assert(vkr_ohos_memory_type_to_host(&map,0)==1 && vkr_ohos_memory_type_bits(&map,2)==5);
    map=(struct vkr_ohos_memory_types){0};
    assert(vkr_ohos_memory_type_to_host(&map,5)==5 && vkr_ohos_memory_type_bits(&map,0xdeadbeef)==0xdeadbeef);
    assert(vkr_ohos_gpu_image_memory_bits(&original,&map,3)==3);
    device=(struct vkr_device){.physical_device=&pd,.ohos_native_export_handle=0x2000,
        .proc_table={buffer_requirements,image_requirements}};
    struct vkr_context context={.ohos_native_memory=true};
    struct vn_dispatch_context dispatch={.data=&context};
    VkMemoryDedicatedRequirements dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,.pNext=&dedicated};
    VkDeviceImageMemoryRequirements im={.sType=VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,.pCreateInfo=&image};
    struct vn_command_vkGetDeviceImageMemoryRequirements ic={.device=(VkDevice)(uintptr_t)1,
        .pInfo=&im,.pMemoryRequirements=&req};
    expect_native=false;
    previous=calls;
    vkr_dispatch_vkGetDeviceImageMemoryRequirements(&dispatch,&ic);
    assert(calls==previous);
    assert(ic.pInfo==&im && image.pNext==&stencil && req.memoryRequirements.size==131072 &&
           req.memoryRequirements.alignment==65536 && req.memoryRequirements.memoryTypeBits==1 &&
           dedicated.requiresDedicatedAllocation && req_calls==1);
    VkDeviceBufferMemoryRequirements bm={.sType=VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,.pCreateInfo=&buffer};
    struct vn_command_vkGetDeviceBufferMemoryRequirements bc={.device=(VkDevice)(uintptr_t)1,
        .pInfo=&bm,.pMemoryRequirements=&req};
    expect_native=true;
    vkr_dispatch_vkGetDeviceBufferMemoryRequirements(&dispatch,&bc);
    assert(req.memoryRequirements.memoryTypeBits==7);
    assert(bc.pInfo==&bm && !buffer.pNext && req_calls==2);
    image.tiling=VK_IMAGE_TILING_LINEAR; wanted_image=image;
    vkr_dispatch_vkGetDeviceImageMemoryRequirements(&dispatch,&ic);
    assert(req.memoryRequirements.memoryTypeBits==7 && req_calls==3);
    query_result=VK_ERROR_FORMAT_NOT_SUPPORTED;
    vkr_dispatch_vkGetDeviceImageMemoryRequirements(&dispatch,&ic);
    assert(!req.memoryRequirements.memoryTypeBits && ic.pInfo==&im && req_calls==3);
    /* The original Wine context must bypass the private capability query. */
    context.ohos_native_memory=false;
    pd.memory_properties=original; pd.ohos_memory_types=(struct vkr_ohos_memory_types){0};
    expect_native=false;
    previous=calls;
    vkr_dispatch_vkGetDeviceBufferMemoryRequirements(&dispatch,&bc);
    vkr_dispatch_vkGetDeviceImageMemoryRequirements(&dispatch,&ic);
    assert(calls==previous && req_calls==5 && req.memoryRequirements.memoryTypeBits==3 && ic.pInfo==&im && bc.pInfo==&bm);
    puts("PASS external buffer/image capabilities, preserved pNext, incompatible handles, format failures, exact maintenance4 requirements, dedicated sizes and heap overflow");
}
'''
c = out / 'resource-contract.c'
c.write_text(code)
for name, flags in [('plain', []), ('sanitized', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'])]:
    binary = out / name
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(source / 'src/venus'), *flags, str(c), '-o', str(binary)],
                   check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=10)
