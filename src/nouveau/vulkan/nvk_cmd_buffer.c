#include "nvk_cmd_buffer.h"

#include "nvk_buffer.h"
#include "nvk_device.h"
#include "nvk_device_memory.h"
#include "nvk_physical_device.h"

#include "nouveau_bo.h"
#include "nouveau_push.h"

#include "classes/cla0b5.h"
#include "push906f.h"

static void
nvk_destroy_cmd_buffer(struct nvk_cmd_buffer *cmd_buffer)
{
   list_del(&cmd_buffer->pool_link);

   nouveau_ws_push_destroy(cmd_buffer->push);
   vk_command_buffer_finish(&cmd_buffer->vk);
   vk_free(&cmd_buffer->pool->vk.alloc, cmd_buffer);
}

static VkResult
nvk_create_cmd_buffer(struct nvk_device *device, struct nvk_cmd_pool *pool,
                      VkCommandBufferLevel level, VkCommandBuffer *pCommandBuffer)
{
   struct nvk_cmd_buffer *cmd_buffer;

   cmd_buffer = vk_zalloc(&pool->vk.alloc, sizeof(*cmd_buffer), 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd_buffer == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result =
      vk_command_buffer_init(&pool->vk, &cmd_buffer->vk, NULL, level);
   if (result != VK_SUCCESS) {
      vk_free(&cmd_buffer->pool->vk.alloc, cmd_buffer);
      return result;
   }

   cmd_buffer->pool = pool;
   list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);

   cmd_buffer->push = nouveau_ws_push_new(device->pdev->dev, NVK_CMD_BUF_SIZE);
   *pCommandBuffer = nvk_cmd_buffer_to_handle(cmd_buffer);
   return VK_SUCCESS;
}

