
#include <array>
#include <iostream>
#include <vector>
#include <vulkan/vulkan_core.h>

#include "VkBootstrap.h"
#include "file_reader.hpp"
#include "vma_usage.h"

constexpr auto n = 1024;

constexpr uint32_t computeShaderProcessUnit() { return 256; }

struct Init {
  vkb::Instance instance;
  vkb::DispatchTable disp;
  vkb::Device device;
};

VmaAllocator allocator;

struct ComputeData {
  VkQueue compute_queue;

  VkDescriptorSetLayout compute_descriptor_set_layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet descriptor_set;

  VkPipelineLayout compute_pipeline_layout;
  VkPipeline compute_pipeline;

  VkCommandPool command_pool;
  VkCommandBuffer command_buffer;
};

VkShaderModule createShaderModule(Init &init, const std::vector<char> &code) {
  VkShaderModuleCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  create_info.codeSize = code.size();
  create_info.pCode = reinterpret_cast<const uint32_t *>(code.data());

  VkShaderModule shaderModule;
  if (init.disp.createShaderModule(&create_info, nullptr, &shaderModule) !=
      VK_SUCCESS) {
    return VK_NULL_HANDLE; // failed to create shader module
  }

  return shaderModule;
}

int device_initialization(Init &init) {
  vkb::InstanceBuilder instance_builder;
  auto inst_ret = instance_builder.set_app_name("Example Vulkan Application")
                      .request_validation_layers()
                      .use_default_debug_messenger()
                      .build();
  if (!inst_ret) {
    std::cerr << "Failed to create Vulkan instance. Error: "
              << inst_ret.error().message() << "\n";
    return -1;
  }

  init.instance = inst_ret.value();

  vkb::PhysicalDeviceSelector selector{init.instance};
  auto phys_ret =
      selector.defer_surface_initialization()
          .set_minimum_version(1, 1) // require a vulkan 1.1 capable device
          .require_separate_compute_queue()
          .select();
  if (!phys_ret) {
    std::cerr << "Failed to select Vulkan Physical Device. Error: "
              << phys_ret.error().message() << "\n";
    exit(1);
  }
  std::cout << "selected GPU: " << phys_ret.value().properties.deviceName
            << '\n';

  vkb::DeviceBuilder device_builder{phys_ret.value()};
  auto dev_ret = device_builder.build();
  if (!dev_ret) {
    std::cerr << "Failed to create Vulkan device. Error: "
              << dev_ret.error().message() << "\n";
    exit(1);
  }

  init.device = dev_ret.value();
  init.disp = init.device.make_table();

  return 0;
}

int get_queues(Init &init, ComputeData &data) {
  auto cq = init.device.get_queue(vkb::QueueType::compute);
  if (!cq.has_value()) {
    std::cout << "failed to get graphics queue: " << cq.error().message()
              << "\n";
    return -1;
  }
  data.compute_queue = cq.value();
  return 0;
}

void vma_initialization(Init &init) {
  VmaVulkanFunctions vulkanFunctions = {};
  vulkanFunctions.vkGetInstanceProcAddr = &vkGetInstanceProcAddr;
  vulkanFunctions.vkGetDeviceProcAddr = &vkGetDeviceProcAddr;

  VmaAllocatorCreateInfo allocatorCreateInfo = {};
  // allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_3;
  allocatorCreateInfo.physicalDevice = init.device.physical_device;
  allocatorCreateInfo.device = init.device.device;
  allocatorCreateInfo.instance = init.instance.instance;
  allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions;
  vmaCreateAllocator(&allocatorCreateInfo, &allocator);
}

