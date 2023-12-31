#include <vulkan/vulkan_core.h>

#include <array>
#include <iostream>
#include <vector>

#include "VkBootstrap.h"
#include "file_reader.hpp"
#include "vma_usage.h"

constexpr auto kN = 1024;

constexpr uint32_t ComputeShaderProcessUnit() { return 256; }

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

[[nodiscard]] VkShaderModule CreateShaderModule(const Init &init,
                                                const std::vector<char> &code) {
  const VkShaderModuleCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = code.size(),
      .pCode = reinterpret_cast<const uint32_t *>(code.data()),
  };

  VkShaderModule shader_module;
  if (init.disp.createShaderModule(&create_info, nullptr, &shader_module) !=
      VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }

  return shader_module;
}

[[nodiscard]] int device_initialization(Init &init) {
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
    return -1;
  }
  std::cout << "selected GPU: " << phys_ret.value().properties.deviceName
            << '\n';

  vkb::DeviceBuilder device_builder{phys_ret.value()};
  auto dev_ret = device_builder.build();
  if (!dev_ret) {
    std::cerr << "Failed to create Vulkan device. Error: "
              << dev_ret.error().message() << "\n";
    return -1;
  }

  init.device = dev_ret.value();
  init.disp = init.device.make_table();
  return 0;
}

[[nodiscard]] int get_queues(const Init &init, ComputeData &data) {
  auto cq = init.device.get_queue(vkb::QueueType::compute);
  if (!cq.has_value()) {
    std::cout << "failed to get graphics queue: " << cq.error().message()
              << "\n";
    return -1;
  }
  data.compute_queue = cq.value();
  return 0;
}

void vma_initialization(const Init &init) {
  constexpr VmaVulkanFunctions vulkan_functions = {
      .vkGetInstanceProcAddr = &vkGetInstanceProcAddr,
      .vkGetDeviceProcAddr = &vkGetDeviceProcAddr};

  const VmaAllocatorCreateInfo allocator_create_info = {
      .physicalDevice = init.device.physical_device,
      .device = init.device.device,
      .pVulkanFunctions = &vulkan_functions,
      .instance = init.instance.instance};
  vmaCreateAllocator(&allocator_create_info, &allocator);
}

[[nodiscard]] int create_descriptor_set_layout_v2(const Init &init,
                                                  ComputeData &data) {
  const std::array<VkDescriptorSetLayoutBinding, 1> binding{
      VkDescriptorSetLayoutBinding{
          .binding = 0,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
  };

  const VkDescriptorSetLayoutCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = binding.data(),
  };

  if (init.disp.createDescriptorSetLayout(
          &create_info, nullptr, &data.compute_descriptor_set_layout) !=
      VK_SUCCESS) {
    std::cout << "failed to create descriptor set layout\n";
    return -1;
  }

  return 0;
}

[[nodiscard]] int create_command_pool(const Init &init, ComputeData &data) {
  const VkCommandPoolCreateInfo pool_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex =
          init.device.get_queue_index(vkb::QueueType::compute).value(),
  };

  if (init.disp.createCommandPool(&pool_info, nullptr, &data.command_pool) !=
      VK_SUCCESS) {
    std::cout << "failed to create command pool\n";
    return -1;
  }
  return 0;
}

[[nodiscard]] int create_compute_pipeline(const Init &init, ComputeData &data) {
  // Load & Create Shader Modules (1/3)
  const auto compute_shader_code = readFile("shaders/square.spv");
  const auto compute_module = CreateShaderModule(init, compute_shader_code);

  if (compute_module == VK_NULL_HANDLE) {
    std::cout << "failed to create shader module\n";
    return -1;
  }

  const VkPipelineShaderStageCreateInfo shader_stage_create_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = compute_module,
      .pName = "main",
  };

  // Create a Pipeline Layout (2/3)
  const VkPipelineLayoutCreateInfo layout_create_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &data.compute_descriptor_set_layout,
      .pushConstantRangeCount = 0,
      .pPushConstantRanges = nullptr,
  };

  if (init.disp.createPipelineLayout(&layout_create_info, nullptr,
                                     &data.compute_pipeline_layout) !=
      VK_SUCCESS) {
    std::cout << "failed to create pipeline layout\n";
    return -1;
  }

  // Pipeline itself (3/3)
  const VkComputePipelineCreateInfo pipeline_create_info{
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = shader_stage_create_info,
      .layout = data.compute_pipeline_layout,
      .basePipelineHandle = VK_NULL_HANDLE,
      .basePipelineIndex = -1,
  };

  if (init.disp.createComputePipelines(VK_NULL_HANDLE, 1, &pipeline_create_info,
                                       nullptr,
                                       &data.compute_pipeline) != VK_SUCCESS) {
    std::cout << "failed to create compute pipeline\n";
    return -1;
  }

  init.disp.destroyShaderModule(compute_module, nullptr);
  return 0;
}

[[nodiscard]] int create_descriptor_pool(const Init &init, ComputeData &data) {
  constexpr VkDescriptorPoolSize pool_size{
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
  };

  // ReSharper disable once CppVariableCanBeMadeConstexpr
  const VkDescriptorPoolCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
  };

  if (init.disp.createDescriptorPool(&create_info, nullptr,
                                     &data.descriptor_pool) != VK_SUCCESS) {
    std::cout << "failed to create descriptor pool\n";
    return -1;
  }
  return 0;
}

