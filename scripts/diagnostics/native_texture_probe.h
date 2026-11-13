/* SPDX-License-Identifier: MIT
 * Synthetic GPU BC1 -> ETC2 -> sampled texels over the real Vulkan transport.
 * Only test data is read back. Not a Vulkan BC format emulation layer.
 */
#include <math.h>
#include <time.h>
#include "texture_probe_spirv.h"
#ifndef PROBE_TEXTURE_WIDTH
#define PROBE_TEXTURE_WIDTH 127
#define PROBE_TEXTURE_HEIGHT 65
#endif
_Static_assert(PROBE_TEXTURE_WIDTH>0&&PROBE_TEXTURE_WIDTH<=1024&&
               PROBE_TEXTURE_HEIGHT>0&&PROBE_TEXTURE_HEIGHT<=1024,"bounded synthetic texture size");

static uint32_t texture_swap32(uint32_t v) {
    return __builtin_bswap32(v);
}
static int texture_clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static const int texture_modifiers[8][4] = {
    {2,8,-2,-8},{5,17,-5,-17},{9,29,-9,-29},{13,42,-13,-42},
    {18,60,-18,-60},{24,80,-24,-80},{33,106,-33,-106},{47,183,-47,-183}
};
static void texture_bc1_decode(const uint32_t *bc, int pixels[16][3]) {
    unsigned endpoints[2] = {bc[0] & 65535, bc[0] >> 16};
    int palette[4][3];
    for (unsigned i=0;i<2;i++) {
        int r=endpoints[i]>>11, g=(endpoints[i]>>5)&63, b=endpoints[i]&31;
        palette[i][0]=(r<<3)|(r>>2);palette[i][1]=(g<<2)|(g>>4);palette[i][2]=(b<<3)|(b>>2);
    }
    for (unsigned c=0;c<3;c++) {
        palette[2][c]=(2*palette[0][c]+palette[1][c])/3;
        palette[3][c]=(palette[0][c]+2*palette[1][c])/3;
    }
    for (unsigned i=0;i<16;i++)memcpy(pixels[i],palette[(bc[1]>>(2*i))&3],sizeof(palette[0]));
}
static void texture_etc2_reference(const uint32_t *bc,uint32_t *encoded) {
    int pixels[16][3];texture_bc1_decode(bc,pixels);
    uint32_t high=0,low=0;
    for(unsigned half=0;half<2;half++) {
        unsigned base[3]={0};
        for(unsigned y=0;y<4;y++)for(unsigned x=half*2;x<half*2+2;x++)
            for(unsigned c=0;c<3;c++)base[c]+=(unsigned)pixels[y*4+x][c];
        for(unsigned c=0;c<3;c++)base[c]=(base[c]+68)/136;
        unsigned best=UINT32_MAX,best_table=0,best_bits=0;
        for(unsigned table=0;table<8;table++) {
            unsigned error=0,bits=0;
            for(unsigned y=0;y<4;y++)for(unsigned x=half*2;x<half*2+2;x++) {
                unsigned pixel_error=UINT32_MAX,selector=0;
                for(unsigned s=0;s<4;s++) {
                    unsigned e=0;
                    for(unsigned c=0;c<3;c++) {
                        int delta=pixels[y*4+x][c]-texture_clamp((int)base[c]*17+texture_modifiers[table][s]);
                        e+=(unsigned)(delta*delta);
                    }
                    if(e<pixel_error){pixel_error=e;selector=s;}
                }
                error+=pixel_error;unsigned bit=x*4+y;
                bits|=(selector&1u)<<bit;bits|=(selector>>1)<<(bit+16);
            }
            if(error<best){best=error;best_table=table;best_bits=bits;}
        }
        high|=base[0]<<(half==0?28:24);high|=base[1]<<(half==0?20:16);
        high|=base[2]<<(half==0?12:8);high|=best_table<<(half==0?5:2);low|=best_bits;
    }
    encoded[0]=texture_swap32(high);encoded[1]=texture_swap32(low);
}
static uint32_t texture_etc2_pixel(const uint32_t *etc,unsigned x,unsigned y,bool srgb) {
    uint32_t high=texture_swap32(etc[0]),low=texture_swap32(etc[1]),out=0xff000000;
    unsigned half=x/2,table=(high>>(half==0?5:2))&7,bit=x*4+y;
    unsigned selector=((low>>bit)&1)|(((low>>(bit+16))&1)<<1);
    for(unsigned c=0;c<3;c++) {
        unsigned shift=(half==0?28:24)-c*8;
        unsigned channel=(unsigned)texture_clamp((int)((high>>shift)&15)*17+texture_modifiers[table][selector]);
        if(srgb) {
            double v=channel/255.0;
            v=v<=0.04045?v/12.92:pow((v+0.055)/1.055,2.4);
            channel=(unsigned)lround(v*255.0);
        }
        out|=channel<<(8*c);
    }
    return out;
}