int create_descriptor_set_layout_v2(Init &init, ComputeData &data) {
  std::array<VkDescriptorSetLayoutBinding, 1> binding;
  binding[0].binding = 0;
  binding[0].descriptorCount = 1;
  binding[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  binding[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  createInfo.bindingCount = 1;
  createInfo.pBindings = binding.data();

  if (init.disp.createDescriptorSetLayout(
          &createInfo, nullptr, &data.compute_descriptor_set_layout) !=
      VK_SUCCESS) {
    std::cout << "failed to create descriptor set layout\n";
    return -1;
  }

  return 0;
}

int create_command_pool(Init &init, ComputeData &data) {
  VkCommandPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.queueFamilyIndex =
      init.device.get_queue_index(vkb::QueueType::compute).value();

  if (init.disp.createCommandPool(&pool_info, nullptr, &data.command_pool) !=
      VK_SUCCESS) {
    std::cout << "failed to create command pool\n";
    return -1;
  }
  return 0;
}

int create_compute_pipeline(Init &init, ComputeData &data) {
  // Load & Create Shader Modules (1/3)
  auto compute_shader_code = readFile("shaders/square.spv");
  auto compute_module = createShaderModule(init, compute_shader_code);

  if (compute_module == VK_NULL_HANDLE) {
    std::cout << "failed to create shader module\n";
    return -1;
  }

  VkPipelineShaderStageCreateInfo shader_stage_create_info = {};
  shader_stage_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stage_create_info.module = compute_module;
  shader_stage_create_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  shader_stage_create_info.pName = "main";

  // Create a Pipeline Layout (2/3)
  VkPipelineLayoutCreateInfo layout_create_info = {};
  layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_create_info.setLayoutCount = 1;
  layout_create_info.pSetLayouts = &data.compute_descriptor_set_layout;
  layout_create_info.pushConstantRangeCount = 0;
  layout_create_info.pPushConstantRanges = nullptr;

  if (init.disp.createPipelineLayout(&layout_create_info, nullptr,
                                     &data.compute_pipeline_layout) !=
      VK_SUCCESS) {
    std::cout << "failed to create pipeline layout\n";
    return -1;
  }

  // Pipeline itself (3/3)
  VkComputePipelineCreateInfo pipeline_create_info = {};
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;
  pipeline_create_info.stage = shader_stage_create_info;
  pipeline_create_info.layout = data.compute_pipeline_layout;

  if (init.disp.createComputePipelines(VK_NULL_HANDLE, 1, &pipeline_create_info,
                                       nullptr,
                                       &data.compute_pipeline) != VK_SUCCESS) {
    std::cout << "failed to create compute pipeline\n";
    return -1;
  }

  init.disp.destroyShaderModule(compute_module, nullptr);
  return 0;
}

int create_descriptor_pool(Init &init, ComputeData &data) {
  VkDescriptorPoolSize pool_size;
  pool_size.descriptorCount = 1;
  pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

  VkDescriptorPoolCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  create_info.poolSizeCount = 1;
  create_info.pPoolSizes = &pool_size;
  create_info.maxSets = 1;

  if (init.disp.createDescriptorPool(&create_info, nullptr,
                                     &data.descriptor_pool) != VK_SUCCESS) {
    std::cout << "failed to create descriptor pool\n";
    return -1;
  }
  return 0;
}

int create_descriptor_set(Init &init, ComputeData &data, VkBuffer &buffer) {
  VkDescriptorSetAllocateInfo alloc_info = {};
  alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc_info.descriptorPool = data.descriptor_pool;
  alloc_info.descriptorSetCount = 1;
  alloc_info.pSetLayouts = &data.compute_descriptor_set_layout;

  if (init.disp.allocateDescriptorSets(&alloc_info, &data.descriptor_set) !=
      VK_SUCCESS) {
    std::cout << "failed to allocate descriptor set\n";
    return -1;
  }

  VkDescriptorBufferInfo bufferInfo;
  bufferInfo.buffer = buffer;
  bufferInfo.offset = 0;
  bufferInfo.range = n * sizeof(float);

  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.dstBinding = 0;
  write.dstArrayElement = 0;
  write.dstSet = data.descriptor_set;
  write.pBufferInfo = &bufferInfo;

  init.disp.updateDescriptorSets(1, &write, 0, nullptr);

  return 0;
}

void cleanup(Init &init, ComputeData &data) {
  vmaDestroyAllocator(allocator);

  init.disp.destroyDescriptorPool(data.descriptor_pool, nullptr);

  init.disp.destroyCommandPool(data.command_pool, nullptr);
  init.disp.destroyDescriptorSetLayout(data.compute_descriptor_set_layout,
                                       nullptr);

  init.disp.destroyPipeline(data.compute_pipeline, nullptr);
  init.disp.destroyPipelineLayout(data.compute_pipeline_layout, nullptr);

  vkb::destroy_device(init.device);
  vkb::destroy_instance(init.instance);
}

inline void vkCheck(int result) {
  if (result != 0) {
    exit(1);
  }
}

// allocate command buffer
// record command buffer
// submit
// wait for queue idle
// read data back
int execute(Init &init, ComputeData &data, VkBuffer &buffer,
            VmaAllocation &allocation, const std::vector<float> &input_data) {
  std::cout << "input data:\n";
  for (size_t i = 0; i < input_data.size(); ++i) {
    if (i % 64 == 0 && i != 0)
      std::cout << '\n';
    std::cout << input_data[i];
  }
  std::cout << '\n';

  // -------

  VkCommandBufferAllocateInfo alloc_info = {};
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.commandPool = data.command_pool;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = 1;
  if (init.disp.allocateCommandBuffers(&alloc_info, &data.command_buffer) !=
      VK_SUCCESS) {
    std::cout << "failed to allocate command buffers\n";
    return 1;
  }

  // ------- RECORD COMMAND BUFFER --------
  VkCommandBufferBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  init.disp.beginCommandBuffer(data.command_buffer, &begin_info);

  init.disp.cmdBindPipeline(data.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            data.compute_pipeline);
  init.disp.cmdBindDescriptorSets(
      data.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
      data.compute_pipeline_layout, 0, 1, &data.descriptor_set, 0, nullptr);

  const auto group_count_x =
      static_cast<uint32_t>(input_data.size() / computeShaderProcessUnit());
  init.disp.cmdDispatch(data.command_buffer, group_count_x, 1, 1);

  init.disp.endCommandBuffer(data.command_buffer);

  // -------

  VkSubmitInfo submit_info = {};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &data.command_buffer;
  submit_info.waitSemaphoreCount = 0;
  submit_info.signalSemaphoreCount = 0;
  if (vkQueueSubmit(data.compute_queue, 1, &submit_info, VK_NULL_HANDLE) !=
      VK_SUCCESS) {
    std::cout << "failed to submit queue\n";
    return 1;
  }

  // wait the calculation to finish
  if (vkQueueWaitIdle(data.compute_queue) != VK_SUCCESS)
    throw std::runtime_error("failed to wait queue idle!");

  std::vector<float> output_data(n);

  void *mapped_data;
  vmaMapMemory(allocator, allocation, &mapped_data);
  memcpy(output_data.data(), mapped_data, n * sizeof(float));
  vmaUnmapMemory(allocator, allocation);

  // -------

  std::cout << "output data:\n";
  for (size_t i = 0; i < output_data.size(); ++i) {
    if (i % 64 == 0 && i != 0)
      std::cout << '\n';
    std::cout << output_data[i];
  }
  std::cout << '\n';
  return 0;
}

int main() {
  // ------------------ DATA ------------------

  std::vector<float> h_data(n, 1.0f);

  // ------------------ INITIALIZATION ------------------
  Init init;
  ComputeData compute_data;

  // setting up vulkan
  vkCheck(device_initialization(init));
  vkCheck(get_queues(init, compute_data));
  vma_initialization(init);

  VkBuffer buffer;
  VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bufferInfo.size = n * sizeof(float);
  bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  bufferInfo.queueFamilyIndexCount = 0;
  bufferInfo.pQueueFamilyIndices = nullptr;

  VmaAllocationCreateInfo allocInfo = {};
  allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
  allocInfo.requiredFlags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

  VmaAllocation allocation;
  vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer, &allocation,
                  nullptr);

  void *mappedMemory = nullptr;
  vmaMapMemory(allocator, allocation, &mappedMemory);
  memcpy(mappedMemory, h_data.data(), n * sizeof(float));
  vmaUnmapMemory(allocator, allocation);

  vkCheck(create_descriptor_set_layout_v2(init, compute_data));
  vkCheck(create_compute_pipeline(init, compute_data));

  vkCheck(create_descriptor_pool(init, compute_data));
  vkCheck(create_descriptor_set(init, compute_data, buffer));

  vkCheck(create_command_pool(init, compute_data));

  execute(init, compute_data, buffer, allocation, h_data);

  vmaDestroyBuffer(allocator, buffer, allocation);

  cleanup(init, compute_data);

  std::cout << "Exiting normally" << std::endl;
  return 0;
}