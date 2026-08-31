#define VK_USE_PLATFORM_OHOS 1

#include "venus_surface_presenter.h"
#include "native_window_vk_target.h"
#include "native_window_lease.h"
#include "present_pacing.h"
#include "present_policy.h"
#include "presenter_common.h"

#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "venus-presenter"

namespace winehua {
namespace {

using SteadyClock = std::chrono::steady_clock;


enum class VulkanPresentTransport {
    Unprobed,
    DirectNativeBuffer,
    Wsi,
    DirectFallbackPending,
};


VkPipelineStageFlags SourceStage(VkImageLayout layout)
{
    switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return VK_PIPELINE_STAGE_TRANSFER_BIT;
    default:
        return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
}

VkAccessFlags SourceAccess(VkImageLayout layout)
{
    switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_GENERAL:
        return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    default:
        return VK_ACCESS_MEMORY_READ_BIT;
    }
}

VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported)
{
    constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> choices = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (const auto choice : choices) {
        if (supported & choice) return choice;
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

bool IsRecoverableWsiTargetLoss(VkResult result)
{
    return result == VK_ERROR_OUT_OF_DATE_KHR ||
           result == VK_ERROR_SURFACE_LOST_KHR ||
           result == VK_ERROR_UNKNOWN;
}

} // namespace

struct VenusSurfaceQueueTarget::Impl {
    struct Frame {
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkSemaphore released = VK_NULL_HANDLE;
        VkFence complete = VK_NULL_HANDLE;
        VkQueryPool gpuTiming = VK_NULL_HANDLE;
        bool submitted = false;
    };

    bool GpuFrameProfileEnabled() const
    {
        return policy_.gpuFrameProfile;
    }

    // This is deliberately a presenter-only timestamp, not a scene GPU
    // timer. Sample it sparsely so ordinary frames retain the production path.
    bool GpuFrameProfileSample(uint32_t serial) const
    {
        return policy_.gpuFrameProfile && serial && !(serial % 120);
    }

    bool TraceFrameOrder() const
    {
        return policy_.traceFrameOrder;
    }

    void TracePresentStage(const char* stage, uint32_t serial,
                           uint64_t sourceImage) const
    {
        if (!policy_.traceStages) return;
        OH_LOG_INFO(LOG_APP,
                    "[VENUS-TRACE][NCP] serial=%{public}u stage=%{public}s "
                    "source=0x%{public}llx timestamp=%{public}llu",
                    serial, stage, static_cast<unsigned long long>(sourceImage),
                    static_cast<unsigned long long>(NowNs()));
    }

    int Attach(uint64_t surfaceKey, uint64_t framePeriodNs, OHNativeWindow* window,
               bool releaseWindowWithUnreference)
    {
        if (!surfaceKey || !window) return -EINVAL;
        std::lock_guard<std::mutex> lock(mutex_);
        if (device_) return -EBUSY;
        ReleaseWindowLocked();
        windowLease_.Adopt(
            window, releaseWindowWithUnreference
                ? NativeWindowReleaseMode::UnreferenceNativeObject
                : NativeWindowReleaseMode::DestroyParcelWindow);
        surfaceKey_ = surfaceKey;
        surfaceAttached_ = true;
        deviceReleasing_ = false;
        transport_ = VulkanPresentTransport::Unprobed;
        transportReason_ = "not-probed";
        displayPeriodNs_ = NormalizePresentFramePeriodNs(framePeriodNs);
        framePeriodNs_ = PresentPacingPeriodNs(displayPeriodNs_);
        lastPresentNs_ = 0;
        framesPresented_ = 0;
        guestDeadlinePacingLogged_ = false;
        policy_ = ReadPresenterRuntimePolicyFromEnvironment();
        lastSerial_ = 0;
        serialRegressions_ = 0;
        failures_ = 0;
        throttled_ = 0;
        clockDeferred_ = 0;
        acquireDeferred_ = 0;
        fenceDeferred_ = 0;
        guestDeadlineFrames_ = 0;
        firstPresentedNs_ = 0;
        totalPresentUs_ = 0;
        maxPresentUs_ = 0;
        totalWaitFenceUs_ = 0;
        totalAcquireUs_ = 0;
        totalSubmitUs_ = 0;
        totalQueuePresentUs_ = 0;
        totalReleaseWaitUs_ = 0;
        totalGpuPresentWorkUs_ = 0;
        maxGpuPresentWorkUs_ = 0;
        gpuTimingSamples_ = 0;
        gpuTimingFailures_ = 0;
        OH_LOG_INFO(LOG_APP,
                    "[VENUS-PRESENT][NCP] target attached key=%{public}llu "
                    "window=%{public}p display_period_us=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey_), windowLease_.Get(),
                    static_cast<unsigned long long>(displayPeriodNs_ / 1000));
        return 0;
    }

    int Detach(uint64_t surfaceKey)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (surfaceKey_ && surfaceKey && surfaceKey_ != surfaceKey) return -EINVAL;
        OH_LOG_INFO(LOG_APP,
                    "[VENUS-PRESENT][NCP] detach begin key=%{public}llu window=%{public}p "
                    "device=%{public}p swapchain=%{public}p surface=%{public}p",
                    static_cast<unsigned long long>(surfaceKey), windowLease_.Get(),
                    device_, swapchain_, surface_);
        surfaceAttached_ = false;
        if (!device_) {
            ReleaseWindowLocked();
            surfaceKey_ = 0;
            OH_LOG_INFO(LOG_APP,
                        "[VENUS-PRESENT][NCP] target detached key=%{public}llu",
                        static_cast<unsigned long long>(surfaceKey));
        } else {
            OH_LOG_INFO(LOG_APP,
                        "[VENUS-PRESENT][NCP] target detach deferred to device owner "
                        "key=%{public}llu ctx=%{public}u device=%{public}p",
                        static_cast<unsigned long long>(surfaceKey), contextId_, device_);
        }
        return 0;
    }

    int SetFramePeriod(uint64_t framePeriodNs)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        displayPeriodNs_ = NormalizePresentFramePeriodNs(framePeriodNs);
        framePeriodNs_ = PresentPacingPeriodNs(displayPeriodNs_);
        return 0;
    }

    bool HasVulkanDevice()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return device_ != VK_NULL_HANDLE;
    }

    bool PrepareDeviceRelease(uint32_t contextId, uintptr_t device)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!MatchesDeviceLocked(contextId, device)) return false;
        deviceReleasing_ = true;
        OH_LOG_INFO(LOG_APP,
                    "[VENUS-PRESENT][NCP] device release prepare key=%{public}llu "
                    "ctx=%{public}u device=%{public}p attached=%{public}d",
                    static_cast<unsigned long long>(surfaceKey_), contextId,
                    device_, surfaceAttached_);
        return true;
    }

    bool FinishDeviceRelease(uint32_t contextId, uintptr_t device,
                             int32_t waitResult)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!MatchesDeviceLocked(contextId, device)) return false;
        OH_LOG_INFO(LOG_APP,
                    "[VENUS-PRESENT][NCP] device release after-wait key=%{public}llu "
                    "ctx=%{public}u device=%{public}p wait_result=%{public}d",
                    static_cast<unsigned long long>(surfaceKey_), contextId,
                    device_, waitResult);
        DestroyVulkanLocked();
        deviceReleasing_ = false;
        transport_ = surfaceAttached_ ? VulkanPresentTransport::Unprobed
                                      : VulkanPresentTransport::Wsi;
        transportReason_ = surfaceAttached_ ? "device-recreated" : "detached";
        if (!surfaceAttached_) {
            ReleaseWindowLocked();
            surfaceKey_ = 0;
        }
        return true;
    }

    int Present(uint32_t contextId,
                uintptr_t instance,
                uintptr_t physicalDevice,
                uintptr_t device,
                uintptr_t queue,
                uint64_t image,
                uint32_t queueFamily,
                uint32_t width,
                uint32_t height,
                uint32_t format,
                uint32_t layout,
                uint32_t serial,
                uint64_t* nextPresentDeadlineNs,
                void (*releaseQueue)(void*),
                void* queueSyncData)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t presentStartNs = NowNs();
        TracePresentStage("enter", serial, image);
        if (nextPresentDeadlineNs) *nextPresentDeadlineNs = 0;
        if (!surfaceAttached_ || !windowLease_) {
            OH_LOG_ERROR(LOG_APP, "[VENUS-PRESENT][NCP] present no window");
            return -EAGAIN;
        }
        if (deviceReleasing_) return -ENODEV;

        const uint64_t nowNs = NowNs();
        const PresentPacingDecision pacing =
            EvaluatePresentPacing(nowNs, lastPresentNs_, framePeriodNs_);
        const bool guestDeadlinePacing =
            DirectPresentUsesGuestDeadline(framesPresented_);
        if (!guestDeadlinePacing && !pacing.presentNow) {
            if (nextPresentDeadlineNs)
                *nextPresentDeadlineNs = pacing.nextDeadlineNs;
            ++throttled_;
            ++clockDeferred_;
            return 1;
        }
        if (guestDeadlinePacing && !guestDeadlinePacingLogged_) {
            guestDeadlinePacingLogged_ = true;
            OH_LOG_INFO(LOG_APP,
                        "[VENUS-PRESENT][NCP] guest_deadline_pacing=1 "
                        "clock_drop=0 acquire_timeout_ns=%{public}llu warmup=%{public}llu",
                        static_cast<unsigned long long>(
                            DirectPresentAcquireTimeoutNs(framesPresented_)),
                        static_cast<unsigned long long>(framesPresented_));
        }

        const VkInstance hostInstance = reinterpret_cast<VkInstance>(instance);
        const VkPhysicalDevice hostPhysical =
            reinterpret_cast<VkPhysicalDevice>(physicalDevice);
        const VkDevice hostDevice = reinterpret_cast<VkDevice>(device);
        const VkQueue hostQueue = reinterpret_cast<VkQueue>(queue);
        const VkImage sourceImage = reinterpret_cast<VkImage>(static_cast<uintptr_t>(image));
        const VkFormat sourceFormat = static_cast<VkFormat>(format);
        const VkImageLayout sourceLayout = static_cast<VkImageLayout>(layout);
        int initError = 0;
        if (device_ && (contextId_ != contextId || device_ != hostDevice))
            return -EAGAIN;
        if (!EnsureVulkanLocked(contextId, hostInstance, hostPhysical, hostDevice,
                                hostQueue, queueFamily, width, height,
                                sourceFormat, initError)) {
            ++failures_;
            return initError ? initError : -EIO;
        }
        Frame& frame = frames_[frameIndex_++ % frames_.size()];
        const bool gpuTiming = gpuTimingEnabled_ && frame.gpuTiming &&
            GpuFrameProfileSample(serial);
        uint64_t stageStartNs = NowNs();
        VkResult result = VK_SUCCESS;
        uint64_t waitFenceUs = 0;
        if (frame.submitted) {
            result = vkWaitForFences(
                device_, 1, &frame.complete, VK_TRUE,
                displayPeriodNs_ * 4);
            waitFenceUs = (NowNs() - stageStartNs) / 1000;
            if (result == VK_TIMEOUT) {
                ++throttled_;
                ++fenceDeferred_;
                if (nextPresentDeadlineNs)
                    *nextPresentDeadlineNs = RetryPresentDeadlineNs(
                        NowNs(), lastPresentNs_, framePeriodNs_);
                return 1;
            }
            if (result != VK_SUCCESS)
                return FailLocked("wait fence", result, serial);
            frame.submitted = false;
        }
        TracePresentStage("source-fence-ready", serial, image);

        if (transport_ == VulkanPresentTransport::DirectNativeBuffer) {
            const int32_t requestTimeoutMs = static_cast<int32_t>(
                DirectPresentAcquireTimeoutNs(framesPresented_) / 1000000ULL);
            if (!vkDirect_.SetRequestTimeoutMs(requestTimeoutMs)) {
                transport_ = VulkanPresentTransport::DirectFallbackPending;
                transportReason_ = "set-request-timeout-failed";
                return -EAGAIN;
            }
            stageStartNs = NowNs();
            const NativeWindowVkBeginResult beginResult = vkDirect_.BeginFrame();
            const uint64_t requestUs = (NowNs() - stageStartNs) / 1000;
            if (beginResult == NativeWindowVkBeginResult::Deferred) {
                ++throttled_;
                ++acquireDeferred_;
                if (nextPresentDeadlineNs) {
                    *nextPresentDeadlineNs = RetryPresentDeadlineNs(
                        NowNs(), lastPresentNs_, framePeriodNs_);
                }
                return 1;
            }
            if (beginResult != NativeWindowVkBeginResult::Ready) {
                transport_ = VulkanPresentTransport::DirectFallbackPending;
                transportReason_ = NativeWindowVkBeginResultName(beginResult);
                ++failures_;
                OH_LOG_ERROR(LOG_APP,
                             "[VENUS-PRESENT][NCP] direct runtime failure "
                             "key=%{public}llu reason=%{public}s fallback=pending",
                             static_cast<unsigned long long>(surfaceKey_),
                             transportReason_);
                return -EAGAIN;
            }
            TracePresentStage("direct-target-acquired", serial, image);

            result = vkDirect_.AcquireGpu(frame.acquired);
            if (result != VK_SUCCESS) {
                transport_ = VulkanPresentTransport::DirectFallbackPending;
                transportReason_ = "acquire-image-failed";
                return FailLocked("direct acquire image", result, serial);
            }

            const VkImage targetImage = vkDirect_.ColorImage();
            const VkFormat targetFormat = vkDirect_.ColorFormat();
            const uint32_t targetSeq = vkDirect_.CurrentSeq();
            if (!targetImage || targetFormat == VK_FORMAT_UNDEFINED) {
                vkDirect_.AbortFrame();
                transport_ = VulkanPresentTransport::DirectFallbackPending;
                transportReason_ = "invalid-imported-image";
                return -EAGAIN;
            }

            result = RecordPresentCopyLocked(
                frame, sourceImage, sourceLayout, targetImage, targetFormat,
                sourceWidth_, sourceHeight_, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL, false);
            if (result != VK_SUCCESS) {
                vkDirect_.AbortFrame();
                transport_ = VulkanPresentTransport::DirectFallbackPending;
                transportReason_ = result == VK_ERROR_FORMAT_NOT_SUPPORTED
                    ? "direct-format-blit-unsupported"
                    : "direct-copy-record-failed";
                return FailLocked("direct record copy", result, serial);
            }

            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &frame.acquired;
            submit.pWaitDstStageMask = &waitStage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &frame.command;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &frame.released;
            stageStartNs = NowNs();
            result = vkQueueSubmit(queue_, 1, &submit, frame.complete);
            const uint64_t submitUs = (NowNs() - stageStartNs) / 1000;
            if (result != VK_SUCCESS) {
                vkDirect_.AbortFrame();
                transport_ = VulkanPresentTransport::DirectFallbackPending;
                transportReason_ = "queue-submit-failed";
                return FailLocked("direct queue submit", result, serial);
            }
            frame.submitted = true;
            TracePresentStage("direct-copy-submitted", serial, image);

            int releaseFenceFd = -1;
            stageStartNs = NowNs();
            result = vkDirect_.SignalRelease(
                queue_, 1, &frame.released, &releaseFenceFd);
            const uint64_t releaseSignalUs = (NowNs() - stageStartNs) / 1000;
            if (releaseQueue) releaseQueue(queueSyncData);
            if (result != VK_SUCCESS) {
                const VkResult completionResult = vkWaitForFences(
                    device_, 1, &frame.complete, VK_TRUE,
                    kReleaseFenceWatchdogNs);
                if (completionResult == VK_SUCCESS) vkDirect_.AbortFrame();
                transport_ = VulkanPresentTransport::DirectFallbackPending;
                transportReason_ = "signal-release-image-failed";
                return FailLocked("direct signal release", result, serial);
            }

            const uint64_t timestamp = NowNs();
            stageStartNs = NowNs();
            const int32_t flushResult =
                vkDirect_.EndFrame(releaseFenceFd, timestamp);
            const uint64_t flushUs = (NowNs() - stageStartNs) / 1000;
            if (flushResult != 0) {
                transport_ = VulkanPresentTransport::DirectFallbackPending;
                transportReason_ = "flush-buffer-failed";
                ++failures_;
                return -EAGAIN;
            }

            lastPresentNs_ = timestamp;
            ++framesPresented_;
            if (guestDeadlinePacing) ++guestDeadlineFrames_;
            if (lastSerial_ && serial <= lastSerial_) ++serialRegressions_;
            lastSerial_ = serial;
            const uint64_t frameEndNs = NowNs();
            const uint64_t presentUs = (frameEndNs - presentStartNs) / 1000;
            if (!firstPresentedNs_) firstPresentedNs_ = frameEndNs;
            totalPresentUs_ += presentUs;
            maxPresentUs_ = std::max(maxPresentUs_, presentUs);
            totalWaitFenceUs_ += waitFenceUs;
            totalAcquireUs_ += requestUs;
            totalSubmitUs_ += submitUs;
            totalQueuePresentUs_ += releaseSignalUs + flushUs;
            if (GpuFrameProfileSample(serial)) {
                OH_LOG_INFO(LOG_APP,
                            "[VENUS-FRAME-TIMELINE][NCP] "
                            "transport=direct-native-buffer serial=%{public}u "
                            "release_wait_us=0 present_cpu_us=%{public}llu "
                            "wait_fence_us=%{public}llu acquire_us=%{public}llu "
                            "submit_us=%{public}llu queue_present_us=%{public}llu "
                            "release_signal_us=%{public}llu flush_us=%{public}llu "
                            "gpu_timing_available=0",
                            serial, static_cast<unsigned long long>(presentUs),
                            static_cast<unsigned long long>(waitFenceUs),
                            static_cast<unsigned long long>(requestUs),
                            static_cast<unsigned long long>(submitUs),
                            static_cast<unsigned long long>(
                                releaseSignalUs + flushUs),
                            static_cast<unsigned long long>(releaseSignalUs),
                            static_cast<unsigned long long>(flushUs));
            }
            if (nextPresentDeadlineNs) {
                *nextPresentDeadlineNs =
                    NextPresentDeadlineNs(lastPresentNs_, framePeriodNs_);
            }
            if (TraceFrameOrder() && framesPresented_ <= 600) {
                OH_LOG_INFO(LOG_APP,
                            "[VENUS-ORDER][NCP] transport=direct-native-buffer "
                            "frame=%{public}llu serial=%{public}u "
                            "serial_regress=%{public}llu source=0x%{public}llx "
                            "target_seq=%{public}u target=0x%{public}llx "
                            "timestamp=%{public}llu",
                            static_cast<unsigned long long>(framesPresented_), serial,
                            static_cast<unsigned long long>(serialRegressions_),
                            static_cast<unsigned long long>(image),
                            targetSeq,
                            static_cast<unsigned long long>(
                                reinterpret_cast<uintptr_t>(targetImage)),
                            static_cast<unsigned long long>(timestamp));
            }
            if (policy_.perfSummary &&
                (framesPresented_ == 1 || !(framesPresented_ % 120))) {
                const uint64_t elapsedNs = frameEndNs - firstPresentedNs_;
                const uint64_t fpsX100 = elapsedNs && framesPresented_ > 1
                    ? ((framesPresented_ - 1) * 100ULL * 1000000000ULL) /
                          elapsedNs
                    : 0;
                OH_LOG_INFO(LOG_APP,
                            "[VENUS-PRESENT][NCP] transport=direct-native-buffer "
                            "frames=%{public}llu key=%{public}llu serial=%{public}u "
                            "fps=%{public}llu.%{public}02llu "
                            "present_us_avg=%{public}llu max=%{public}llu "
                            "slot_wait_avg=%{public}llu request_us=%{public}llu "
                            "submit_us=%{public}llu release_signal_us=%{public}llu "
                            "flush_us=%{public}llu imported_slots=%{public}zu "
                            "post_present_cpu_wait=0 failures=%{public}llu "
                            "throttled=%{public}llu",
                            static_cast<unsigned long long>(framesPresented_),
                            static_cast<unsigned long long>(surfaceKey_), serial,
                            static_cast<unsigned long long>(fpsX100 / 100),
                            static_cast<unsigned long long>(fpsX100 % 100),
                            static_cast<unsigned long long>(
                                totalPresentUs_ / framesPresented_),
                            static_cast<unsigned long long>(maxPresentUs_),
                            static_cast<unsigned long long>(
                                totalWaitFenceUs_ / framesPresented_),
                            static_cast<unsigned long long>(requestUs),
                            static_cast<unsigned long long>(submitUs),
                            static_cast<unsigned long long>(releaseSignalUs),
                            static_cast<unsigned long long>(flushUs),
                            vkDirect_.ImportedSlotCount(),
                            static_cast<unsigned long long>(failures_),
                            static_cast<unsigned long long>(throttled_));
            }
            TracePresentStage("published", serial, image);
            return 0;
        }

        uint32_t imageIndex = 0;
        stageStartNs = NowNs();
        result = vkAcquireNextImageKHR(
            device_, swapchain_, DirectPresentAcquireTimeoutNs(framesPresented_),
            frame.acquired, VK_NULL_HANDLE, &imageIndex);
        const uint64_t acquireUs = (NowNs() - stageStartNs) / 1000;
        if (result == VK_TIMEOUT || result == VK_NOT_READY) {
            ++throttled_;
            ++acquireDeferred_;
            if (nextPresentDeadlineNs)
                *nextPresentDeadlineNs = RetryPresentDeadlineNs(
                    NowNs(), lastPresentNs_, framePeriodNs_);
            return 1;
        }
        /*
         * Some Harmony Vulkan drivers report a transient SurfaceQueue target
         * loss as VK_ERROR_UNKNOWN (rather than VK_ERROR_OUT_OF_DATE_KHR or
         * VK_ERROR_SURFACE_LOST_KHR).  Treat these WSI-only results as a
         * dirty swapchain and let the next present rebuild it.  No image was
         * acquired in this branch, so destroying the target is safe after the
         * per-frame fence wait above.  Do not convert it to device-lost: the
         * Vulkan device and guest Venus context remain usable.
         */
        if (IsRecoverableWsiTargetLoss(result)) {
            swapchainDirty_ = true;
            return -EAGAIN;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return FailLocked("acquire", result, serial);
        TracePresentStage("target-acquired", serial, image);

        result = RecordPresentCopyLocked(
            frame, sourceImage, sourceLayout, swapchainImages_[imageIndex],
            targetFormat_, extent_.width, extent_.height,
            targetInitialized_[imageIndex] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                           : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, gpuTiming);
        if (result != VK_SUCCESS)
            return FailLocked("record present copy", result, serial);

        const uint64_t timestamp = NowNs();
        OH_NativeWindow_NativeWindowHandleOpt(
            windowLease_.Get(), SET_UI_TIMESTAMP, timestamp);
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame.command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderFinished_[imageIndex];
        stageStartNs = NowNs();
        result = vkQueueSubmit(queue_, 1, &submit, frame.complete);
        const uint64_t submitUs = (NowNs() - stageStartNs) / 1000;
        if (result != VK_SUCCESS) return FailLocked("queue submit", result, serial);
        frame.submitted = true;
        TracePresentStage("copy-submitted", serial, image);

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderFinished_[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &imageIndex;
        stageStartNs = NowNs();
        result = vkQueuePresentKHR(queue_, &present);
        const uint64_t queuePresentUs = (NowNs() - stageStartNs) / 1000;
        TracePresentStage("queue-present-returned", serial, image);

        /* Vulkan only requires external synchronization while a host queue
         * command is executing. Keep the synchronous completion wait, but do
         * not block unrelated guest QueueSubmit calls on this queue mutex. */
        if (releaseQueue) releaseQueue(queueSyncData);

        VkResult fenceResult = VK_SUCCESS;
        uint64_t releaseWaitUs = 0;
        stageStartNs = NowNs();
        const uint64_t timeoutNs =
            std::max(displayPeriodNs_ * 4, kReleaseFenceWatchdogNs);
        fenceResult = vkWaitForFences(
            device_, 1, &frame.complete, VK_TRUE, timeoutNs);
        releaseWaitUs = (NowNs() - stageStartNs) / 1000;
        if (fenceResult == VK_TIMEOUT) {
            if (IsRecoverableWsiTargetLoss(result)) {
                swapchainDirty_ = true;
                return -EAGAIN;
            }
            if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
                return FailLocked("queue present", result, serial);
            targetInitialized_[imageIndex] = true;
            ++throttled_;
            ++fenceDeferred_;
            if (nextPresentDeadlineNs)
                *nextPresentDeadlineNs = RetryPresentDeadlineNs(
                    NowNs(), lastPresentNs_, framePeriodNs_);
            if (throttled_ == 1 || !(throttled_ % 60)) {
                OH_LOG_WARN(LOG_APP,
                            "[VENUS-PRESENT][NCP] source release fence watchdog "
                            "serial=%{public}u wait_us=%{public}llu throttled=%{public}llu",
                            serial, static_cast<unsigned long long>(releaseWaitUs),
                            static_cast<unsigned long long>(throttled_));
            }
            return 1;
        }
        if (fenceResult != VK_SUCCESS)
            return FailLocked("source release fence", fenceResult, serial);
        frame.submitted = false;
        targetInitialized_[imageIndex] = true;
        TracePresentStage("source-release-ready", serial, image);

        uint64_t gpuPresentCopyUs = 0;
        if (gpuTiming) {
            uint64_t ticks[2] = {};
            const VkResult timingResult = vkGetQueryPoolResults(
                device_, frame.gpuTiming, 0, 2, sizeof(ticks), ticks,
                sizeof(ticks[0]), VK_QUERY_RESULT_64_BIT);
            if (timingResult == VK_SUCCESS && ticks[1] >= ticks[0]) {
                gpuPresentCopyUs = static_cast<uint64_t>(
                    (static_cast<double>(ticks[1] - ticks[0]) * timestampPeriodNs_) /
                    1000.0);
                totalGpuPresentWorkUs_ += gpuPresentCopyUs;
                maxGpuPresentWorkUs_ = std::max(maxGpuPresentWorkUs_, gpuPresentCopyUs);
                ++gpuTimingSamples_;
            } else {
                ++gpuTimingFailures_;
                if (gpuTimingFailures_ == 1 || !(gpuTimingFailures_ % 120)) {
                    OH_LOG_WARN(LOG_APP,
                                "[VENUS-GPU-TIME][NCP] query failed result=%{public}d "
                                "serial=%{public}u failures=%{public}llu",
                                static_cast<int32_t>(timingResult), serial,
                                static_cast<unsigned long long>(gpuTimingFailures_));
                }
            }
        }

        if (IsRecoverableWsiTargetLoss(result)) {
            swapchainDirty_ = true;
            return -EAGAIN;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return FailLocked("queue present", result, serial);

        lastPresentNs_ = timestamp;
        ++framesPresented_;
        if (guestDeadlinePacing) ++guestDeadlineFrames_;
        if (lastSerial_ && serial <= lastSerial_) ++serialRegressions_;
        lastSerial_ = serial;
        const uint64_t frameEndNs = NowNs();
        const uint64_t presentUs = (frameEndNs - presentStartNs) / 1000;
        if (!firstPresentedNs_) firstPresentedNs_ = frameEndNs;
        totalPresentUs_ += presentUs;
        maxPresentUs_ = std::max(maxPresentUs_, presentUs);
        totalWaitFenceUs_ += waitFenceUs;
        totalAcquireUs_ += acquireUs;
        totalSubmitUs_ += submitUs;
        totalQueuePresentUs_ += queuePresentUs;
        totalReleaseWaitUs_ += releaseWaitUs;
        if (GpuFrameProfileSample(serial)) {
            OH_LOG_INFO(LOG_APP,
                        "[VENUS-FRAME-TIMELINE][NCP] serial=%{public}u "
                        "release_wait_us=%{public}llu present_cpu_us=%{public}llu "
                        "wait_fence_us=%{public}llu acquire_us=%{public}llu "
                        "submit_us=%{public}llu queue_present_us=%{public}llu "
                        "gpu_present_copy_us=%{public}llu",
                        serial,
                        static_cast<unsigned long long>(releaseWaitUs),
                        static_cast<unsigned long long>(presentUs),
                        static_cast<unsigned long long>(waitFenceUs),
                        static_cast<unsigned long long>(acquireUs),
                        static_cast<unsigned long long>(submitUs),
                        static_cast<unsigned long long>(queuePresentUs),
                        static_cast<unsigned long long>(gpuPresentCopyUs));
        }
        if (nextPresentDeadlineNs)
            *nextPresentDeadlineNs =
                NextPresentDeadlineNs(lastPresentNs_, framePeriodNs_);
        if (TraceFrameOrder() && framesPresented_ <= 600) {
            OH_LOG_INFO(LOG_APP,
                        "[VENUS-ORDER][NCP] frame=%{public}llu serial=%{public}u "
                        "serial_regress=%{public}llu source=0x%{public}llx "
                        "target_index=%{public}u target=0x%{public}llx "
                        "timestamp=%{public}llu",
                        static_cast<unsigned long long>(framesPresented_), serial,
                        static_cast<unsigned long long>(serialRegressions_),
                        static_cast<unsigned long long>(image), imageIndex,
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(
                            swapchainImages_[imageIndex])),
                        static_cast<unsigned long long>(timestamp));
        }
        TracePresentStage("published", serial, image);
        if (policy_.perfSummary &&
            (framesPresented_ == 1 || !(framesPresented_ % 120))) {
            const uint64_t elapsedNs = frameEndNs - firstPresentedNs_;
            const uint64_t fpsX100 = elapsedNs && framesPresented_ > 1
                ? ((framesPresented_ - 1) * 100ULL * 1000000000ULL) / elapsedNs
                : 0;
            OH_LOG_INFO(LOG_APP,
                        "[VENUS-PRESENT][NCP] transport=wsi "
                        "frames=%{public}llu ctx=%{public}u "
                        "key=%{public}llu serial=%{public}u size=%{public}ux%{public}u "
                        "target=%{public}ux%{public}u format=%{public}u "
                        "fps=%{public}llu.%{public}02llu gpu_copy=1 "
                        "present_us_avg=%{public}llu max=%{public}llu "
                        "wait_fence_avg=%{public}llu acquire_avg=%{public}llu "
                        "submit_avg=%{public}llu queue_present_avg=%{public}llu "
                        "release_wait_avg=%{public}llu "
                        "gpu_present_copy_avg=%{public}llu max=%{public}llu samples=%{public}llu "
                        "release_minus_present_gpu_avg=%{public}llu "
                        "release_mode=wait post_present_cpu_wait=1 "
                        "failures=%{public}llu "
                        "throttled=%{public}llu clock_deferred=%{public}llu "
                        "acquire_deferred=%{public}llu fence_deferred=%{public}llu "
                        "guest_deadline_frames=%{public}llu",
                        static_cast<unsigned long long>(framesPresented_), contextId,
                        static_cast<unsigned long long>(surfaceKey_), serial,
                        width, height, extent_.width, extent_.height, format,
                        static_cast<unsigned long long>(fpsX100 / 100),
                        static_cast<unsigned long long>(fpsX100 % 100),
                        static_cast<unsigned long long>(totalPresentUs_ / framesPresented_),
                        static_cast<unsigned long long>(maxPresentUs_),
                        static_cast<unsigned long long>(totalWaitFenceUs_ / framesPresented_),
                        static_cast<unsigned long long>(totalAcquireUs_ / framesPresented_),
                        static_cast<unsigned long long>(totalSubmitUs_ / framesPresented_),
                        static_cast<unsigned long long>(totalQueuePresentUs_ / framesPresented_),
                        static_cast<unsigned long long>(totalReleaseWaitUs_ / framesPresented_),
                        static_cast<unsigned long long>(gpuTimingSamples_
                            ? totalGpuPresentWorkUs_ / gpuTimingSamples_ : 0),
                        static_cast<unsigned long long>(maxGpuPresentWorkUs_),
                        static_cast<unsigned long long>(gpuTimingSamples_),
                        static_cast<unsigned long long>(
                            (totalReleaseWaitUs_ / framesPresented_) >
                                    (gpuTimingSamples_
                                        ? totalGpuPresentWorkUs_ / gpuTimingSamples_ : 0)
                                ? (totalReleaseWaitUs_ / framesPresented_) -
                                      (gpuTimingSamples_
                                          ? totalGpuPresentWorkUs_ / gpuTimingSamples_ : 0)
                                : 0),
                        static_cast<unsigned long long>(failures_),
                        static_cast<unsigned long long>(throttled_),
                        static_cast<unsigned long long>(clockDeferred_),
                        static_cast<unsigned long long>(acquireDeferred_),
                        static_cast<unsigned long long>(fenceDeferred_),
                        static_cast<unsigned long long>(guestDeadlineFrames_));
        }
        return 0;
    }

    ~Impl()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (device_) {
            OH_LOG_WARN(LOG_APP,
                        "[VENUS-PRESENT][NCP] abandoning presenter objects without "
                        "device-owner callback key=%{public}llu ctx=%{public}u device=%{public}p",
                        static_cast<unsigned long long>(surfaceKey_), contextId_, device_);
            vkDirect_.Abandon();
            ClearVulkanStateLocked();
        }
        ReleaseWindowLocked();
    }

