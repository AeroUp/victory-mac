// Per-frame cost counters for the slow-frame log (LEONIDA_HITCH_MS, d3d12_xs_device.cpp).
//
// Each counter sums microseconds spent in one part of a submission since the log last took it. The present path
// takes them all once per frame, so a slow frame's line says how much of it the runtime spent where; the rest of
// the frame is the title's own time (its CPU work, and its waits on fences the runtime does not see).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace leonida::frame_profile {

enum Part : unsigned {
    room,            // VulkanQueue::wait_for_room: the GPU is kMaxQueuedCommandBuffers submissions behind
    shadows,         // DeviceState::refresh_shadows: title pages copied into shadow buffers and title windows
    geometry,        // vertex and index windows over the title's memory
    title_textures,  // refresh_title_textures: hashing and refilling textures the CPU writes
    upload_idle,     // inside it: vkDeviceWaitIdle before a staged title-texture upload
    upload_slot,     // inside it: waiting for a staging ring slot's last copy to finish
    part_count
};

inline std::atomic<std::uint64_t> part_us[part_count]{};

// Adds the time from construction to destruction to one part.
class Scope {
public:
    explicit Scope(Part part) : part_(part), start_(std::chrono::steady_clock::now()) {}
    ~Scope() {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_);
        part_us[part_].fetch_add(std::uint64_t(us.count()), std::memory_order_relaxed);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Part part_;
    std::chrono::steady_clock::time_point start_;
};

inline std::uint64_t take(Part part) { return part_us[part].exchange(0, std::memory_order_relaxed); }

// The longest a command buffer took from vkQueueSubmit to its timeline signal, among those that finished since the
// log last took it: how far behind the GPU runs (queued work ahead of it included).
inline std::atomic<std::uint64_t> gpu_latency_max_us{0};
inline std::atomic<std::uint64_t> gpu_done{0};

inline bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("LEONIDA_HITCH_MS");
        return v && *v && *v != '0';
    }();
    return on;
}

inline void note_gpu_latency(std::uint64_t us) {
    std::uint64_t seen = gpu_latency_max_us.load(std::memory_order_relaxed);
    while (us > seen && !gpu_latency_max_us.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {
    }
    gpu_done.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace leonida::frame_profile
