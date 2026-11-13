# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise the prepared Venus capability guard against renderer contradictions.

No GPU/runtime qualification is implied by these host query doubles.
"""
import os
from pathlib import Path
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[1]
SOURCE=Path(os.environ.get('VPP_GNU_MESA_TEST_SOURCE',str(ROOT/'build/gnu-venus-overlays/source')))
TASK=ROOT/'build/host_tests/gnu-venus-bc-contract'

PREAMBLE=r'''
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
struct vn_ring { int unused; };
struct vn_instance { int unused; };
static unsigned queries,logs;
static VkFormat broken;
static unsigned fault;
static void vn_log(struct vn_instance *instance,const char *format,...) {(void)instance;(void)format;++logs;}
static void vn_call_vkGetPhysicalDeviceFormatProperties2(struct vn_ring *r,VkPhysicalDevice p,VkFormat f,VkFormatProperties2 *out) {
    (void)r;(void)p;++queries;
    assert(f>=VK_FORMAT_BC1_RGB_UNORM_BLOCK&&f<=VK_FORMAT_BC7_SRGB_BLOCK);
    assert(out->sType==VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2&&!out->pNext);
    out->formatProperties.optimalTilingFeatures=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if(f==broken&&fault==1)out->formatProperties.optimalTilingFeatures&=~VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
}
static VkResult vn_call_vkGetPhysicalDeviceImageFormatProperties2(struct vn_ring *r,VkPhysicalDevice p,const VkPhysicalDeviceImageFormatInfo2 *info,VkImageFormatProperties2 *out) {
    (void)r;(void)p;++queries;
    assert(info->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2&&!info->pNext);
    assert(info->usage==(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    assert(info->type==VK_IMAGE_TYPE_2D&&info->tiling==VK_IMAGE_TILING_OPTIMAL&&!info->flags);
    assert(out->sType==VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2&&!out->pNext);
    out->imageFormatProperties.sampleCounts=info->format==broken&&fault==3?0:VK_SAMPLE_COUNT_1_BIT;
    return info->format==broken&&fault==2?VK_ERROR_FORMAT_NOT_SUPPORTED:VK_SUCCESS;
}
'''
MAIN=r'''
int main(void) {
    VkPhysicalDeviceFeatures original={.textureCompressionBC=VK_TRUE,.robustBufferAccess=VK_TRUE,.textureCompressionETC2=VK_TRUE};
    VkPhysicalDeviceFeatures f=original;
    vn_physical_device_check_bc_features(NULL,NULL,VK_NULL_HANDLE,&f);
    assert(!memcmp(&f,&original,sizeof(f))&&queries==32&&logs==0);
    f.textureCompressionBC=VK_FALSE;queries=0;
    vn_physical_device_check_bc_features(NULL,NULL,VK_NULL_HANDLE,&f);
    assert(!f.textureCompressionBC&&queries==0&&logs==0);
    for(broken=VK_FORMAT_BC1_RGB_UNORM_BLOCK;broken<=VK_FORMAT_BC7_SRGB_BLOCK;++broken) {
        for(fault=1;fault<=3;++fault) {
            f=original;queries=logs=0;
            vn_physical_device_check_bc_features(NULL,NULL,VK_NULL_HANDLE,&f);
            assert(!f.textureCompressionBC&&logs==1&&queries<=32);
            f.textureCompressionBC=VK_TRUE;assert(!memcmp(&f,&original,sizeof(f)));
        }
    }
    puts("PASS: complete BC retained; unsupported stays off; 48 format/image/sample contradictions lower only aggregate; no render-path work");
}
'''

class ContractTests(unittest.TestCase):
    def test_actual_venus_guard(self):
        source=(SOURCE/'src/virtio/vulkan/vn_physical_device.c').read_text()
        begin=source.index('static void\nvn_physical_device_check_bc_features(')
        end=source.index('static void\nvn_physical_device_init_features(',begin)
        TASK.mkdir(parents=True,exist_ok=True)
        fixture=TASK/'contract.c';binary=TASK/'contract'
        fixture.write_text(PREAMBLE+source[begin:end]+MAIN)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(SOURCE/'include'),fixture,'-o',binary],check=True,timeout=30)
        subprocess.run([binary],check=True,timeout=10)

if __name__=='__main__':unittest.main()
