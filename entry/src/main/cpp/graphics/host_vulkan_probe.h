#pragma once

#include <cstdint>
#include <string>

bool StartHostVulkanProbe(uint64_t surfaceId, const std::string& runId, bool harmonyGpu = false);
void StopHostVulkanProbe();
struct HarmonyGpuProbeResult {
    std::string runId;
    std::string stage;
    std::string detail;
    uint32_t realFrames = 0;
    uint32_t generatedFrames = 0;
    uint32_t baselineFrames = 0;
    int32_t sdkError = 0;
};
HarmonyGpuProbeResult GetHarmonyGpuProbeResult();
/** 轻量查询 host Vulkan 物理设备名称 (GPU 型号, 如 "Mali-G920")。无 Vulkan
 * 环境/失败时返回空串。用于 ArkTS 按 GPU 能力控制 DXVK2.6 等选项。 */
std::string ProbeGpuDeviceName();
