// Owns shadPS4's GPU model (Liverpool command processor) and Vulkan renderer.
// Runs headless; our own window shows frames.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace gpu {

// Creates the Vulkan device, command processor and rasterizer. Returns false
// if no usable GPU (the runtime then keeps its scaffolding path).
bool Init();
bool Active();

// Tells the GPU caches about game memory so CPU writes to it are tracked
// (call for every mapping; mappings made before Init are kept until then).
void MapMemory(uint64_t addr, uint64_t size);
void UnmapMemory(uint64_t addr, uint64_t size);

// Queues one graphics command buffer pair (sizes in bytes) for the GPU model.
// The first call also sends the SDK's hardware init sequence, like the Gnm driver.
void SubmitGfx(const uint32_t* dcb, uint32_t dcbBytes, const uint32_t* ccb, uint32_t ccbBytes);

// Blocks until the GPU model has processed everything submitted so far.
void WaitIdle();
// How often a submit returned before the GPU finished (it stayed blocked on a wait), since
// the last call.
uint32_t TakeEarlyReleases();
// How many wait addresses were found to be written by the guest after its submit (total).
uint32_t LearnedWaits();

// Async compute rings (sceGnmMapComputeQueue / sceGnmDingDong). MapComputeQueue returns
// the 1-based virtual queue id, or a negative error.
int MapComputeQueue(uint32_t pipeId, uint32_t queueId, uint64_t ringBase, uint32_t ringSizeDw, uint32_t* readPtr);
void UnmapComputeQueue(uint32_t vqid);
void DingDong(uint32_t vqid, uint32_t nextOffsDw);

// Copies the GPU's image for a display buffer into `out` (BGRA8, tightly
// packed). Must be called on the GPU thread (e.g. from the flip interrupt).
// Returns false if the renderer has nothing for that buffer yet.
struct DisplayBufferDesc {
    uint64_t address;
    uint32_t width, height, pitchPixels, pixelFormat, tilingMode;
};
// Debug: LBP3_DUMP_TARGETS=<flip> saves every render target at that flip.
void MaybeDumpRenderTargets();
// F9 hotkey: trace the next frame and dump its render targets.
void RequestTargetDump();

bool ReadbackDisplayBuffer(const DisplayBufferDesc& desc, std::vector<uint8_t>& out, uint32_t& width,
                           uint32_t& height);

// Called (from the GPU thread) for every interrupt the GPU model raises, e.g.
// 0x40 end-of-pipe. Used to fire the game's sceGnmAddEqEvent events.
void SetInterruptHandler(std::function<void(uint32_t)> handler);

}  // namespace gpu