private:
    int FailLocked(const char* operation, VkResult result, uint32_t serial)
    {
        ++failures_;
        if (failures_ == 1 || !(failures_ % 60)) {
            OH_LOG_ERROR(LOG_APP,
                         "[VENUS-PRESENT][NCP] %{public}s failed result=%{public}d "
                         "serial=%{public}u failures=%{public}llu",
                         operation, static_cast<int32_t>(result), serial,
                         static_cast<unsigned long long>(failures_));
        }
        return result == VK_ERROR_DEVICE_LOST ? -ENODEV : -EIO;
    }

    VkResult RecordPresentCopyLocked(Frame& frame,
                                     VkImage sourceImage,
                                     VkImageLayout sourceLayout,
                                     VkImage targetImage,
                                     VkFormat targetFormat,
                                     uint32_t targetWidth,
                                     uint32_t targetHeight,
                                     VkImageLayout targetOldLayout,
                                     VkImageLayout targetFinalLayout,
                                     bool gpuTiming)
    {
        if (!sourceImage || !targetImage || targetFormat == VK_FORMAT_UNDEFINED ||
            !sourceWidth_ || !sourceHeight_ || !targetWidth || !targetHeight) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const bool useBlit = sourceFormat_ != targetFormat ||
            sourceWidth_ != targetWidth || sourceHeight_ != targetHeight;
        if (useBlit) {
            VkFormatProperties sourceProperties{};
            VkFormatProperties targetProperties{};
            vkGetPhysicalDeviceFormatProperties(
                physicalDevice_, sourceFormat_, &sourceProperties);
            vkGetPhysicalDeviceFormatProperties(
                physicalDevice_, targetFormat, &targetProperties);
            if (!(sourceProperties.optimalTilingFeatures &
                  VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
                !(targetProperties.optimalTilingFeatures &
                  VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
        }

        VkResult result = vkResetCommandBuffer(frame.command, 0);
        if (result != VK_SUCCESS) return result;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(frame.command, &begin);
        if (result != VK_SUCCESS) return result;

        if (gpuTiming) {
            vkCmdResetQueryPool(frame.command, frame.gpuTiming, 0, 2);
            vkCmdWriteTimestamp(frame.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                frame.gpuTiming, 0);
        }

        VkImageMemoryBarrier sourceToTransfer{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        sourceToTransfer.srcAccessMask = SourceAccess(sourceLayout);
        sourceToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceToTransfer.oldLayout = sourceLayout;
        sourceToTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        sourceToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceToTransfer.image = sourceImage;
        sourceToTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        sourceToTransfer.subresourceRange.levelCount = 1;
        sourceToTransfer.subresourceRange.layerCount = 1;

        VkImageMemoryBarrier targetToTransfer{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        targetToTransfer.srcAccessMask =
            targetOldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT;
        targetToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        targetToTransfer.oldLayout = targetOldLayout;
        targetToTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        targetToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        targetToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        targetToTransfer.image = targetImage;
        targetToTransfer.subresourceRange = sourceToTransfer.subresourceRange;
        const std::array<VkImageMemoryBarrier, 2> before = {
            sourceToTransfer, targetToTransfer};
        const VkPipelineStageFlags targetSourceStage =
            targetOldLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        vkCmdPipelineBarrier(
            frame.command, SourceStage(sourceLayout) | targetSourceStage,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
            static_cast<uint32_t>(before.size()), before.data());

        if (useBlit) {
            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.layerCount = 1;
            blit.srcOffsets[1] = {
                static_cast<int32_t>(sourceWidth_),
                static_cast<int32_t>(sourceHeight_), 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.layerCount = 1;
            blit.dstOffsets[1] = {
                static_cast<int32_t>(targetWidth),
                static_cast<int32_t>(targetHeight), 1};
            vkCmdBlitImage(
                frame.command, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                targetImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                VK_FILTER_NEAREST);
        } else {
            VkImageCopy copy{};
            copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.srcSubresource.layerCount = 1;
            copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.dstSubresource.layerCount = 1;
            copy.extent = {sourceWidth_, sourceHeight_, 1};
            vkCmdCopyImage(
                frame.command, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                targetImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }

        VkImageMemoryBarrier sourceRestore{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        sourceRestore.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceRestore.dstAccessMask = SourceAccess(sourceLayout);
        sourceRestore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        sourceRestore.newLayout = sourceLayout;
        sourceRestore.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceRestore.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceRestore.image = sourceImage;
        sourceRestore.subresourceRange = sourceToTransfer.subresourceRange;

        VkImageMemoryBarrier targetPublish{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        targetPublish.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        targetPublish.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        targetPublish.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        targetPublish.newLayout = targetFinalLayout;
        targetPublish.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        targetPublish.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        targetPublish.image = targetImage;
        targetPublish.subresourceRange = targetToTransfer.subresourceRange;
        const std::array<VkImageMemoryBarrier, 2> after = {
            sourceRestore, targetPublish};
        vkCmdPipelineBarrier(
            frame.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
            static_cast<uint32_t>(after.size()), after.data());

        if (gpuTiming) {
            vkCmdWriteTimestamp(frame.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                frame.gpuTiming, 1);
        }
        result = vkEndCommandBuffer(frame.command);
        if (result != VK_SUCCESS) return result;
        return vkResetFences(device_, 1, &frame.complete);
    }

    VkResult WaitForOutstandingFramesLocked()
    {
        std::vector<VkFence> pending;
        pending.reserve(frames_.size());
        for (const Frame& frame : frames_) {
            if (frame.submitted && frame.complete)
                pending.push_back(frame.complete);
        }
        if (pending.empty()) return VK_SUCCESS;

        const VkResult result = vkWaitForFences(
            device_, static_cast<uint32_t>(pending.size()), pending.data(),
            VK_TRUE, kReleaseFenceWatchdogNs);
        if (result == VK_SUCCESS) {
            for (Frame& frame : frames_) frame.submitted = false;
        }
        return result;
    }

    bool CreateDirectResourcesLocked(int& error)
    {
        extent_ = {sourceWidth_, sourceHeight_};
        targetFormat_ = sourceFormat_;
        canBlit_ = true;
        useBlit_ = false;

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily_;
        VkResult result = vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);
        if (result != VK_SUCCESS) {
            error = -EIO;
            return false;
        }

        frames_.resize(3);
        std::vector<VkCommandBuffer> commands(frames_.size());
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = commandPool_;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = static_cast<uint32_t>(commands.size());
        result = vkAllocateCommandBuffers(device_, &allocate, commands.data());
        if (result != VK_SUCCESS) {
            error = -ENOMEM;
            return false;
        }

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (size_t i = 0; i < frames_.size(); ++i) {
            Frame& frame = frames_[i];
            frame.command = commands[i];
            if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr,
                                  &frame.acquired) != VK_SUCCESS ||
                vkCreateSemaphore(device_, &semaphoreInfo, nullptr,
                                  &frame.released) != VK_SUCCESS ||
                vkCreateFence(device_, &fenceInfo, nullptr,
                              &frame.complete) != VK_SUCCESS) {
                error = -ENOMEM;
                return false;
            }
        }

        // DP1 measures CPU queue/request/flush cost. Timestamp queries remain
        // on the WSI comparison path until delayed query collection is added;
        // Direct must not reintroduce a post-present CPU fence wait for metrics.
        gpuTimingRequested_ = GpuFrameProfileEnabled();
        gpuTimingEnabled_ = false;
        directReady_ = true;
        OH_LOG_INFO(LOG_APP,
                    "[VENUS-PRESENT][NCP] transport=direct-native-buffer "
                    "key=%{public}llu size=%{public}ux%{public}u frames=%{public}zu "
                    "post_present_cpu_wait=0",
                    static_cast<unsigned long long>(surfaceKey_), sourceWidth_,
                    sourceHeight_, frames_.size());
        return true;
    }

    bool EnsureVulkanLocked(uint32_t contextId,
                            VkInstance instance,
                            VkPhysicalDevice physicalDevice,
                            VkDevice device,
                            VkQueue queue,
                            uint32_t queueFamily,
                            uint32_t width,
                            uint32_t height,
                            VkFormat sourceFormat,
                            int& error)
    {
        const bool sameSource = contextId_ == contextId &&
            instance_ == instance && physicalDevice_ == physicalDevice &&
            device_ == device && queue_ == queue && queueFamily_ == queueFamily &&
            sourceWidth_ == width && sourceHeight_ == height &&
            sourceFormat_ == sourceFormat;
        if (transport_ == VulkanPresentTransport::DirectNativeBuffer &&
            directReady_ && sameSource) {
            return true;
        }
        if (transport_ == VulkanPresentTransport::Wsi && swapchain_ &&
            sameSource && !swapchainDirty_) {
            return true;
        }

        if ((transport_ == VulkanPresentTransport::DirectNativeBuffer ||
             transport_ == VulkanPresentTransport::DirectFallbackPending) &&
            device_ && queue_) {
            const VkResult idleResult = vkQueueWaitIdle(queue_);
            if (idleResult != VK_SUCCESS) {
                error = idleResult == VK_ERROR_DEVICE_LOST ? -ENODEV : -EIO;
                return false;
            }
        }

        if (transport_ == VulkanPresentTransport::Wsi && device_) {
            const VkResult waitResult = WaitForOutstandingFramesLocked();
            if (waitResult != VK_SUCCESS) {
                error = waitResult == VK_TIMEOUT ? -EAGAIN
                    : waitResult == VK_ERROR_DEVICE_LOST ? -ENODEV : -EIO;
                return false;
            }
        }

        /* A WSI error can leave the platform present queue waiting forever.
         * The dirty path has already waited for the per-frame fence and has
         * no newly acquired image, so it must not perform an unbounded
         * vkQueueWaitIdle during recovery. */
        DestroyVulkanLocked();
        contextId_ = contextId;
        instance_ = instance;
        physicalDevice_ = physicalDevice;
        device_ = device;
        queue_ = queue;
        queueFamily_ = queueFamily;
        sourceWidth_ = width;
        sourceHeight_ = height;
        sourceFormat_ = sourceFormat;
        swapchainDirty_ = false;

        if (!instance_ || !physicalDevice_ || !device_ || !queue_) {
            error = -EINVAL;
            return false;
        }

        if (transport_ == VulkanPresentTransport::DirectFallbackPending) {
            transport_ = VulkanPresentTransport::Wsi;
            OH_LOG_WARN(LOG_APP,
                        "[VENUS-PRESENT][NCP] transport fallback latched "
                        "key=%{public}llu reason=%{public}s selected=wsi",
                        static_cast<unsigned long long>(surfaceKey_), transportReason_);
        }

        if (transport_ == VulkanPresentTransport::Unprobed) {
            const NativeWindowVkConfigureResult directResult = vkDirect_.Configure(
                surfaceKey_, windowLease_.Get(), width, height, physicalDevice_,
                device_, sourceFormat_);
            if (directResult == NativeWindowVkConfigureResult::Ready) {
                transport_ = VulkanPresentTransport::DirectNativeBuffer;
                transportReason_ = "capability-ready";
            } else {
                transport_ = VulkanPresentTransport::Wsi;
                transportReason_ = NativeWindowVkConfigureResultName(directResult);
                OH_LOG_WARN(LOG_APP,
                            "[VENUS-PRESENT][NCP] transport fallback latched "
                            "key=%{public}llu reason=%{public}s selected=wsi",
                            static_cast<unsigned long long>(surfaceKey_),
                            transportReason_);
            }
        }

        if (transport_ == VulkanPresentTransport::DirectNativeBuffer) {
            if (!vkDirect_.Ready()) {
                const NativeWindowVkConfigureResult directResult = vkDirect_.Configure(
                    surfaceKey_, windowLease_.Get(), width, height,
                    physicalDevice_, device_, sourceFormat_);
                if (directResult != NativeWindowVkConfigureResult::Ready) {
                    transport_ = VulkanPresentTransport::Wsi;
                    transportReason_ = NativeWindowVkConfigureResultName(directResult);
                    OH_LOG_WARN(LOG_APP,
                                "[VENUS-PRESENT][NCP] transport fallback latched "
                                "key=%{public}llu reason=%{public}s selected=wsi",
                                static_cast<unsigned long long>(surfaceKey_),
                                transportReason_);
                }
            }
            if (transport_ == VulkanPresentTransport::DirectNativeBuffer) {
                if (CreateDirectResourcesLocked(error)) return true;
                transport_ = VulkanPresentTransport::DirectFallbackPending;
                transportReason_ = "direct-resource-create-failed";
                return false;
            }
        }

        OH_NativeWindow_NativeWindowHandleOpt(
            windowLease_.Get(), SET_BUFFER_GEOMETRY,
            static_cast<int32_t>(width), static_cast<int32_t>(height));
        OH_NativeWindow_NativeWindowHandleOpt(
            windowLease_.Get(), SET_USAGE,
            static_cast<uint64_t>(NATIVEBUFFER_USAGE_HW_RENDER |
                                  NATIVEBUFFER_USAGE_HW_TEXTURE));
        OH_NativeWindow_NativeWindowHandleOpt(windowLease_.Get(), SET_TIMEOUT, 0);

        VkSurfaceCreateInfoOHOS surfaceInfo{VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS};
        surfaceInfo.window = windowLease_.Get();
        VkResult result = vkCreateSurfaceOHOS(instance_, &surfaceInfo, nullptr, &surface_);
        if (result != VK_SUCCESS) {
            error = -ENOTSUP;
            return false;
        }

        VkBool32 presentSupported = VK_FALSE;
        result = vkGetPhysicalDeviceSurfaceSupportKHR(
            physicalDevice_, queueFamily_, surface_, &presentSupported);
        if (result != VK_SUCCESS || !presentSupported) {
            error = -ENOTSUP;
            return false;
        }

        VkSurfaceCapabilitiesKHR capabilities{};
        result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            physicalDevice_, surface_, &capabilities);
        if (result != VK_SUCCESS ||
            !(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            error = -ENOTSUP;
            return false;
        }

        uint32_t formatCount = 0;
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice_, surface_, &formatCount, nullptr);
        if (result != VK_SUCCESS || !formatCount) {
            error = -ENOTSUP;
            return false;
        }
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice_, surface_, &formatCount, formats.data());
        if (result != VK_SUCCESS) {
            error = -EIO;
            return false;
        }
        VkSurfaceFormatKHR chosen = formats.front();
        for (const auto& candidate : formats) {
            if (candidate.format == sourceFormat_) {
                chosen = candidate;
                break;
            }
            if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM ||
                candidate.format == VK_FORMAT_R8G8B8A8_UNORM)
                chosen = candidate;
        }
        targetFormat_ = chosen.format;

        extent_ = capabilities.currentExtent;
        if (extent_.width == UINT32_MAX) {
            extent_.width = std::clamp(width, capabilities.minImageExtent.width,
                                      capabilities.maxImageExtent.width);
            extent_.height = std::clamp(height, capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height);
        }
        uint32_t imageCount = std::max(2u, capabilities.minImageCount);
        if (capabilities.maxImageCount)
            imageCount = std::min(imageCount, capabilities.maxImageCount);

        VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        create.surface = surface_;
        create.minImageCount = imageCount;
        create.imageFormat = chosen.format;
        create.imageColorSpace = chosen.colorSpace;
        create.imageExtent = extent_;
        create.imageArrayLayers = 1;
        create.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.preTransform = capabilities.currentTransform;
        create.compositeAlpha = ChooseCompositeAlpha(capabilities.supportedCompositeAlpha);
        // FIFO is guaranteed by Vulkan WSI and is the single product queue
        // discipline for both VirGL and Vulkan routes.
        create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        create.clipped = VK_TRUE;
        result = vkCreateSwapchainKHR(device_, &create, nullptr, &swapchain_);
        if (result != VK_SUCCESS) {
            error = -ENOTSUP;
            return false;
        }

        result = vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
        if (result != VK_SUCCESS || !imageCount) {
            error = -EIO;
            return false;
        }
        swapchainImages_.resize(imageCount);
        result = vkGetSwapchainImagesKHR(
            device_, swapchain_, &imageCount, swapchainImages_.data());
        if (result != VK_SUCCESS) {
            error = -EIO;
            return false;
        }
        targetInitialized_.assign(imageCount, false);
        renderFinished_.resize(imageCount, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (VkSemaphore& semaphore : renderFinished_) {
            if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) {
                error = -ENOMEM;
                return false;
            }
        }

        VkFormatProperties sourceProperties{};
        VkFormatProperties targetProperties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, sourceFormat_, &sourceProperties);
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, targetFormat_, &targetProperties);
        canBlit_ =
            (sourceProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
            (targetProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
        useBlit_ = sourceFormat_ != targetFormat_ || width != extent_.width ||
            height != extent_.height;
        if (useBlit_ && !canBlit_) {
            error = -ENOTSUP;
            return false;
        }

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily_;
        result = vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);
        if (result != VK_SUCCESS) {
            error = -EIO;
            return false;
        }
        frames_.resize(std::min<size_t>(3, swapchainImages_.size()));
        std::vector<VkCommandBuffer> commands(frames_.size());
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = commandPool_;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = static_cast<uint32_t>(commands.size());
        result = vkAllocateCommandBuffers(device_, &allocate, commands.data());
        if (result != VK_SUCCESS) {
            error = -ENOMEM;
            return false;
        }
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        gpuTimingRequested_ = GpuFrameProfileEnabled();
        gpuTimingEnabled_ = false;
        timestampPeriodNs_ = 0.0f;
        if (gpuTimingRequested_) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
            uint32_t queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount,
                                                     nullptr);
            std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            if (queueFamilyCount) {
                vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount,
                                                         queueFamilies.data());
            }
            const bool queryResetSupported =
                VK_API_VERSION_MAJOR(properties.apiVersion) > 1 ||
                (VK_API_VERSION_MAJOR(properties.apiVersion) == 1 &&
                 VK_API_VERSION_MINOR(properties.apiVersion) >= 2);
            if (queryResetSupported &&
                properties.limits.timestampComputeAndGraphics &&
                queueFamily_ < queueFamilyCount &&
                queueFamilies[queueFamily_].timestampValidBits &&
                properties.limits.timestampPeriod > 0.0f) {
                timestampPeriodNs_ = properties.limits.timestampPeriod;
                gpuTimingEnabled_ = true;
            } else {
                OH_LOG_WARN(LOG_APP,
                            "[VENUS-GPU-TIME][NCP] timestamp queries unsupported "
                            "api=%{public}u compute_graphics=%{public}u "
                            "queue_bits=%{public}u period_ps=%{public}llu",
                            properties.apiVersion,
                            properties.limits.timestampComputeAndGraphics,
                            queueFamily_ < queueFamilyCount
                                ? queueFamilies[queueFamily_].timestampValidBits : 0,
                            static_cast<unsigned long long>(
                                properties.limits.timestampPeriod * 1000.0f));
            }
        }
        for (size_t i = 0; i < frames_.size(); ++i) {
            frames_[i].command = commands[i];
            if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr,
                                  &frames_[i].acquired) != VK_SUCCESS ||
                vkCreateFence(device_, &fenceInfo, nullptr,
                              &frames_[i].complete) != VK_SUCCESS) {
                error = -ENOMEM;
                return false;
            }
            if (gpuTimingEnabled_) {
                VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
                queryInfo.queryCount = 2;
                if (vkCreateQueryPool(device_, &queryInfo, nullptr,
                                      &frames_[i].gpuTiming) != VK_SUCCESS) {
                    OH_LOG_WARN(LOG_APP,
                                "[VENUS-GPU-TIME][NCP] timestamp query pool unavailable; "
                                "continuing without GPU timing");
                    gpuTimingEnabled_ = false;
                    break;
                }
            }
        }

        OH_LOG_INFO(LOG_APP,
                    "[VENUS-PRESENT][NCP] swapchain ready key=%{public}llu "
                    "source=%{public}ux%{public}u target=%{public}ux%{public}u "
                    "source_format=%{public}u target_format=%{public}u images=%{public}u "
                    "blit_supported=%{public}d transfer=%{public}s "
                    "queue_family=%{public}u present_mode=fifo release_mode=wait "
                    "gpu_timing=%{public}s timestamp_period_ps=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey_),
                    width, height, extent_.width, extent_.height,
                    static_cast<uint32_t>(sourceFormat_),
                    static_cast<uint32_t>(targetFormat_), imageCount,
                    canBlit_, useBlit_ ? "blit" : "copy", queueFamily_,
                    gpuTimingEnabled_ ? "enabled" : "off",
                    static_cast<unsigned long long>(timestampPeriodNs_ * 1000.0f));
        return true;
    }

    bool MatchesDeviceLocked(uint32_t contextId, uintptr_t device) const
    {
        return contextId_ == contextId && device_ != VK_NULL_HANDLE &&
            reinterpret_cast<uintptr_t>(device_) == device;
    }

    void DestroyVulkanLocked()
    {
        if (device_) {
            for (auto& frame : frames_) {
                if (frame.gpuTiming) vkDestroyQueryPool(device_, frame.gpuTiming, nullptr);
                if (frame.complete) vkDestroyFence(device_, frame.complete, nullptr);
                if (frame.acquired) vkDestroySemaphore(device_, frame.acquired, nullptr);
                if (frame.released) vkDestroySemaphore(device_, frame.released, nullptr);
            }
            for (const VkSemaphore semaphore : renderFinished_) {
                if (semaphore) vkDestroySemaphore(device_, semaphore, nullptr);
            }
            if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
            vkDirect_.Reset();
            if (swapchain_) {
                OH_LOG_INFO(LOG_APP, "[VENUS-PRESENT][NCP] destroy swapchain begin key=%{public}llu",
                            static_cast<unsigned long long>(surfaceKey_));
                vkDestroySwapchainKHR(device_, swapchain_, nullptr);
                OH_LOG_INFO(LOG_APP, "[VENUS-PRESENT][NCP] destroy swapchain end key=%{public}llu",
                            static_cast<unsigned long long>(surfaceKey_));
            }
        }
        if (instance_ && surface_) {
            OH_LOG_INFO(LOG_APP, "[VENUS-PRESENT][NCP] destroy surface begin key=%{public}llu",
                        static_cast<unsigned long long>(surfaceKey_));
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            OH_LOG_INFO(LOG_APP, "[VENUS-PRESENT][NCP] destroy surface end key=%{public}llu",
                        static_cast<unsigned long long>(surfaceKey_));
        }
        ClearVulkanStateLocked();
    }

    void ClearVulkanStateLocked()
    {
        frames_.clear();
        renderFinished_.clear();
        targetInitialized_.clear();
        swapchainImages_.clear();
        commandPool_ = VK_NULL_HANDLE;
        swapchain_ = VK_NULL_HANDLE;
        surface_ = VK_NULL_HANDLE;
        contextId_ = 0;
        instance_ = VK_NULL_HANDLE;
        physicalDevice_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        queue_ = VK_NULL_HANDLE;
        queueFamily_ = 0;
        sourceWidth_ = 0;
        sourceHeight_ = 0;
        sourceFormat_ = VK_FORMAT_UNDEFINED;
        targetFormat_ = VK_FORMAT_UNDEFINED;
        extent_ = {};
        frameIndex_ = 0;
        canBlit_ = false;
        useBlit_ = false;
        gpuTimingRequested_ = false;
        gpuTimingEnabled_ = false;
        timestampPeriodNs_ = 0.0f;
        swapchainDirty_ = false;
        directReady_ = false;
    }

    void ReleaseWindowLocked()
    {
        if (!windowLease_) return;
        const bool unreference = windowLease_.UsesNativeObjectReference();
        if (unreference) {
            OH_LOG_INFO(LOG_APP,
                        "[VENUS-PRESENT][NCP] detach window-unreference begin key=%{public}llu",
                        static_cast<unsigned long long>(surfaceKey_));
            const int32_t result = windowLease_.Reset();
            OH_LOG_INFO(LOG_APP,
                        "[VENUS-PRESENT][NCP] detach window-unreference end key=%{public}llu result=%{public}d",
                        static_cast<unsigned long long>(surfaceKey_), result);
        } else {
            OH_LOG_INFO(LOG_APP, "[VENUS-PRESENT][NCP] detach window-destroy begin key=%{public}llu",
                        static_cast<unsigned long long>(surfaceKey_));
            windowLease_.Reset();
            OH_LOG_INFO(LOG_APP, "[VENUS-PRESENT][NCP] detach window-destroy end key=%{public}llu",
                        static_cast<unsigned long long>(surfaceKey_));
        }
    }

    std::mutex mutex_;
    NativeWindowLease windowLease_;
    NativeWindowVkTarget vkDirect_;
    uint64_t surfaceKey_ = 0;
    uint32_t contextId_ = 0;
    bool surfaceAttached_ = false;
    bool deviceReleasing_ = false;
    PresenterRuntimePolicy policy_;
    VulkanPresentTransport transport_ = VulkanPresentTransport::Unprobed;
    const char* transportReason_ = "not-probed";
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages_;
    std::vector<bool> targetInitialized_;
    std::vector<VkSemaphore> renderFinished_;
    std::vector<Frame> frames_;
    size_t frameIndex_ = 0;
    VkExtent2D extent_{};
    VkFormat sourceFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat targetFormat_ = VK_FORMAT_UNDEFINED;
    uint32_t sourceWidth_ = 0;
    uint32_t sourceHeight_ = 0;
    bool canBlit_ = false;
    bool useBlit_ = false;
    bool directReady_ = false;
    bool swapchainDirty_ = false;
    uint64_t displayPeriodNs_ = kDefaultPresentFramePeriodNs;
    uint64_t framePeriodNs_ = kDefaultPresentFramePeriodNs;
    uint64_t lastPresentNs_ = 0;
    uint64_t framesPresented_ = 0;
    bool guestDeadlinePacingLogged_ = false;
    uint32_t lastSerial_ = 0;
    uint64_t serialRegressions_ = 0;
    uint64_t failures_ = 0;
    uint64_t throttled_ = 0;
    uint64_t clockDeferred_ = 0;
    uint64_t acquireDeferred_ = 0;
    uint64_t fenceDeferred_ = 0;
    uint64_t guestDeadlineFrames_ = 0;
    uint64_t firstPresentedNs_ = 0;
    uint64_t totalPresentUs_ = 0;
    uint64_t maxPresentUs_ = 0;
    uint64_t totalWaitFenceUs_ = 0;
    uint64_t totalAcquireUs_ = 0;
    uint64_t totalSubmitUs_ = 0;
    uint64_t totalQueuePresentUs_ = 0;
    uint64_t totalReleaseWaitUs_ = 0;
    uint64_t totalGpuPresentWorkUs_ = 0;
    uint64_t maxGpuPresentWorkUs_ = 0;
    uint64_t gpuTimingSamples_ = 0;
    uint64_t gpuTimingFailures_ = 0;
    bool gpuTimingRequested_ = false;
    bool gpuTimingEnabled_ = false;
    float timestampPeriodNs_ = 0.0f;
};

