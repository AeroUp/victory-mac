#pragma once

#include "xg_xs_layout.h"
#include "../src/vulkan_backend.h"

namespace rt::d3d12 {

// Ring-buffered staging upload buffers and command buffers per device, used under the title-texture registry lock.
// A whole image (including arrays and compressed mip tails) is detiled into the mapped UMA/host-visible buffer.
// Batched refreshes pack all changed textures into one command buffer and submit asynchronously on a 3-slot ring,
// avoiding per-frame vkDeviceWaitIdle / vkWaitSemaphores stalls while keeping GPU ordering via image barriers and
// timeline semaphores.
class TitleImageUpload {
public:
    static constexpr std::size_t kRing = 3;

    VkDeviceSize capacity() const {
        VkDeviceSize total = 0;
        for (const auto& s : slots_) total += s.capacity;
        return total;
    }

    template<class Device>
    void release(Device& d) {
        auto& vk = d.context->vk;
        if (ready_ && value_ > 0) {
            VkSemaphoreWaitInfo done{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            done.semaphoreCount = 1;
            done.pSemaphores = &ready_;
            done.pValues = &value_;
            vk.vkWaitSemaphores(d.info.device, &done, UINT64_MAX);
        }
        for (auto& s : slots_) {
            release_slot_buffer(d, s);
            s.command = VK_NULL_HANDLE;
            s.pending_value = 0;
        }
        if (pool_) vk.vkDestroyCommandPool(d.info.device, pool_, nullptr);
        if (ready_) vk.vkDestroySemaphore(d.info.device, ready_, nullptr);
        pool_ = VK_NULL_HANDLE;
        ready_ = VK_NULL_HANDLE;
        batching_ = false;
        batch_bytes_ = 0;
        batch_items_.clear();
        batch_regions_.clear();
    }

    void begin_batch() {
        batching_ = true;
        batch_bytes_ = 0;
        batch_items_.clear();
        batch_regions_.clear();
    }

    template<class Device>
    bool flush_batch(Device& d) {
        if (batch_items_.empty()) return true;
        Slot& s = slots_[ring_index_];
        // On a single-queue device (e.g. MoltenVK), pipeline barriers on d.info.queue order the copy after any prior
        // reads without stalling the CPU on vkDeviceWaitIdle. Only drain when separate consumer queues exist.
        const bool need_idle = !consumers_.empty();
        const auto result = d.context->write_staged_title_batch(
            need_idle,
            [&](const leonida::ReplayFunctions& replay, auto&& get_pieces, auto&& mark_general) -> VkResult {
                auto& vk = d.context->vk;
                VkResult r = vk.vkResetCommandBuffer(s.command, 0);
                if (r != VK_SUCCESS) return r;
                VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                if ((r = vk.vkBeginCommandBuffer(s.command, &begin)) != VK_SUCCESS) return r;
                std::vector<leonida::LayoutPieces> all_pieces(batch_items_.size());
                for (std::size_t i = 0; i < batch_items_.size(); ++i) {
                    const auto& item = batch_items_[i];
                    if (!get_pieces(item.id, item.range, all_pieces[i])) continue;
                    leonida::detail::image_barrier(replay, s.command, item.image, item.range, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    vk.vkCmdCopyBufferToImage(s.command, s.buffer, item.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                              static_cast<std::uint32_t>(item.region_count),
                                              batch_regions_.data() + item.region_start);
                    for (const auto& piece : all_pieces[i])
                        leonida::detail::image_barrier(replay, s.command, item.image, piece.range,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            piece.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_GENERAL : piece.layout,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                }
                if ((r = vk.vkEndCommandBuffer(s.command)) != VK_SUCCESS) return r;
                r = submit_slot(d, s, /*wait_now=*/false);
                if (r == VK_SUCCESS) {
                    for (std::size_t i = 0; i < batch_items_.size(); ++i)
                        if (!all_pieces[i].empty()) mark_general(batch_items_[i].id, all_pieces[i]);
                }
                return r;
            });
        batch_items_.clear();
        batch_regions_.clear();
        batch_bytes_ = 0;
        if (result == VK_SUCCESS) ring_index_ = (ring_index_ + 1) % kRing;
        return result == VK_SUCCESS;
    }

    template<class Device>
    bool end_batch(Device& d) {
        const bool ok = flush_batch(d);
        batching_ = false;
        return ok;
    }

    template<class Device>
    bool write(Device& d, leonida::ResourceId id, const leonida::ResourceBinding& image,
               const xg_xs::Texture& layout, const std::uint8_t* tiled, std::vector<std::uint64_t>& columns,
               bool inspect, bool& nonzero) {
        const auto block = leonida::copy_block_format(image, VK_IMAGE_ASPECT_COLOR_BIT);
        const auto mips = layout.layout.mips;
        const auto layers = layout.array;
        if (!block.bytes || block.bytes != layout.format.bytes || !mips || mips > image.mip_levels ||
            !layers || layers > image.array_layers) return false;

        regions_.clear();
        VkDeviceSize bytes = 0;
        for (unsigned layer = 0; layer < layers; ++layer) {
            for (unsigned level = 0; level < mips; ++level) {
                const auto& mip = layout.layout.plane[0].mips[level];
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
                copy.imageExtent = {std::max(1u, image.width >> level), std::max(1u, image.height >> level),
                                    image.volume ? std::max(1u, image.depth >> level) : 1u};
                if (mip.width_elements != (copy.imageExtent.width + block.block - 1) / block.block ||
                    mip.height_elements != (copy.imageExtent.height + block.block - 1) / block.block ||
                    (image.volume && mip.depth != copy.imageExtent.depth)) return false;
                // Vulkan requires offsets aligned to four bytes and to the texel block size.
                const auto align = 4u * block.bytes;
                copy.bufferOffset = bytes + (align - bytes % align) % align;
                const VkDeviceSize row = VkDeviceSize(mip.width_elements) * block.bytes;
                const VkDeviceSize slice = row * mip.height_elements;
                if (slice > UINT32_MAX || (UINT64_MAX - copy.bufferOffset) / copy.imageExtent.depth < slice) return false;
                bytes = copy.bufferOffset + slice * copy.imageExtent.depth;
                regions_.push_back(copy);
            }
        }
        if (batching_) {
            const VkDeviceSize item_align = std::max<VkDeviceSize>(16, 4u * block.bytes);
            VkDeviceSize base_offset = batch_bytes_ + (item_align - batch_bytes_ % item_align) % item_align;
            Slot* s = &slots_[ring_index_];
            if (batch_items_.empty()) {
                // Before writing the first item of a batch into this ring slot, ensure its prior GPU copy finished.
                const VkDeviceSize target = std::max<VkDeviceSize>(bytes, VkDeviceSize(32) << 20);
                if (!prepare_slot(d, *s, target)) return false;
            } else if (base_offset + bytes > s->capacity) {
                if (!flush_batch(d)) return false;
                s = &slots_[ring_index_];
                base_offset = 0;
                const VkDeviceSize target = std::max<VkDeviceSize>(bytes, VkDeviceSize(32) << 20);
                if (!prepare_slot(d, *s, target)) return false;
            }
            const std::size_t region_start = batch_regions_.size();
            for (auto copy : regions_) {
                copy.bufferOffset += base_offset;
                const auto level = copy.imageSubresource.mipLevel;
                const auto& mip = layout.layout.plane[0].mips[level];
                const auto row = mip.width_elements * block.bytes, slice = row * mip.height_elements;
                auto* dst = s->mapped + copy.bufferOffset;
                xg_xs::copy_elements<false>(layout, level, copy.imageSubresource.baseArrayLayer, tiled, dst, row, slice, columns);
                if (inspect && !nonzero)
                    nonzero = std::any_of(dst, dst + VkDeviceSize(slice) * copy.imageExtent.depth,
                                          [](std::uint8_t c) { return c != 0; });
                batch_regions_.push_back(copy);
            }
            batch_items_.push_back({id, image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers},
                                    region_start, regions_.size()});
            batch_bytes_ = base_offset + bytes;
            return true;
        }
        Slot& s = slots_[ring_index_];
        if (!prepare_slot(d, s, bytes)) return false;
        for (const auto& copy : regions_) {
            const auto level = copy.imageSubresource.mipLevel;
            const auto& mip = layout.layout.plane[0].mips[level];
            const auto row = mip.width_elements * block.bytes, slice = row * mip.height_elements;
            auto* dst = s.mapped + copy.bufferOffset;
            xg_xs::copy_elements<false>(layout, level, copy.imageSubresource.baseArrayLayer, tiled, dst, row, slice, columns);
            if (inspect && !nonzero)
                nonzero = std::any_of(dst, dst + VkDeviceSize(slice) * copy.imageExtent.depth,
                                      [](std::uint8_t c) { return c != 0; });
        }

        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
        const auto result = d.context->write_staged_title_image(id, range, [&](const leonida::ReplayFunctions& replay,
                                                                               const leonida::LayoutPieces& pieces) {
            auto& vk = d.context->vk;
            VkResult r = vk.vkResetCommandBuffer(s.command, 0);
            if (r != VK_SUCCESS) return r;
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if ((r = vk.vkBeginCommandBuffer(s.command, &begin)) != VK_SUCCESS) return r;
            leonida::detail::image_barrier(replay, s.command, image, range, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            vk.vkCmdCopyBufferToImage(s.command, s.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      static_cast<std::uint32_t>(regions_.size()), regions_.data());
            for (const auto& piece : pieces)
                leonida::detail::image_barrier(replay, s.command, image, piece.range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    piece.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_GENERAL : piece.layout,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            if ((r = vk.vkEndCommandBuffer(s.command)) != VK_SUCCESS) return r;
            return submit_slot(d, s, /*wait_now=*/true);
        });
        if (result == VK_SUCCESS) ring_index_ = (ring_index_ + 1) % kRing;
        return result == VK_SUCCESS;
    }

private:
    struct Slot {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        std::uint8_t* mapped = nullptr;
        VkDeviceSize capacity = 0;
        VkCommandBuffer command{};
        std::uint64_t pending_value = 0;
    };

    template<class Device>
    void release_slot_buffer(Device& d, Slot& s) {
        auto& vk = d.context->vk;
        if (s.buffer) vk.vkDestroyBuffer(d.info.device, s.buffer, nullptr);
        if (s.memory) vk.vkFreeMemory(d.info.device, s.memory, nullptr);
        s.buffer = VK_NULL_HANDLE;
        s.memory = VK_NULL_HANDLE;
        s.mapped = nullptr;
        s.capacity = 0;
    }

    template<class Device>
    bool wait_slot(Device& d, Slot& s) {
        if (!s.pending_value || !ready_) return true;
        auto& vk = d.context->vk;
        VkSemaphoreWaitInfo done{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        done.semaphoreCount = 1;
        done.pSemaphores = &ready_;
        done.pValues = &s.pending_value;
        if (vk.vkWaitSemaphores(d.info.device, &done, UINT64_MAX) != VK_SUCCESS) return false;
        s.pending_value = 0;
        return true;
    }

    template<class Device>
    bool prepare_slot(Device& d, Slot& s, VkDeviceSize size) {
        auto& vk = d.context->vk;
        const auto dev = d.info.device;
        if (!pool_) {
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = d.info.queue_family;
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            if (vk.vkCreateCommandPool(dev, &pool, nullptr, &pool_) != VK_SUCCESS) return false;
        }
        if (!s.command) {
            VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            alloc.commandPool = pool_;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = 1;
            if (vk.vkAllocateCommandBuffers(dev, &alloc, &s.command) != VK_SUCCESS) return false;
        }
        if (!ready_) {
            VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline};
            if (vk.vkCreateSemaphore(dev, &info, nullptr, &ready_) != VK_SUCCESS) return false;
            for (const auto* qs : {&d.info.command_queues, &d.info.compute_queues, &d.info.copy_queues})
                for (auto q : *qs)
                    if (q && q != d.info.queue && std::find(consumers_.begin(), consumers_.end(), q) == consumers_.end())
                        consumers_.push_back(q);
        }
        if (!wait_slot(d, s)) return false;
        if (size <= s.capacity) return true;
        release_slot_buffer(d, s);
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = size;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vk.vkCreateBuffer(dev, &buffer, nullptr, &s.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vk.vkGetBufferMemoryRequirements(dev, s.buffer, &req);
        s.memory = d.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        void* map = nullptr;
        if (!s.memory || vk.vkBindBufferMemory(dev, s.buffer, s.memory, 0) != VK_SUCCESS ||
            vk.vkMapMemory(dev, s.memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
            release_slot_buffer(d, s);
            return false;
        }
        s.mapped = static_cast<std::uint8_t*>(map);
        s.capacity = size;
        return true;
    }

    template<class Device>
    VkResult submit_slot(Device& d, Slot& s, bool wait_now) {
        auto& vk = d.context->vk;
        ++value_;
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &value_;
        VkSubmitInfo transfer{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline};
        transfer.commandBufferCount = 1;
        transfer.pCommandBuffers = &s.command;
        transfer.signalSemaphoreCount = 1;
        transfer.pSignalSemaphores = &ready_;
        auto r = vk.vkQueueSubmit(d.info.queue, 1, &transfer, VK_NULL_HANDLE);
        if (r != VK_SUCCESS) return r;
        s.pending_value = value_;
        if (wait_now) {
            VkSemaphoreWaitInfo done{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            done.semaphoreCount = 1;
            done.pSemaphores = &ready_;
            done.pValues = &value_;
            if ((r = vk.vkWaitSemaphores(d.info.device, &done, UINT64_MAX)) != VK_SUCCESS) return r;
            s.pending_value = 0;
        }
        // GPU writes become visible to later submissions on every other queue. The primary queue has the image barrier.
        if (!consumers_.empty()) {
            VkTimelineSemaphoreSubmitInfo acquire{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            acquire.waitSemaphoreValueCount = 1;
            acquire.pWaitSemaphoreValues = &value_;
            VkPipelineStageFlags stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO, &acquire};
            wait.waitSemaphoreCount = 1;
            wait.pWaitSemaphores = &ready_;
            wait.pWaitDstStageMask = &stages;
            for (auto q : consumers_)
                if ((r = vk.vkQueueSubmit(q, 1, &wait, VK_NULL_HANDLE)) != VK_SUCCESS) return r;
        }
        return VK_SUCCESS;
    }

    Slot slots_[kRing]{};
    std::size_t ring_index_ = 0;
    VkCommandPool pool_{};
    VkSemaphore ready_{};
    std::uint64_t value_ = 0;
    std::vector<VkQueue> consumers_;
    struct BatchItem {
        leonida::ResourceId id;
        leonida::ResourceBinding image;
        VkImageSubresourceRange range;
        std::size_t region_start;
        std::size_t region_count;
    };
    bool batching_ = false;
    VkDeviceSize batch_bytes_ = 0;
    std::vector<BatchItem> batch_items_;
    std::vector<VkBufferImageCopy> batch_regions_;
    std::vector<VkBufferImageCopy> regions_;
};

}  // namespace rt::d3d12
