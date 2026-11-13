/* SPDX-License-Identifier: MIT
 * Real API resource tests plus independent bcdec reference for every BC mode.
 * Synthetic texture readbacks only; never reads a displayed frame.
 */
#include <math.h>
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#define BCDECDEF static __attribute__((unused))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-sign"
#include "bcdec.h"
#pragma GCC diagnostic pop
#include "native_bc_spirv.h"
#include "native_bc_sample_spirv.h"
#define BC_FUNCTIONS(X) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements2) X(BindImageMemory) \
 X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) X(CreateShaderModule) X(DestroyShaderModule) \
 X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
 X(CreateComputePipelines) X(DestroyPipeline) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
 X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) \
 X(CmdDispatch) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(CmdCopyImage) \
 X(ResetCommandPool) X(ResetCommandBuffer) X(FreeCommandBuffers)
#define BC_DECL(n) static PFN_vk##n b##n;
BC_FUNCTIONS(BC_DECL)
static PFN_vkGetBufferDeviceAddress bGetBufferDeviceAddress;
#define BC_CHECK(call) do{VkResult r_=(call);if(r_!=VK_SUCCESS){fprintf(stderr,"VK_BC error line=%d result=%d call=%s\n",__LINE__,r_,#call);_Exit(2);}}while(0)
static VkDevice bd;static VkQueue bq;static VkPhysicalDeviceMemoryProperties bm;static unsigned bt;
static VkCommandPool bp;static VkCommandBuffer bc;
typedef struct {VkBuffer buffer;VkDeviceMemory memory;VkDeviceAddress address;uint8_t *map;size_t bytes;} BBuffer;
static BBuffer bbuffer(size_t bytes) {
    BBuffer b={.bytes=bytes};VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,
        .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    BC_CHECK(vkCreateBuffer(bd,&ci,NULL,&b.buffer));VkMemoryRequirements req;vkGetBufferMemoryRequirements(bd,b.buffer,&req);
    if(!(req.memoryTypeBits&(1u<<bt)))_Exit(2);
    VkMemoryAllocateFlagsInfo flags={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&flags,.allocationSize=req.size,.memoryTypeIndex=bt};
    BC_CHECK(vkAllocateMemory(bd,&ai,NULL,&b.memory));BC_CHECK(vkBindBufferMemory(bd,b.buffer,b.memory,0));
    BC_CHECK(vkMapMemory(bd,b.memory,0,VK_WHOLE_SIZE,0,(void**)&b.map));
    VkBufferDeviceAddressInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer=b.buffer};
    b.address=bGetBufferDeviceAddress(bd,&info);if(!b.address){fprintf(stderr,"VK_BC missing_address\n");_Exit(2);}return b;
}
static void bfree(BBuffer b){vkUnmapMemory(bd,b.memory);vkDestroyBuffer(bd,b.buffer,NULL);vkFreeMemory(bd,b.memory,NULL);}
static void bflush(BBuffer b,bool read){VkMappedMemoryRange r={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=b.memory,.size=VK_WHOLE_SIZE};
    if(read)BC_CHECK(vkInvalidateMappedMemoryRanges(bd,1,&r));else BC_CHECK(vkFlushMappedMemoryRanges(bd,1,&r));}
static void bbegin(void){BC_CHECK(bResetCommandPool(bd,bp,0));VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};BC_CHECK(vkBeginCommandBuffer(bc,&begin));}
static void bbarrier(void){VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,
    .dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};vkCmdPipelineBarrier(bc,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,NULL,0,NULL);}
static void bsubmit(void){bbarrier();BC_CHECK(vkEndCommandBuffer(bc));VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence f;
    BC_CHECK(vkCreateFence(bd,&fi,NULL,&f));VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&bc};
    BC_CHECK(vkQueueSubmit(bq,1,&si,f));BC_CHECK(vkWaitForFences(bd,1,&f,VK_TRUE,15000000000ull));vkDestroyFence(bd,f,NULL);}