VenusSurfaceQueueTarget::VenusSurfaceQueueTarget()
    : impl_(std::make_unique<Impl>())
{
}

VenusSurfaceQueueTarget::~VenusSurfaceQueueTarget() = default;

int VenusSurfaceQueueTarget::Attach(uint64_t surfaceKey, uint64_t framePeriodNs,
                                    OHNativeWindow* window,
                                    bool releaseWindowWithUnreference)
{
    return impl_->Attach(surfaceKey, framePeriodNs, window, releaseWindowWithUnreference);
}

int VenusSurfaceQueueTarget::Detach(uint64_t surfaceKey)
{
    return impl_->Detach(surfaceKey);
}

int VenusSurfaceQueueTarget::SetFramePeriod(uint64_t framePeriodNs)
{
    return impl_->SetFramePeriod(framePeriodNs);
}

bool VenusSurfaceQueueTarget::HasVulkanDevice()
{
    return impl_->HasVulkanDevice();
}

bool VenusSurfaceQueueTarget::PrepareDeviceRelease(uint32_t contextId,
                                                   uintptr_t device)
{
    return impl_->PrepareDeviceRelease(contextId, device);
}

bool VenusSurfaceQueueTarget::FinishDeviceRelease(uint32_t contextId,
                                                  uintptr_t device,
                                                  int32_t waitResult)
{
    return impl_->FinishDeviceRelease(contextId, device, waitResult);
}