static int run_texture_probe(VkPhysicalDevice physical,VkDevice device,VkQueue queue,
    uint32_t family,unsigned host_type,const VkPhysicalDeviceMemoryProperties *memory,
    PFN_vkGetInstanceProcAddr get,VkInstance instance) {
#define TEXTURE_FUNCTIONS(X) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements2) \
 X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
 X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
 X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
 X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
 X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCmdCopyBufferToImage)
#define TEXTURE_LOAD(n) PFN_##n n=(PFN_##n)vkGetDeviceProcAddr(device,#n);if(!n)return 2;
    TEXTURE_FUNCTIONS(TEXTURE_LOAD)
#undef TEXTURE_LOAD
    PFN_vkGetPhysicalDeviceFormatProperties format_properties=(PFN_vkGetPhysicalDeviceFormatProperties)get(instance,"vkGetPhysicalDeviceFormatProperties");
    if(!format_properties)return 2;
    enum{WIDTH=PROBE_TEXTURE_WIDTH,HEIGHT=PROBE_TEXTURE_HEIGHT,BX=(WIDTH+3)/4,BY=(HEIGHT+3)/4,
        BLOCKS=BX*BY,PIXEL_BYTES=WIDTH*HEIGHT*4,READ_BYTES=PIXEL_BYTES+BLOCKS*8+256};
    uint32_t input[BLOCKS*2],reference[BLOCKS*2];
    for(unsigned b=0;b<BLOCKS;b++) {
        /* Opaque, varied endpoints and selectors. No game assets. */
        unsigned c0=0x8000u+((b*193u)%32768u),c1=(b*67u)%32768u;
        input[b*2]=c0|(c1<<16);input[b*2+1]=0xe4e4e4e4u^(b*0x1234567u);
        texture_etc2_reference(&input[b*2],&reference[b*2]);
    }
    unsigned wrong=0,cases=0;
    for(unsigned srgb=0;srgb<2;srgb++) {
        VkFormat format=srgb?VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
        VkFormatProperties fp;format_properties(physical,format,&fp);
        VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        fprintf(stderr,"VK_TEXTURE format=%u features=%x required=%x\n",format,fp.optimalTilingFeatures,needed);
        if((fp.optimalTilingFeatures&needed)!=needed)return 3;
        VkResult result=VK_SUCCESS;
        VkBuffer buffers[3]={0};VkDeviceMemory allocations[3]={0};void *maps[3]={0};
        VkImage image=VK_NULL_HANDLE;VkDeviceMemory image_memory=VK_NULL_HANDLE;
        VkImageView view=VK_NULL_HANDLE;VkSampler sampler=VK_NULL_HANDLE;
        VkShaderModule modules[2]={0};VkPipelineLayout layouts[2]={0};
        VkDescriptorSetLayout set_layouts[2]={0};VkPipeline pipelines[2]={0};
        VkDescriptorPool descriptor_pool=VK_NULL_HANDLE;VkCommandPool command_pool=VK_NULL_HANDLE;
        VkFence fence=VK_NULL_HANDLE;
#define TEXTURE_TRY(call) do{result=(call);if(result!=VK_SUCCESS){fprintf(stderr,"VK_TEXTURE error line=%d result=%d call=%s\n",__LINE__,result,#call);goto texture_done;}}while(0)
        for(unsigned i=0;i<3;i++) {
            VkDeviceSize bytes=i==2?READ_BYTES:sizeof(input);
            VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,
                .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
            TEXTURE_TRY(vkCreateBuffer(device,&bi,NULL,&buffers[i]));
            VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffers[i],&req);
            unsigned type=host_type;
            if(i==1)for(unsigned j=0;j<memory->memoryTypeCount;j++)
                if((req.memoryTypeBits&(1u<<j))&&(memory->memoryTypes[j].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=j;break;}
            if(!(req.memoryTypeBits&(1u<<type))||req.size>16*1024*1024){result=VK_ERROR_FEATURE_NOT_PRESENT;goto texture_done;}
            VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
            TEXTURE_TRY(vkAllocateMemory(device,&ai,NULL,&allocations[i]));
            TEXTURE_TRY(vkBindBufferMemory(device,buffers[i],allocations[i],0));
            if(i!=1)TEXTURE_TRY(vkMapMemory(device,allocations[i],0,VK_WHOLE_SIZE,0,&maps[i]));
        }
        memcpy(maps[0],input,sizeof(input));memset(maps[2],0xa5,READ_BYTES);
        VkMappedMemoryRange ranges[2]={{.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=allocations[0],.size=VK_WHOLE_SIZE},
            {.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=allocations[2],.size=VK_WHOLE_SIZE}};
        TEXTURE_TRY(vkFlushMappedMemoryRanges(device,2,ranges));
        VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
            .format=format,.extent={WIDTH,HEIGHT,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
            .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
        TEXTURE_TRY(vkCreateImage(device,&ii,NULL,&image));
        VkMemoryDedicatedRequirements dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 req={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,.pNext=&dedicated};
        VkImageMemoryRequirementsInfo2 query={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,.image=image};
        vkGetImageMemoryRequirements2(device,&query,&req);
        unsigned image_type=VK_MAX_MEMORY_TYPES;
        for(unsigned i=0;i<memory->memoryTypeCount;i++)if((req.memoryRequirements.memoryTypeBits&(1u<<i))&&
            (memory->memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){image_type=i;break;}
        if(image_type==VK_MAX_MEMORY_TYPES||req.memoryRequirements.size>16*1024*1024){result=VK_ERROR_FEATURE_NOT_PRESENT;goto texture_done;}
        VkMemoryDedicatedAllocateInfo binding={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=image};
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=dedicated.requiresDedicatedAllocation?&binding:NULL,
            .allocationSize=req.memoryRequirements.size,.memoryTypeIndex=image_type};
        TEXTURE_TRY(vkAllocateMemory(device,&ai,NULL,&image_memory));TEXTURE_TRY(vkBindImageMemory(device,image,image_memory,0));
        /* Query (but never allocate) the same image in RGBA8 for an actual
         * driver-requirements comparison, including tiling/alignment overhead. */
        VkImage rgba_image=VK_NULL_HANDLE;VkImageCreateInfo rgba_info=ii;
        rgba_info.format=srgb?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM;
        TEXTURE_TRY(vkCreateImage(device,&rgba_info,NULL,&rgba_image));
        VkMemoryRequirements2 rgba_req={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        query.image=rgba_image;vkGetImageMemoryRequirements2(device,&query,&rgba_req);
        vkDestroyImage(device,rgba_image,NULL);
        fprintf(stderr,"VK_TEXTURE allocation format=%u width=%u height=%u bytes=%llu blocks=%u rgba_payload=%u rgba_requirement=%llu transcode_device_address=0\n",
            format,WIDTH,HEIGHT,(unsigned long long)req.memoryRequirements.size,BLOCKS,PIXEL_BYTES,
            (unsigned long long)rgba_req.memoryRequirements.size);
        VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
            .format=format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        TEXTURE_TRY(vkCreateImageView(device,&vi,NULL,&view));
        VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,
            .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        TEXTURE_TRY(vkCreateSampler(device,&si,NULL,&sampler));
        for(unsigned i=0;i<2;i++) {
            const uint32_t *code=i?etc2_sample_spirv:bc1_etc2_spirv;
            size_t size=i?sizeof(etc2_sample_spirv):sizeof(bc1_etc2_spirv);
            VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=size,.pCode=code};
            TEXTURE_TRY(vkCreateShaderModule(device,&mi,NULL,&modules[i]));
            VkDescriptorSetLayoutBinding bindings[2]={{.binding=0,.descriptorType=i?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
                {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
            VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
            TEXTURE_TRY(vkCreateDescriptorSetLayout(device,&li,NULL,&set_layouts[i]));
            VkPushConstantRange push={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.size=8};
            VkPipelineLayoutCreateInfo pi={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layouts[i],.pushConstantRangeCount=1,.pPushConstantRanges=&push};
            TEXTURE_TRY(vkCreatePipelineLayout(device,&pi,NULL,&layouts[i]));
            VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=modules[i],.pName="main"},.layout=layouts[i]};
            TEXTURE_TRY(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,NULL,&pipelines[i]));
        }
        VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1}};
        VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=2,.poolSizeCount=2,.pPoolSizes=sizes};
        TEXTURE_TRY(vkCreateDescriptorPool(device,&dpi,NULL,&descriptor_pool));
        VkDescriptorSet sets[2];VkDescriptorSetAllocateInfo dai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=descriptor_pool,.descriptorSetCount=2,.pSetLayouts=set_layouts};
        TEXTURE_TRY(vkAllocateDescriptorSets(device,&dai,sets));
        VkDescriptorBufferInfo buffer_infos[3]={{buffers[0],0,sizeof(input)},{buffers[1],0,sizeof(reference)},{buffers[2],0,PIXEL_BYTES}};
        VkDescriptorImageInfo image_info={sampler,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[4]={{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[0],.dstBinding=0,.descriptorCount=1,
            .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffer_infos[0]},
            {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[0],.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffer_infos[1]},
            {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[1],.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&image_info},
            {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[1],.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffer_infos[2]}};
        vkUpdateDescriptorSets(device,4,writes,0,NULL);
        VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
        TEXTURE_TRY(vkCreateCommandPool(device,&pi,NULL,&command_pool));
        VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=command_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        VkCommandBuffer cb;TEXTURE_TRY(vkAllocateCommandBuffers(device,&cai,&cb));
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};TEXTURE_TRY(vkBeginCommandBuffer(cb,&begin));
        VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,NULL,0,NULL);
        uint32_t push[2]={BLOCKS,0};vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[0]);
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,layouts[0],0,1,&sets[0],0,NULL);
        vkCmdPushConstants(cb,layouts[0],VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
        vkCmdDispatch(cb,(BLOCKS+63)/64,1,1);
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,NULL,0,NULL);
        VkImageMemoryBarrier transition={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&transition);
        VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={WIDTH,HEIGHT,1}};
        vkCmdCopyBufferToImage(cb,buffers[1],image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        VkBufferCopy read_encoded={.dstOffset=PIXEL_BYTES,.size=sizeof(reference)};
        vkCmdCopyBuffer(cb,buffers[1],buffers[2],1,&read_encoded);
        transition.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;transition.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        transition.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;transition.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,&transition);
        push[0]=WIDTH;push[1]=HEIGHT;vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[1]);
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,layouts[1],0,1,&sets[1],0,NULL);
        vkCmdPushConstants(cb,layouts[1],VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
        vkCmdDispatch(cb,(WIDTH+7)/8,(HEIGHT+7)/8,1);
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
        TEXTURE_TRY(vkEndCommandBuffer(cb));
        VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};TEXTURE_TRY(vkCreateFence(device,&fi,NULL,&fence));
        struct timespec start,end;clock_gettime(CLOCK_MONOTONIC,&start);
        VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
        TEXTURE_TRY(vkQueueSubmit(queue,1,&submit,fence));
        result=vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull);
        if(result!=VK_SUCCESS){fprintf(stderr,"VK_TEXTURE submitted_work_failed result=%d\n",result);_Exit(2);}
        clock_gettime(CLOCK_MONOTONIC,&end);
        TEXTURE_TRY(vkInvalidateMappedMemoryRanges(device,1,&ranges[1]));
        unsigned encoded_wrong=0,sampled_wrong=0,guard_wrong=0;uint32_t *out=maps[2];
        for(unsigned i=0;i<BLOCKS*2;i++)if(out[WIDTH*HEIGHT+i]!=reference[i])encoded_wrong++;
        for(unsigned i=WIDTH*HEIGHT+BLOCKS*2;i<READ_BYTES/4;i++)if(out[i]!=0xa5a5a5a5u)guard_wrong++;
        for(unsigned y=0;y<HEIGHT;y++)for(unsigned x=0;x<WIDTH;x++) {
            uint32_t expected=texture_etc2_pixel(&reference[((y/4)*BX+x/4)*2],x%4,y%4,srgb);
            uint32_t actual=out[y*WIDTH+x];
            for(unsigned c=0;c<4;c++)if(abs((int)((actual>>(8*c))&255)-(int)((expected>>(8*c))&255))>1){sampled_wrong++;break;}
        }
        wrong+=encoded_wrong+sampled_wrong+guard_wrong;cases++;
        fprintf(stderr,"VK_TEXTURE case srgb=%u blocks=%u encoded_wrong=%u sampled_wrong=%u guard_wrong=%u fence_ms=%.3f transcode_rgba_intermediate=0 synthetic_readback=1 display_readbacks=0\n",
            srgb,BLOCKS,encoded_wrong,sampled_wrong,guard_wrong,(end.tv_sec-start.tv_sec)*1000.0+(end.tv_nsec-start.tv_nsec)/1000000.0);
