cmake_minimum_required(VERSION 3.16)
project(hello_vulkan LANGUAGES CXX)

add_subdirectory(vk-bootstrap)
# add_subdirectory(VulkanMemoryAllocator)

find_package(Vulkan REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

add_executable(VulkanTest main.cpp)
target_compile_features(VulkanTest PRIVATE cxx_std_20)

target_link_libraries(VulkanTest PRIVATE vk-bootstrap::vk-bootstrap Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)