// Present (GL) 为防御性死路径: Manager 按 IsVulkan 调度, GL 帧不会送达
// venus 目标。返回 kPresentInvalid 以指示错误的呈现通道。
int VenusSurfaceQueueTarget::Present(GLuint /*texture*/, uint32_t /*width*/,
                                     uint32_t /*height*/, uint64_t /*drawable*/,
                                     uint32_t /*serial*/,
                                     uint64_t* /*nextPresentDeadlineNs*/)
{
    return kPresentInvalid;
}

int VenusSurfaceQueueTarget::PresentVenus(uint32_t contextId,
                                          uintptr_t instance,
                                          uintptr_t physicalDevice,
                                          uintptr_t device,
                                          uintptr_t queue,
                                          uint64_t image,
                                          uint32_t queueFamily,
                                          uint32_t width,
                                          uint32_t height,
                                          uint32_t format,
                                          uint32_t layout,
                                          uint32_t serial,
                                          uint64_t* nextPresentDeadlineNs,
                                          void (*releaseQueue)(void*),
                                          void* queueSyncData)
{
    return impl_->Present(contextId, instance, physicalDevice, device, queue,
                          image, queueFamily, width, height, format, layout,
                          serial, nextPresentDeadlineNs, releaseQueue,
                          queueSyncData);
}

} // namespace winehua