texture_done:
        if(fence)vkDestroyFence(device,fence,NULL);
        if(command_pool)vkDestroyCommandPool(device,command_pool,NULL);
        if(descriptor_pool)vkDestroyDescriptorPool(device,descriptor_pool,NULL);
        for(unsigned i=0;i<2;i++) {
            if(pipelines[i])vkDestroyPipeline(device,pipelines[i],NULL);
            if(layouts[i])vkDestroyPipelineLayout(device,layouts[i],NULL);
            if(set_layouts[i])vkDestroyDescriptorSetLayout(device,set_layouts[i],NULL);
            if(modules[i])vkDestroyShaderModule(device,modules[i],NULL);
        }
        if(sampler)vkDestroySampler(device,sampler,NULL);
        if(view)vkDestroyImageView(device,view,NULL);
        if(image)vkDestroyImage(device,image,NULL);
        if(image_memory)vkFreeMemory(device,image_memory,NULL);
        for(unsigned i=0;i<3;i++) {
            if(maps[i])vkUnmapMemory(device,allocations[i]);
            if(buffers[i])vkDestroyBuffer(device,buffers[i],NULL);
            if(allocations[i])vkFreeMemory(device,allocations[i],NULL);
        }
#undef TEXTURE_TRY
        if(result!=VK_SUCCESS)return 2;
    }
    fprintf(stderr,"VK_TEXTURE complete cases=%u mismatches=%u result=%u\n",cases,wrong,!!wrong);
    return wrong?1:0;
}