static uint32_t bseed=0x397519ab;
static uint32_t brand(void){bseed^=bseed<<13;bseed^=bseed>>17;bseed^=bseed<<5;return bseed;}
static void breference(unsigned fmt,const void *input,uint8_t output[128]) {
    memset(output,0,128);
    switch(fmt){case 0:case 1:bcdec_bc1(input,output,16);if(fmt==1)for(unsigned i=0;i<16;i++)output[i*4+3]=255;break;
    case 2:bcdec_bc2(input,output,16);break;case 3:bcdec_bc3(input,output,16);break;
    case 4:case 5:bcdec_bc4(input,output,4,fmt==5);break;case 6:case 7:bcdec_bc5(input,output,8,fmt==7);break;
    case 8:case 9:{uint16_t rgb[48];bcdec_bc6h_half(input,rgb,12,fmt==9);uint16_t *o=(uint16_t*)output;
        for(unsigned i=0;i<16;i++){for(unsigned c=0;c<3;c++)o[i*4+c]=rgb[i*3+c];o[i*4+3]=0x3c00;}break;}
    case 10:bcdec_bc7(input,output,16);break;}
}
static unsigned bpixel(unsigned fmt){return fmt==4||fmt==5?1:fmt==6||fmt==7?2:fmt==8||fmt==9?8:4;}
static unsigned bblock(unsigned fmt){return fmt<=1||fmt==4||fmt==5?8:16;}
typedef struct {uint64_t src,dst;uint32_t srcRow,srcSlice,dstRow,dstSlice,bx,by,format,flags;} BPC;
_Static_assert(sizeof(BPC)==48,"shader ABI");
static VkShaderModule bshader(const uint32_t *code,size_t bytes){VkShaderModuleCreateInfo info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=bytes,.pCode=code};VkShaderModule m;BC_CHECK(bCreateShaderModule(bd,&info,NULL,&m));return m;}
static VkPipeline bpipe(VkShaderModule shader,VkPipelineLayout layout){VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"},.layout=layout};VkPipeline pipe;
    BC_CHECK(bCreateComputePipelines(bd,VK_NULL_HANDLE,1,&ci,NULL,&pipe));return pipe;}
