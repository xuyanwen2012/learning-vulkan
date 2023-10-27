CFLAGS = -std=c++20 -O2
LDFLAGS = -lvulkan

VulkanTest: main.cpp
	g++ $(CFLAGS) -o VulkanTest VkBootstrap.cpp main.cpp $(LDFLAGS)

.PHONY: test clean

test: VulkanTest
	./VulkanTest

clean:
	rm -f VulkanTest