[[nodiscard]] int create_descriptor_set(const Init &init, ComputeData &data,
                                        const VkBuffer &buffer) {
  const VkDescriptorSetAllocateInfo alloc_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = data.descriptor_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &data.compute_descriptor_set_layout,
  };

  if (init.disp.allocateDescriptorSets(&alloc_info, &data.descriptor_set) !=
      VK_SUCCESS) {
    std::cout << "failed to allocate descriptor set\n";
    return -1;
  }

  const VkDescriptorBufferInfo buffer_info{
      .buffer = buffer,
      .offset = 0,
      .range = kN * sizeof(float),
  };

  const VkWriteDescriptorSet write{
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = data.descriptor_set,
      .dstBinding = 0,
      .dstArrayElement = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &buffer_info,
  };

  init.disp.updateDescriptorSets(1, &write, 0, nullptr);

  return 0;
}

void cleanup(const Init &init, const ComputeData &data) {
  vmaDestroyAllocator(allocator);

  init.disp.destroyDescriptorPool(data.descriptor_pool, nullptr);

  init.disp.destroyCommandPool(data.command_pool, nullptr);
  init.disp.destroyDescriptorSetLayout(data.compute_descriptor_set_layout,
                                       nullptr);

  init.disp.destroyPipeline(data.compute_pipeline, nullptr);
  init.disp.destroyPipelineLayout(data.compute_pipeline_layout, nullptr);

  destroy_device(init.device);
  destroy_instance(init.instance);
}

inline void VkCheck(const int result) {
  if (result != 0) {
    exit(1);
  }
}

int execute(const Init &init, ComputeData &data, VkBuffer &buffer,
            const VmaAllocation &allocation,
            const std::vector<float> &input_data) {
  std::cout << "input data:\n";
  for (size_t i = 0; i < input_data.size(); ++i) {
    if (i % 64 == 0 && i != 0)
      std::cout << '\n';
    std::cout << input_data[i];
  }
  std::cout << '\n';

  // -------

  const VkCommandBufferAllocateInfo alloc_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = data.command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };

  if (init.disp.allocateCommandBuffers(&alloc_info, &data.command_buffer) !=
      VK_SUCCESS) {
    std::cout << "failed to allocate command buffers\n";
    return -1;
  }

  // ------- RECORD COMMAND BUFFER --------
  constexpr VkCommandBufferBeginInfo begin_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
  };
  init.disp.beginCommandBuffer(data.command_buffer, &begin_info);

  init.disp.cmdBindPipeline(data.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            data.compute_pipeline);
  init.disp.cmdBindDescriptorSets(
      data.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
      data.compute_pipeline_layout, 0, 1, &data.descriptor_set, 0, nullptr);

  const auto group_count_x =
      static_cast<uint32_t>(input_data.size() / ComputeShaderProcessUnit());
  init.disp.cmdDispatch(data.command_buffer, group_count_x, 1, 1);

  init.disp.endCommandBuffer(data.command_buffer);

  // -------

  const VkSubmitInfo submit_info{
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = 0,
      .commandBufferCount = 1,
      .pCommandBuffers = &data.command_buffer,
      .signalSemaphoreCount = 0,
  };

  if (vkQueueSubmit(data.compute_queue, 1, &submit_info, VK_NULL_HANDLE) !=
      VK_SUCCESS) {
    std::cout << "failed to submit queue\n";
    return -1;
  }

  // wait the calculation to finish
  if (vkQueueWaitIdle(data.compute_queue) != VK_SUCCESS)
    throw std::runtime_error("failed to wait queue idle!");

  std::vector<float> output_data(kN);

  void *mapped_data;
  vmaMapMemory(allocator, allocation, &mapped_data);
  memcpy(output_data.data(), mapped_data, kN * sizeof(float));
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

  const std::vector h_data(kN, 1.0f);

  // ------------------ INITIALIZATION ------------------
  Init init;
  ComputeData compute_data;

  // setting up vulkan
  VkCheck(device_initialization(init));
  VkCheck(get_queues(init, compute_data));
  vma_initialization(init);

  VkBuffer buffer;
  constexpr VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = kN * sizeof(float),
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .queueFamilyIndexCount = 0,
      .pQueueFamilyIndices = nullptr,
  };

  constexpr VmaAllocationCreateInfo alloc_info{
      .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
      .requiredFlags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
  };

  VmaAllocation allocation;
  vmaCreateBuffer(allocator, &buffer_info, &alloc_info, &buffer, &allocation,
                  nullptr);

  void *mapped_memory = nullptr;
  vmaMapMemory(allocator, allocation, &mapped_memory);
  memcpy(mapped_memory, h_data.data(), kN * sizeof(float));
  vmaUnmapMemory(allocator, allocation);

  VkCheck(create_descriptor_set_layout_v2(init, compute_data));
  VkCheck(create_compute_pipeline(init, compute_data));

  VkCheck(create_descriptor_pool(init, compute_data));
  VkCheck(create_descriptor_set(init, compute_data, buffer));

  VkCheck(create_command_pool(init, compute_data));

  execute(init, compute_data, buffer, allocation, h_data);

  vmaDestroyBuffer(allocator, buffer, allocation);

  cleanup(init, compute_data);

  std::cout << "Exiting normally" << std::endl;
  return 0;
}
