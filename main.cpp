#include "VkBootstrap.h"

#include <array>
#include <fstream>
#include <iostream>

struct Init {
  vkb::Instance instance;
  vkb::DispatchTable disp;
  vkb::Device device;
};

struct ComputeData {
  VkQueue compute_queue;

  VkDescriptorSetLayout compute_descriptor_set_layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet descriptor_set;

  VkPipelineLayout compute_pipeline_layout;
  VkPipeline compute_pipeline;

  VkCommandPool command_pool;
  std::vector<VkCommandBuffer> command_buffers; // 1?
};

std::vector<char> readFile(const std::string &filename) {
  std::ifstream file(filename, std::ios::ate | std::ios::binary);

  if (!file.is_open()) {
    throw std::runtime_error("failed to open file!");
  }

  size_t file_size = (size_t)file.tellg();
  std::vector<char> buffer(file_size);

  file.seekg(0);
  file.read(buffer.data(), static_cast<std::streamsize>(file_size));

  file.close();

  return buffer;
}

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

int create_compute_descriptor_set_layout(Init &init, ComputeData &data) {
  std::array<VkDescriptorSetLayoutBinding, 3> layoutBindings{};
  layoutBindings[0].binding = 0;
  layoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  layoutBindings[0].descriptorCount = 1;
  layoutBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  layoutBindings[1].binding = 1;
  layoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  layoutBindings[1].descriptorCount = 1;
  layoutBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  layoutBindings[2].binding = 2;
  layoutBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  layoutBindings[2].descriptorCount = 1;
  layoutBindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 3;
  layoutInfo.pBindings = layoutBindings.data();

  if (vkCreateDescriptorSetLayout(init.device, &layoutInfo, nullptr,
                                  &data.compute_descriptor_set_layout) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to create compute descriptor set layout!");
  }
  return 0;
}

int create_descriptor_pool(Init &init, ComputeData &data) {
  std::array<VkDescriptorPoolSize, 1> poolSizes{};
  poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  poolSizes[0].descriptorCount = 1;

  VkDescriptorPoolCreateInfo poolInfo = {};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 1;
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes = poolSizes.data();

  if (init.disp.createDescriptorPool(&poolInfo, nullptr,
                                     &data.descriptor_pool)) {
    std::cout << "failed to create descriptor pool\n";
    return -1;
  };

  return 0;
}

int create_compute_descriptor_sets(Init &init, ComputeData &data) { return 0; }

int create_compute_pipeline(Init &init, ComputeData &data) {
  // Load & Create Shader Modules (1/3)
  const auto compute_shader_code = readFile("shaders/compute.spv");
  VkShaderModule compute_module = createShaderModule(init, compute_shader_code);

  if (compute_module == VK_NULL_HANDLE) {
    std::cout << "failed to create shader module\n";
    return -1;
  }

  // Create a Pipeline Layout (2/3)
  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &data.compute_descriptor_set_layout;

  if (init.disp.createPipelineLayout(&pipeline_layout_info, nullptr,
                                     &data.compute_pipeline_layout) !=
      VK_SUCCESS) {
    std::cout << "failed to create pipeline layout\n";
    return -1;
  }

  // Pipeline itself (3/3)
  VkPipelineShaderStageCreateInfo computeShaderStageInfo = {};
  computeShaderStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  computeShaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  computeShaderStageInfo.module = compute_module;
  computeShaderStageInfo.pName = "main";

  VkComputePipelineCreateInfo pipelineInfo = {};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipelineInfo.stage = computeShaderStageInfo;
  pipelineInfo.layout = data.compute_pipeline_layout;

  if (init.disp.createComputePipelines(VK_NULL_HANDLE, 1, &pipelineInfo,
                                       nullptr,
                                       &data.compute_pipeline) != VK_SUCCESS) {
    std::cout << "failed to create compute pipeline\n";
    return -1;
  }

  init.disp.destroyShaderModule(compute_module, nullptr);
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

int create_command_buffers(Init &init, ComputeData &data) {
  data.command_buffers.resize(1);

  VkCommandBufferAllocateInfo allocInfo = {};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = data.command_pool;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = (uint32_t)data.command_buffers.size();

  if (init.disp.allocateCommandBuffers(
          &allocInfo, data.command_buffers.data()) != VK_SUCCESS) {
    std::cout << "failed to allocate command buffers\n";
    return -1;
  }

  for (size_t i = 0; i < data.command_buffers.size(); i++) {
    // TODO: record command buffer
  }

  return 0;
}

int create_sync_objects(Init &init, ComputeData &data) { return 0; }

void cleanup(Init &init, ComputeData &data) {
  init.disp.destroyDescriptorPool(data.descriptor_pool, nullptr);

  init.disp.destroyCommandPool(data.command_pool, nullptr);
  init.disp.destroyDescriptorSetLayout(data.compute_descriptor_set_layout,
                                       nullptr);

  init.disp.destroyPipeline(data.compute_pipeline, nullptr);
  init.disp.destroyPipelineLayout(data.compute_pipeline_layout, nullptr);

  vkb::destroy_device(init.device);
  vkb::destroy_instance(init.instance);
}

inline int vkCheck(int result) {
  if (result != 0) {
    return -1;
  }
  return 0;
}

int main() {
  Init init;
  ComputeData compute_data;

  // setting up vulkan
  if (vkCheck(device_initialization(init)) ||
      vkCheck(get_queues(init, compute_data)) ||
      vkCheck(create_compute_descriptor_set_layout(init, compute_data)) ||
      vkCheck(create_compute_pipeline(init, compute_data)) ||
      vkCheck(create_command_pool(init, compute_data)) ||
      vkCheck(create_descriptor_pool(init, compute_data)) ||
      vkCheck(create_compute_descriptor_sets(init, compute_data)) ||
      vkCheck(create_command_buffers(init, compute_data)) ||
      vkCheck(create_sync_objects(init, compute_data))) {
    return -1;
  }

  cleanup(init, compute_data);

  std::cout << "Exiting normally" << std::endl;
  return 0;
}