VkResult
nvk_reset_cmd_buffer(struct nvk_cmd_buffer *cmd_buffer)
{
   vk_command_buffer_reset(&cmd_buffer->vk);

   nouveau_ws_push_reset(cmd_buffer->push);

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateCommandPool(VkDevice _device, const VkCommandPoolCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator, VkCommandPool *pCmdPool)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   struct nvk_cmd_pool *pool;

   pool =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(*pool), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pool == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = vk_command_pool_init(&device->vk, &pool->vk, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, pAllocator, pool);
      return result;
   }

   list_inithead(&pool->cmd_buffers);
   list_inithead(&pool->free_cmd_buffers);

   *pCmdPool = nvk_cmd_pool_to_handle(pool);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyCommandPool(VkDevice _device, VkCommandPool commandPool,
                       const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_cmd_pool, pool, commandPool);

   if (!pool)
      return;

   list_for_each_entry_safe(struct nvk_cmd_buffer, cmd_buffer, &pool->cmd_buffers, pool_link)
   {
      nvk_destroy_cmd_buffer(cmd_buffer);
   }

   list_for_each_entry_safe(struct nvk_cmd_buffer, cmd_buffer, &pool->free_cmd_buffers, pool_link)
   {
      nvk_destroy_cmd_buffer(cmd_buffer);
   }

   vk_command_pool_finish(&pool->vk);
   vk_free2(&device->vk.alloc, pAllocator, pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_ResetCommandPool(VkDevice device, VkCommandPool commandPool, VkCommandPoolResetFlags flags)
{
   VK_FROM_HANDLE(nvk_cmd_pool, pool, commandPool);
   VkResult result;

   list_for_each_entry(struct nvk_cmd_buffer, cmd_buffer, &pool->cmd_buffers, pool_link)
   {
      result = nvk_reset_cmd_buffer(cmd_buffer);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_TrimCommandPool(VkDevice device, VkCommandPool commandPool, VkCommandPoolTrimFlags flags)
{
   VK_FROM_HANDLE(nvk_cmd_pool, pool, commandPool);

   list_for_each_entry_safe(struct nvk_cmd_buffer, cmd_buffer, &pool->free_cmd_buffers, pool_link)
   {
      nvk_destroy_cmd_buffer(cmd_buffer);
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_AllocateCommandBuffers(VkDevice _device,
                           const VkCommandBufferAllocateInfo *pAllocateInfo,
                           VkCommandBuffer *pCommandBuffers)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_cmd_pool, pool, pAllocateInfo->commandPool);
   uint32_t i;
   VkResult result = VK_SUCCESS;

   for (i = 0; i < pAllocateInfo->commandBufferCount; i++) {
      if (!list_is_empty(&pool->free_cmd_buffers)) {
         struct nvk_cmd_buffer *cmd_buffer =
            list_first_entry(&pool->free_cmd_buffers, struct nvk_cmd_buffer, pool_link);

         list_del(&cmd_buffer->pool_link);
         list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);

         result = nvk_reset_cmd_buffer(cmd_buffer);
         vk_command_buffer_finish(&cmd_buffer->vk);
         VkResult init_result =
            vk_command_buffer_init(&pool->vk, &cmd_buffer->vk, NULL,
                                   pAllocateInfo->level);
         if (init_result != VK_SUCCESS)
            result = init_result;

         pCommandBuffers[i] = nvk_cmd_buffer_to_handle(cmd_buffer);
      } else {
         result = nvk_create_cmd_buffer(device, pool, pAllocateInfo->level, &pCommandBuffers[i]);
      }
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS) {
      nvk_FreeCommandBuffers(_device, pAllocateInfo->commandPool, i, pCommandBuffers);
      /* From the Vulkan 1.0.66 spec:
       *
       * "vkAllocateCommandBuffers can be used to create multiple
       *  command buffers. If the creation of any of those command
       *  buffers fails, the implementation must destroy all
       *  successfully created command buffer objects from this
       *  command, set all entries of the pCommandBuffers array to
       *  NULL and return the error."
       */
      memset(pCommandBuffers, 0, sizeof(*pCommandBuffers) * pAllocateInfo->commandBufferCount);
   }
   return result;
}

VKAPI_ATTR void VKAPI_CALL
nvk_FreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount,
                       const VkCommandBuffer *pCommandBuffers)
{
   VK_FROM_HANDLE(nvk_cmd_pool, pool, commandPool);
   for (uint32_t i = 0; i < commandBufferCount; i++) {
      VK_FROM_HANDLE(nvk_cmd_buffer, cmd_buffer, pCommandBuffers[i]);

      if (!cmd_buffer)
         continue;
      assert(cmd_buffer->pool == pool);

      list_del(&cmd_buffer->pool_link);
      list_addtail(&cmd_buffer->pool_link, &pool->free_cmd_buffers);
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_ResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd_buffer, commandBuffer);
   return nvk_reset_cmd_buffer(cmd_buffer);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_BeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   if (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)
      cmd->reset_on_submit = true;
   else
      cmd->reset_on_submit = false;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdCopyBuffer(VkCommandBuffer commandBuffer,
                  VkBuffer srcBuffer, VkBuffer dstBuffer,
                  uint32_t regionCount, const VkBufferCopy* pRegions)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_buffer, src, srcBuffer);
   VK_FROM_HANDLE(nvk_buffer, dst, dstBuffer);
   struct nouveau_ws_push *push = cmd->push;

   nouveau_ws_push_ref(push, src->mem->bo, NOUVEAU_WS_BO_RD);
   nouveau_ws_push_ref(push, dst->mem->bo, NOUVEAU_WS_BO_WR);

   for (unsigned r = 0; r < regionCount; r++) {
      const VkBufferCopy *region = &pRegions[r];
      VkDeviceSize dstoff = dst->mem->bo->offset + dst->offset + region->dstOffset;
      VkDeviceSize srcoff = src->mem->bo->offset + src->offset + region->srcOffset;
      VkDeviceSize size = region->size;

      while (size) {
         unsigned bytes = MIN2(size, 1 << 17);

         PUSH_MTHD(push, NVA0B5, OFFSET_IN_UPPER,
                   NVVAL(NVA0B5, OFFSET_IN_UPPER, UPPER, srcoff >> 32),
                                 OFFSET_IN_LOWER, srcoff & 0xffffffff,

                                 OFFSET_OUT_UPPER,
                   NVVAL(NVA0B5, OFFSET_OUT_UPPER, UPPER, dstoff >> 32),
                                 OFFSET_OUT_LOWER, dstoff & 0xffffffff);

         PUSH_MTHD(push, NVA0B5, LINE_LENGTH_IN, bytes,
                                 LINE_COUNT, 1);

         PUSH_IMMD(push, NVA0B5, LAUNCH_DMA,
                   NVDEF(NVA0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
                   NVDEF(NVA0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
                   NVDEF(NVA0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) |
                   NVDEF(NVA0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH));

         srcoff += bytes;
         dstoff += bytes;
         size -= bytes;
      }
   }
}


VKAPI_ATTR void VKAPI_CALL
nvk_CmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo *pDependencyInfo) {
}