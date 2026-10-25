/* Owned offscreen dzn clear/readback probe. No window or presentation claim. */
#define VK_NO_PROTOTYPES
#include <windows.h>
#include "vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#define I(n) PFN_##n n=(PFN_##n)gip(instance,#n)
#define D(n) PFN_##n n=(PFN_##n)gd(device,#n)
#define CHECK(call) do { VkResult r=(call); printf("%s = %d\n",#call,r); if(r!=VK_SUCCESS) return 10; } while(0)
static uint32_t memory_type(VkPhysicalDeviceMemoryProperties *p,uint32_t bits,VkMemoryPropertyFlags flags) {
    for(uint32_t i=0;i<p->memoryTypeCount;i++)
        if((bits&(1u<<i)) && (p->memoryTypes[i].propertyFlags&flags)==flags) return i;
    fprintf(stderr,"Required memory type unavailable\n"); exit(11);
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    puts("O3 offscreen Vulkan clear/readback attempt; lavapipe != Xbox D3D12");
    HMODULE dll=LoadLibraryA("vulkan_dzn.dll");
    if(!dll){printf("LoadLibrary vulkan_dzn.dll error=%lu\n",GetLastError());return 1;}
    PFN_vkGetInstanceProcAddr gip=(PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(dll,"vk_icdGetInstanceProcAddr");
    if(!gip)return 2;
    PFN_vkCreateInstance create=(PFN_vkCreateInstance)gip(NULL,"vkCreateInstance");
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="R4G-O3-clear",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app}; VkInstance instance;
    CHECK(create(&ci,NULL,&instance));
    I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
    I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance);
    uint32_t count=0; CHECK(vkEnumeratePhysicalDevices(instance,&count,NULL));
    printf("physicalDeviceCount=%u\n",count); if(!count)return 3;
    VkPhysicalDevice *physical=calloc(count,sizeof(*physical)); if(!physical)return 12;
    CHECK(vkEnumeratePhysicalDevices(instance,&count,physical));
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(physical[0],&props); printf("adapter=%s\n",props.deviceName);
    uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(physical[0],&nq,NULL);
    VkQueueFamilyProperties *queues=calloc(nq,sizeof(*queues)); if(!queues)return 12;
    vkGetPhysicalDeviceQueueFamilyProperties(physical[0],&nq,queues);
    uint32_t family=~0u; for(uint32_t i=0;i<nq;i++)if(queues[i].queueCount&&(queues[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){family=i;break;}
    if(family==~0u)return 4;
    float priority=1; VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi}; VkDevice device;
    CHECK(vkCreateDevice(physical[0],&di,NULL,&device)); PFN_vkGetDeviceProcAddr gd=vkGetDeviceProcAddr;
    D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkAllocateMemory); D(vkBindImageMemory);
    D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkBindBufferMemory); D(vkCreateCommandPool);
    D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkCmdPipelineBarrier); D(vkCmdClearColorImage);
    D(vkCmdCopyImageToBuffer); D(vkEndCommandBuffer); D(vkGetDeviceQueue); D(vkQueueSubmit); D(vkQueueWaitIdle);
    D(vkMapMemory); D(vkUnmapMemory); D(vkDestroyCommandPool); D(vkDestroyBuffer); D(vkDestroyImage); D(vkFreeMemory); D(vkDestroyDevice);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physical[0],&mp);
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,.extent={64,64,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT}; VkImage image;
    CHECK(vkCreateImage(device,&ii,NULL,&image)); VkMemoryRequirements mr; vkGetImageMemoryRequirements(device,image,&mr);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=memory_type(&mp,mr.memoryTypeBits,0)}; VkDeviceMemory imem,bmem;
    CHECK(vkAllocateMemory(device,&ai,NULL,&imem)); CHECK(vkBindImageMemory(device,image,imem,0));
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=64*64*4,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT}; VkBuffer buffer;
    CHECK(vkCreateBuffer(device,&bi,NULL,&buffer)); vkGetBufferMemoryRequirements(device,buffer,&mr);
    ai.allocationSize=mr.size; ai.memoryTypeIndex=memory_type(&mp,mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkAllocateMemory(device,&ai,NULL,&bmem)); CHECK(vkBindBufferMemory(device,buffer,bmem,0));
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family}; VkCommandPool pool; CHECK(vkCreateCommandPool(device,&pi,NULL,&pool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1}; VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device,&ca,&cmd));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&begin));
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange=range};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkClearColorValue color={.float32={1,0,0,1}}; vkCmdClearColorImage(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&range);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT; barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={64,64,1}};
    vkCmdCopyImageToBuffer(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
    CHECK(vkEndCommandBuffer(cmd)); VkQueue queue; vkGetDeviceQueue(device,family,0,&queue);
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd}; CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    unsigned char *pixels; CHECK(vkMapMemory(device,bmem,0,VK_WHOLE_SIZE,0,(void **)&pixels));
    unsigned bad=0; for(unsigned i=0;i<64*64;i++)if(pixels[4*i]!=255||pixels[4*i+1]!=0||pixels[4*i+2]!=0||pixels[4*i+3]!=255)bad++;
    printf("red clear readback: pixels=4096 mismatches=%u\n",bad);
    vkUnmapMemory(device,bmem); vkDestroyCommandPool(device,pool,NULL); vkDestroyBuffer(device,buffer,NULL); vkDestroyImage(device,image,NULL); vkFreeMemory(device,bmem,NULL); vkFreeMemory(device,imem,NULL); vkDestroyDevice(device,NULL); vkDestroyInstance(instance,NULL); free(physical);free(queues);FreeLibrary(dll);
    if(bad)return 20;
    puts("PASS offscreen clear/readback only; no presentation or Xbox proof");return 0;
}