static void bdispatch(VkPipeline pipeline,VkPipelineLayout layout,BPC pc){bCmdBindPipeline(bc,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    bCmdPushConstants(bc,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),&pc);bCmdDispatch(bc,(pc.bx+7)/8,(pc.by+7)/8,1);}
static float bhalf(uint16_t h){int e=(h>>10)&31;float v=e?ldexpf(1+(h&1023)/1024.0f,e-15):ldexpf(h&1023,-24);return h&32768?-v:v;}
static int bclamp(int v){return v<0?0:v>255?255:v;}
static void betc(const uint8_t *bytes,unsigned x,unsigned y,unsigned alpha,float out[4]) {
    static const int mods[8][4]={{2,8,-2,-8},{5,17,-5,-17},{9,29,-9,-29},{13,42,-13,-42},{18,60,-18,-60},{24,80,-24,-80},{33,106,-33,-106},{47,183,-47,-183}};
    static const int am[16][8]={{-3,-6,-9,-15,2,5,8,14},{-3,-7,-10,-13,2,6,9,12},{-2,-5,-8,-13,1,4,7,12},{-2,-4,-6,-13,1,3,5,12},
        {-3,-6,-8,-12,2,5,7,11},{-3,-7,-9,-11,2,6,8,10},{-4,-7,-8,-11,3,6,7,10},{-3,-5,-8,-11,2,4,7,10},
        {-2,-6,-8,-10,1,5,7,9},{-2,-5,-8,-10,1,4,7,9},{-2,-4,-8,-10,1,3,7,9},{-2,-5,-7,-10,1,4,6,9},
        {-3,-4,-7,-10,2,3,6,9},{-1,-2,-3,-10,0,1,2,9},{-4,-6,-8,-9,3,5,7,8},{-3,-5,-7,-9,2,4,6,8}};
    const uint8_t *c=bytes+(alpha==1?8:0);uint32_t hi=((uint32_t)c[0]<<24)|((uint32_t)c[1]<<16)|((uint32_t)c[2]<<8)|c[3];
    uint32_t lo=((uint32_t)c[4]<<24)|((uint32_t)c[5]<<16)|((uint32_t)c[6]<<8)|c[7];
    unsigned half=x/2,table=(hi>>(half?2:5))&7,bit=x*4+y,sel=((lo>>bit)&1)|(((lo>>(bit+16))&1)<<1);
    if(alpha==2) {
        bool opaque=!!(hi&2u);
        if(!opaque&&sel==2){for(unsigned k=0;k<4;k++)out[k]=0;return;}
        for(unsigned k=0;k<3;k++) {
            int base=(int)((hi>>(27-k*8))&31),delta=(int)((hi>>(24-k*8))&7);if(delta&4)delta-=8;
            if(half)base+=delta;
            out[k]=bclamp(((base<<3)|(base>>2))+(!opaque&&sel==0?0:mods[table][sel]))/255.0f;
        }
    } else for(unsigned k=0;k<3;k++)out[k]=bclamp((int)((hi>>((half?24:28)-k*8))&15)*17+mods[table][sel])/255.0f;
    out[3]=1;if(alpha==1){uint64_t indices=0;for(unsigned i=2;i<8;i++)indices=(indices<<8)|bytes[i];
        unsigned a=(indices>>(45-3*(x*4+y)))&7;out[3]=bclamp(bytes[0]+(bytes[1]>>4)*am[bytes[1]&15][a])/255.0f;}
}
static int run_bc_probe(VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family,unsigned type,
    const VkPhysicalDeviceMemoryProperties *memory,PFN_vkGetInstanceProcAddr get,VkInstance instance) {
    bd=device;bq=queue;bm=*memory;bt=type;
#define BC_LOAD(n) b##n=(PFN_vk##n)vkGetDeviceProcAddr(device,"vk" #n);if(!b##n)return 2;
    BC_FUNCTIONS(BC_LOAD)
#undef BC_LOAD
    bGetBufferDeviceAddress=(PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(device,"vkGetBufferDeviceAddress");
    if(!bGetBufferDeviceAddress)bGetBufferDeviceAddress=(PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(device,"vkGetBufferDeviceAddressKHR");
    if(!bGetBufferDeviceAddress)return 2;
    PFN_vkGetPhysicalDeviceFormatProperties fp=(PFN_vkGetPhysicalDeviceFormatProperties)get(instance,"vkGetPhysicalDeviceFormatProperties");
    PFN_vkGetPhysicalDeviceFeatures features=(PFN_vkGetPhysicalDeviceFeatures)get(instance,"vkGetPhysicalDeviceFeatures");
    VkPhysicalDeviceFeatures feature;features(physical,&feature);fprintf(stderr,"VK_BC begin feature_bc=%u synthetic_readbacks=1 display_readbacks=0\n",feature.textureCompressionBC);
    if(!feature.textureCompressionBC)return 3;
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};BC_CHECK(vkCreateCommandPool(bd,&pi,NULL,&bp));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=bp,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};BC_CHECK(vkAllocateCommandBuffers(bd,&ca,&bc));
    VkPushConstantRange range={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.size=48};VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.pushConstantRangeCount=1,.pPushConstantRanges=&range};
    VkPipelineLayout layout;BC_CHECK(bCreatePipelineLayout(bd,&li,NULL,&layout));VkShaderModule shader=bshader(native_bc_spirv,sizeof(native_bc_spirv));VkPipeline pipeline=bpipe(shader,layout);
    enum{BX=16,BY=8,W=BX*4,H=BY*4};BBuffer in=bbuffer(BX*BY*16+512),out=bbuffer(W*H*8+512);
    unsigned total_wrong=0,decode_cases=0;
    for(unsigned fmt=0;fmt<11;fmt++) {
        unsigned bb=bblock(fmt),pb=bpixel(fmt);uint32_t *input=(uint32_t*)(in.map+256);unsigned words=BX*BY*bb/4;
        for(unsigned i=0;i<words;i++)input[i]=brand();
        // Cover every BC7 mode instead of letting random data favor mode 0.
        if(fmt==10)for(unsigned i=0;i<BX*BY;i++)input[i*4]=(input[i*4]&~255u)|(1u<<(i%8));
        memset(out.map,0xa5,out.bytes);bflush(in,false);bflush(out,false);bbegin();
        BPC pc={in.address+256,out.address+256,BX*bb,BX*BY*bb,W*pb,W*H*pb,BX,BY,fmt,1};bdispatch(pipeline,layout,pc);bsubmit();bflush(out,true);
        unsigned wrong=0,guard=0,max_error=0;
        for(unsigned by=0;by<BY;by++)for(unsigned bx=0;bx<BX;bx++) {
            uint8_t expected[128];breference(fmt,(uint8_t*)input+(by*BX+bx)*bb,expected);
            for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)for(unsigned c=0;c<pb;c++) {
                unsigned index=((by*4+y)*W+bx*4+x)*pb+c;unsigned a=out.map[256+index],e=expected[(y*4+x)*pb+c];
                unsigned diff=abs((int)a-(int)e);max_error=diff>max_error?diff:max_error;
                if(diff>(pb==8?0u:1u)){if(wrong<2)fprintf(stderr,"VK_BC decode_mismatch fmt=%u block=%u pixel=%u component=%u actual=%u expected=%u\n",fmt,by*BX+bx,y*4+x,c,a,e);wrong++;}
            }
        }
        for(unsigned i=0;i<256;i++)guard+=out.map[i]!=0xa5;
        for(unsigned i=256+W*H*pb;i<out.bytes;i++)guard+=out.map[i]!=0xa5;
        fprintf(stderr,"VK_BC decoder fmt=%u blocks=%u wrong=%u guards=%u max_byte_error=%u\n",fmt,BX*BY,wrong,guard,max_error);total_wrong+=wrong+guard;decode_cases++;
    }
    bfree(in);bfree(out);
    if(getenv("VP_BC_DECODER_ONLY")) {
        bFreeCommandBuffers(bd,bp,1,&bc);vkDestroyCommandPool(bd,bp,NULL);bDestroyPipeline(bd,pipeline,NULL);bDestroyPipelineLayout(bd,layout,NULL);bDestroyShaderModule(bd,shader,NULL);
        fprintf(stderr,"VK_BC decoder_control cases=%u mismatches=%u result=%u resource_tests=not_executed\n",decode_cases,total_wrong,total_wrong!=0);return total_wrong?1:0;
    }
    if(total_wrong){fprintf(stderr,"VK_BC complete decoder_cases=%u mismatches=%u result=1\n",decode_cases,total_wrong);return 1;}
    // Actual advertised BC image path: odd dimensions, mip/layer row padding,
    // two-image copies, exact readback, hardware sampling and compute state.
    const unsigned ids[16]={1,1,0,0,2,2,3,3,4,5,6,7,8,9,10,10};
    unsigned resource_cases=0;
    for(unsigned fi=0;fi<16;fi++) {
        VkFormat f=(VkFormat)(VK_FORMAT_BC1_RGB_UNORM_BLOCK+fi);unsigned fmt=ids[fi],bb=bblock(fmt),pb=bpixel(fmt);
        bool srgb=fi==1||fi==3||fi==5||fi==7||fi==15,encoded=fmt<=3||fmt==10;unsigned eb=fmt<=1?8:16;
        VkFormatProperties props;fp(physical,f,&props);if(!(props.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))return 3;
        BBuffer src=bbuffer(65536),raw=bbuffer(65536),reference=bbuffer(65536),sample=bbuffer(65536);
        uint8_t *expected_raw=malloc(65536);if(!expected_raw)return 2;
        memset(src.map,0xa5,src.bytes);memset(raw.map,0xa5,raw.bytes);memset(reference.map,0xa5,reference.bytes);memset(sample.map,0xa5,sample.bytes);
        VkBufferImageCopy regions[6];size_t cursor=256,ref_offset[6];size_t ref_cursor=256;
        for(unsigned mip=0;mip<3;mip++)for(unsigned layer=0;layer<2;layer++) {
            unsigned n=mip*2+layer,w=31u>>mip,h=17u>>mip,bx=(w+3)/4,by=(h+3)/4;
            regions[n]=(VkBufferImageCopy){.bufferOffset=cursor,.bufferRowLength=(bx+1)*4,.bufferImageHeight=(by+1)*4,
                .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,layer,1},.imageExtent={w,h,1}};
            for(unsigned y=0;y<by;y++)for(unsigned x=0;x<bx;x++)for(unsigned j=0;j<bb/4;j++)((uint32_t*)(src.map+cursor+y*(bx+1)*bb+x*bb))[j]=brand();
            if(fmt==0&&n==0) {
                // Guaranteed all-transparent, mixed alpha, opaque three-colour
                // and opaque four-colour blocks, in addition to random blocks.
                const uint32_t edge[8]={0xffff0000u,0xffffffffu,0xffff0000u,0xe4e4e4e4u,
                    0x0000ffffu,0xaaaaaaaau,0u,0u};
                memcpy(src.map+cursor,edge,sizeof(edge));
            }
            cursor+=(by+1)*(bx+1)*bb+256;ref_offset[n]=ref_cursor;ref_cursor+=bx*by*(encoded?eb:16*pb)+256;
        }
        memcpy(expected_raw,src.map,65536);
        for(unsigned i=0;i<4;i++)bflush((BBuffer[]){src,raw,reference,sample}[i],false);
        VkImage images_[2];VkDeviceMemory memories[2];size_t requirements[2];
        VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=f,.extent={31,17,1},.mipLevels=3,.arrayLayers=2,
            .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        for(unsigned j=0;j<2;j++) {
            BC_CHECK(bCreateImage(bd,&ii,NULL,&images_[j]));VkMemoryRequirements2 req={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};VkImageMemoryRequirementsInfo2 qi={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,.image=images_[j]};
            bGetImageMemoryRequirements2(bd,&qi,&req);unsigned mt=0;while(mt<bm.memoryTypeCount&&(!(req.memoryRequirements.memoryTypeBits&(1u<<mt))||!(bm.memoryTypes[mt].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))mt++;
            if(mt==bm.memoryTypeCount)return 3;
            size_t offset=req.memoryRequirements.alignment;requirements[j]=req.memoryRequirements.size;
            VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.memoryRequirements.size+offset,.memoryTypeIndex=mt};
            BC_CHECK(vkAllocateMemory(bd,&ai,NULL,&memories[j]));BC_CHECK(bBindImageMemory(bd,images_[j],memories[j],offset));
        }
        VkImage mixed;VkDeviceMemory mixed_memory;VkImageCreateInfo mixed_info=ii;
        mixed_info.format=bb==8?VK_FORMAT_R32G32_UINT:VK_FORMAT_R32G32B32A32_UINT;
        mixed_info.extent=(VkExtent3D){8,5,1};mixed_info.mipLevels=1;mixed_info.arrayLayers=1;
        mixed_info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        BC_CHECK(bCreateImage(bd,&mixed_info,NULL,&mixed));
        VkMemoryRequirements2 mixed_req={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};VkImageMemoryRequirementsInfo2 mixed_query={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,.image=mixed};
        bGetImageMemoryRequirements2(bd,&mixed_query,&mixed_req);
        unsigned mixed_type=0;while(mixed_type<bm.memoryTypeCount&&!(mixed_req.memoryRequirements.memoryTypeBits&(1u<<mixed_type)))mixed_type++;
        if(mixed_type==bm.memoryTypeCount)return 3;
        VkMemoryAllocateInfo mixed_ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mixed_req.memoryRequirements.size,.memoryTypeIndex=mixed_type};
        BC_CHECK(vkAllocateMemory(bd,&mixed_ai,NULL,&mixed_memory));BC_CHECK(bBindImageMemory(bd,mixed,mixed_memory,0));
        VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=images_[1],.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY,.format=f,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,3,0,2}};
        VkImageView view;BC_CHECK(bCreateImageView(bd,&vi,NULL,&view));VkSampler sampler;VkSamplerCreateInfo sci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.maxLod=3};BC_CHECK(bCreateSampler(bd,&sci,NULL,&sampler));
        VkDescriptorSetLayoutBinding bindings[2]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
        VkDescriptorSetLayoutCreateInfo dli={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};VkDescriptorSetLayout dsl;BC_CHECK(bCreateDescriptorSetLayout(bd,&dli,NULL,&dsl));
        VkPushConstantRange spr={VK_SHADER_STAGE_COMPUTE_BIT,0,16};VkPipelineLayoutCreateInfo sli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&dsl,.pushConstantRangeCount=1,.pPushConstantRanges=&spr};
        VkPipelineLayout sl;BC_CHECK(bCreatePipelineLayout(bd,&sli,NULL,&sl));VkShaderModule sm=bshader(native_bc_sample_spirv,sizeof(native_bc_sample_spirv));VkPipeline sp=bpipe(sm,sl);
        VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
        VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};VkDescriptorPool dp;BC_CHECK(bCreateDescriptorPool(bd,&dpi,NULL,&dp));
        VkDescriptorSetAllocateInfo dai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=dp,.descriptorSetCount=1,.pSetLayouts=&dsl};VkDescriptorSet ds;BC_CHECK(bAllocateDescriptorSets(bd,&dai,&ds));
        VkDescriptorImageInfo di={sampler,view,VK_IMAGE_LAYOUT_GENERAL};VkDescriptorBufferInfo db={sample.buffer,256,31*17*16};
        VkWriteDescriptorSet writes[2]={{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&di},
            {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&db}};bUpdateDescriptorSets(bd,2,writes,0,NULL);
        bbegin();
        for(unsigned j=0;j<2;j++) {VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=images_[j],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,3,0,2}};
            vkCmdPipelineBarrier(bc,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);}
        for(unsigned n=0;n<6;n++) {
            unsigned w=regions[n].imageExtent.width,h=regions[n].imageExtent.height,bx=(w+3)/4,by=(h+3)/4;
            BPC pc={src.address+regions[n].bufferOffset,reference.address+ref_offset[n],(bx+1)*bb,(bx+1)*(by+1)*bb,
                encoded?bx*eb:bx*4*pb,encoded?bx*by*eb:bx*by*16*pb,bx,by,fmt,0};bdispatch(pipeline,layout,pc);
        }
        bbarrier();uint32_t pc_sample[4]={31,17,0,0};bCmdBindPipeline(bc,VK_PIPELINE_BIND_POINT_COMPUTE,sp);
        bCmdBindDescriptorSets(bc,VK_PIPELINE_BIND_POINT_COMPUTE,sl,0,1,&ds,0,NULL);bCmdPushConstants(bc,sl,VK_SHADER_STAGE_COMPUTE_BIT,0,16,pc_sample);
        bCmdCopyBufferToImage(bc,src.buffer,images_[0],VK_IMAGE_LAYOUT_GENERAL,6,regions);bbarrier();
        VkImageCopy copies[6];for(unsigned n=0;n<6;n++)copies[n]=(VkImageCopy){regions[n].imageSubresource,{0,0,0},regions[n].imageSubresource,{0,0,0},regions[n].imageExtent};
        bCmdCopyImage(bc,images_[0],VK_IMAGE_LAYOUT_GENERAL,images_[1],VK_IMAGE_LAYOUT_GENERAL,6,copies);bbarrier();
        VkImageMemoryBarrier mixed_transition={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=mixed,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        vkCmdPipelineBarrier(bc,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&mixed_transition);
        VkImageCopy mixed_copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={31,17,1}};
        bCmdCopyImage(bc,images_[0],VK_IMAGE_LAYOUT_GENERAL,mixed,VK_IMAGE_LAYOUT_GENERAL,1,&mixed_copy);bbarrier();
        VkBufferImageCopy mixed_read={.bufferOffset=60000,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={8,5,1}};
        bCmdCopyImageToBuffer(bc,mixed,VK_IMAGE_LAYOUT_GENERAL,raw.buffer,1,&mixed_read);
        mixed_copy.dstSubresource.baseArrayLayer=1;mixed_copy.extent=(VkExtent3D){8,5,1};
        bCmdCopyImage(bc,mixed,VK_IMAGE_LAYOUT_GENERAL,images_[1],VK_IMAGE_LAYOUT_GENERAL,1,&mixed_copy);bbarrier();
        for(unsigned y=0;y<5;y++) {
            memcpy(expected_raw+60000+y*8*bb,src.map+regions[0].bufferOffset+y*9*bb,8*bb);
            memcpy(expected_raw+regions[1].bufferOffset+y*9*bb,src.map+regions[0].bufferOffset+y*9*bb,8*bb);
        }
        VkBufferImageCopy partial=regions[1];partial.imageOffset=(VkOffset3D){4,4,0};partial.imageExtent=(VkExtent3D){27,13,1};
        bCmdCopyBufferToImage(bc,src.buffer,images_[1],VK_IMAGE_LAYOUT_GENERAL,1,&partial);bbarrier();
        for(unsigned y=0;y<4;y++)memcpy(expected_raw+regions[1].bufferOffset+(y+1)*9*bb+bb,src.map+regions[1].bufferOffset+y*9*bb,7*bb);
        bCmdCopyImageToBuffer(bc,images_[1],VK_IMAGE_LAYOUT_GENERAL,raw.buffer,6,regions);bbarrier();
        // No rebind after the meta work: app pipeline/descriptors/push state
        // must survive all six upload/copy transcodes.
        bCmdDispatch(bc,4,3,1);bsubmit();bflush(raw,true);bflush(reference,true);bflush(sample,true);
        unsigned raw_wrong=0,sampled_wrong=0,guard_wrong=0,alpha_wrong=0;for(unsigned i=0;i<65536;i++)raw_wrong+=expected_raw[i]!=raw.map[i];
        // Both maps started with equal guard bytes; only original BC payload
        // rows may be written by exact copy/readback.
        float *pixels=(float*)(sample.map+256);
        for(unsigned y=0;y<17;y++)for(unsigned x=0;x<31;x++) {
            float expected[4]={0,0,0,1};unsigned bx=(31+3)/4;const uint8_t *base=reference.map+ref_offset[0];
            if(encoded)betc(base+((y/4)*bx+x/4)*eb,x%4,y%4,fmt==0?2:fmt!=1,expected);
            else {const uint8_t *p=base+(y*bx*4+x)*pb;
                if(pb==8){for(unsigned c=0;c<4;c++)expected[c]=bhalf(((const uint16_t*)p)[c]);}
                else for(unsigned c=0;c<pb;c++)expected[c]=(fmt==5||fmt==7)?fmaxf(-1,((const int8_t*)p)[c]/127.0f):p[c]/255.0f;}
            if(srgb)for(unsigned c=0;c<3;c++)expected[c]=expected[c]<=0.04045f?expected[c]/12.92f:powf((expected[c]+0.055f)/1.055f,2.4f);
            if(fmt==0) {
                uint8_t decoded[128];breference(0,src.map+regions[0].bufferOffset+((y/4)*9+x/4)*bb,decoded);
                float alpha=decoded[((y%4)*4+x%4)*4+3]/255.0f;
                alpha_wrong+=!isfinite(pixels[(y*31+x)*4+3])||fabsf(pixels[(y*31+x)*4+3]-alpha)>0.001f;
            }
            for(unsigned c=0;c<4;c++){float actual=pixels[(y*31+x)*4+c];float tolerance=pb==8?fmaxf(0.001f,fabsf(expected[c])*0.001f):0.006f;
                if(!isfinite(actual)||fabsf(actual-expected[c])>tolerance){if(sampled_wrong<2)fprintf(stderr,"VK_BC sample_mismatch format=%u x=%u y=%u c=%u actual=%g expected=%g\n",f,x,y,c,actual,expected[c]);sampled_wrong++;}}
        }
        for(unsigned i=0;i<256;i++)guard_wrong+=sample.map[i]!=0xa5;
        for(unsigned i=256+31*17*16;i<sample.bytes;i++)guard_wrong+=sample.map[i]!=0xa5;
        fprintf(stderr,"VK_BC resource format=%u mip_levels=3 layers=2 bytes=%zu raw_wrong=%u sampled_wrong=%u alpha_wrong=%u guards=%u state_restored=1 mixed_raw_copies=2 partial_update=1\n",f,requirements[0],raw_wrong,sampled_wrong,alpha_wrong,guard_wrong);
        total_wrong+=raw_wrong+sampled_wrong+alpha_wrong+guard_wrong;resource_cases++;
        // Drop recorded private scratch/replay references before app objects.
        BC_CHECK(bResetCommandPool(bd,bp,0));bDestroyDescriptorPool(bd,dp,NULL);bDestroyPipeline(bd,sp,NULL);bDestroyShaderModule(bd,sm,NULL);bDestroyPipelineLayout(bd,sl,NULL);bDestroyDescriptorSetLayout(bd,dsl,NULL);
        bDestroySampler(bd,sampler,NULL);bDestroyImageView(bd,view,NULL);
        for(unsigned j=0;j<2;j++){bDestroyImage(bd,images_[j],NULL);vkFreeMemory(bd,memories[j],NULL);}
        bDestroyImage(bd,mixed,NULL);vkFreeMemory(bd,mixed_memory,NULL);free(expected_raw);
        bfree(src);bfree(raw);bfree(reference);bfree(sample);
    }
    bFreeCommandBuffers(bd,bp,1,&bc);vkDestroyCommandPool(bd,bp,NULL);bDestroyPipeline(bd,pipeline,NULL);bDestroyPipelineLayout(bd,layout,NULL);bDestroyShaderModule(bd,shader,NULL);
    fprintf(stderr,"VK_BC complete decoder_cases=%u resource_cases=%u mismatches=%u result=%u\n",decode_cases,resource_cases,total_wrong,total_wrong!=0);
    return total_wrong?1:0;
}
