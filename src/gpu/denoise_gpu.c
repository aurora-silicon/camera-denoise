// SPDX-License-Identifier: MIT
/* Synchronous Vulkan 1.1 compute backend. No window system or runtime compiler. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "../backend.h"
#include "shaders.h"

#define CACHE 16
#define PARAM_BYTES ((16 + 288) * sizeof(int32_t))
struct buffer { VkBuffer buf; VkDeviceMemory mem; VkDeviceSize size; void *map; bool coherent; };
struct imported { struct buffer b; dev_t dev; ino_t ino; };
struct push { int32_t w, h, sy, sc, oy, oc, strength, seed; };
struct gpu {
	int w, h;
	bool valid, dead, dma;
	float sigma;
	bool said_upload, said_import;
	VkInstance instance;
	VkPhysicalDevice physical;
	VkPhysicalDeviceMemoryProperties memory;
	VkDevice device;
	VkQueue queue;
	uint32_t family, max_range;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkDescriptorPool descriptors;
	VkDescriptorSet set;
	VkPipeline pipeline[10];
	VkCommandPool pool;
	VkCommandBuffer cmd;
	VkFence fence;
	// upload, history, scratch, output, parameters
	struct buffer b[5];
	struct imported imported[CACHE];
	unsigned next_import;
	PFN_vkGetMemoryFdPropertiesKHR fd_properties;
};

static void copy_frame(const struct cdn_frame *in, struct cdn_frame *out)
{
	for (int y = 0; y < in->height; y++)
		memcpy(out->y + (size_t)y*out->stride_y, in->y + (size_t)y*in->stride_y, (size_t)in->width);
	for (int y = 0; y < in->height/2; y++)
		memcpy(out->uv + (size_t)y*out->stride_uv, in->uv + (size_t)y*in->stride_uv, (size_t)in->width);
}

static int vkerror(VkResult r, const char *where)
{
	fprintf(stderr, "camera-denoise: gpu: %s: VkResult %d\n", where, r);
	return r == VK_ERROR_DEVICE_LOST ? -EIO : -ENOMEM;
}
#define TRY(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) return vkerror(r_, #call); } while (0)

static void free_buffer(struct gpu *s, struct buffer *b)
{
	if (b->map) vkUnmapMemory(s->device, b->mem);
	if (b->buf) vkDestroyBuffer(s->device, b->buf, NULL);
	if (b->mem) vkFreeMemory(s->device, b->mem, NULL);
	memset(b, 0, sizeof(*b));
}

static int memory_type(struct gpu *s, uint32_t bits, VkMemoryPropertyFlags required,
		       VkMemoryPropertyFlags preferred)
{
	int fallback = -1;
	for (uint32_t i = 0; i < s->memory.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = s->memory.memoryTypes[i].propertyFlags;
		if (!(bits & (1u << i)) || (f & required) != required) continue;
		if ((f & preferred) == preferred) return (int)i;
		fallback = (int)i;
	}
	return fallback;
}

static int make_buffer(struct gpu *s, struct buffer *b, VkDeviceSize size, bool host, int fd)
{
	VkExternalMemoryBufferCreateInfo external = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
	VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.pNext = fd >= 0 ? &external : NULL, .size = size,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
	TRY(vkCreateBuffer(s->device, &ci, NULL, &b->buf));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(s->device, b->buf, &req);
	uint32_t bits = req.memoryTypeBits;
	if (fd >= 0) {
		VkMemoryFdPropertiesKHR prop = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
		TRY(s->fd_properties(s->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &prop));
		bits &= prop.memoryTypeBits;
		// Never bind beyond the exporter allocation.
		if (req.size > size) return -ENOTSUP;
	}
	int mt = memory_type(s, bits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : 0,
		host ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (mt < 0) return -ENOTSUP;
	VkMemoryDedicatedAllocateInfo dedicated = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = b->buf };
	VkImportMemoryFdInfoKHR import = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		.pNext = &dedicated, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = -1 };
	if (fd >= 0 && (import.fd = fcntl(fd, F_DUPFD_CLOEXEC, 0)) < 0) return -errno;
	VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = fd >= 0 ? &import : NULL, .allocationSize = fd >= 0 ? size : req.size,
		.memoryTypeIndex = (uint32_t)mt };
	VkResult r = vkAllocateMemory(s->device, &alloc, NULL, &b->mem);
	if (r != VK_SUCCESS) {
		if (import.fd >= 0) close(import.fd); // Vulkan owns it only on success
		return vkerror(r, "allocate buffer");
	}
	TRY(vkBindBufferMemory(s->device, b->buf, b->mem, 0));
	b->size = size;
	b->coherent = s->memory.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	if (host) TRY(vkMapMemory(s->device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map));
	return 0;
}

static int cache_sync(struct gpu *s, struct buffer *b, bool invalidate)
{
	if (b->coherent) return 0;
	VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
		.memory = b->mem, .size = VK_WHOLE_SIZE };
	if (invalidate) TRY(vkInvalidateMappedMemoryRanges(s->device, 1, &range));
	else TRY(vkFlushMappedMemoryRanges(s->device, 1, &range));
	return 0;
}

static void gpu_destroy(void *p)
{
	struct gpu *s = p;
	if (!s) return;
	if (s->device) {
		vkDeviceWaitIdle(s->device);
		for (unsigned i = 0; i < CACHE; i++) free_buffer(s, &s->imported[i].b);
		for (unsigned i = 0; i < 5; i++) free_buffer(s, &s->b[i]);
		for (unsigned i = 0; i < 10; i++) if (s->pipeline[i]) vkDestroyPipeline(s->device, s->pipeline[i], NULL);
		if (s->fence) vkDestroyFence(s->device, s->fence, NULL);
		if (s->pool) vkDestroyCommandPool(s->device, s->pool, NULL);
		if (s->descriptors) vkDestroyDescriptorPool(s->device, s->descriptors, NULL);
		if (s->layout) vkDestroyPipelineLayout(s->device, s->layout, NULL);
		if (s->set_layout) vkDestroyDescriptorSetLayout(s->device, s->set_layout, NULL);
		vkDestroyDevice(s->device, NULL);
	}
	if (s->instance) vkDestroyInstance(s->instance, NULL);
	free(s);
}

static bool has_extension(const VkExtensionProperties *ext, uint32_t n, const char *name)
{
	for (uint32_t i = 0; i < n; i++) if (!strcmp(ext[i].extensionName, name)) return true;
	return false;
}

static int init(struct gpu *s)
{
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "camera-denoise", .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	TRY(vkCreateInstance(&ici, NULL, &s->instance));
	uint32_t count = 0;
	TRY(vkEnumeratePhysicalDevices(s->instance, &count, NULL));
	VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
	if (!devices || !count) { free(devices); return -ENODEV; }
	VkResult result = vkEnumeratePhysicalDevices(s->instance, &count, devices);
	if (result != VK_SUCCESS) { free(devices); return vkerror(result, "enumerate devices"); }
	int best = -1;
	for (uint32_t i = 0; i < count; i++) {
		VkPhysicalDeviceProperties prop;
		vkGetPhysicalDeviceProperties(devices[i], &prop);
		if (prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !getenv("CDN_GPU_ALLOW_SOFTWARE")) continue;
		if (prop.apiVersion < VK_API_VERSION_1_1 || prop.limits.maxPerStageDescriptorStorageBuffers < 6 || prop.limits.maxDescriptorSetStorageBuffers < 6 ||
		    (uint64_t)s->w*s->h > (uint64_t)prop.limits.maxComputeWorkGroupCount[0]*128 || prop.limits.maxComputeWorkGroupInvocations < 128 || prop.limits.maxComputeWorkGroupSize[0] < 128 ||
		    prop.limits.maxStorageBufferRange < (uint64_t)s->w*s->h*20) continue;
		uint32_t n = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &n, NULL);
		VkQueueFamilyProperties *q = calloc(n, sizeof(*q));
		if (!q) continue;
		vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &n, q);
		for (uint32_t j = 0; j < n; j++) if (q[j].queueFlags & VK_QUEUE_COMPUTE_BIT) {
			int score = prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3 :
				prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
			if (score > best) { s->physical = devices[i]; s->family = j; best = score; }
			break;
		}
		free(q);
	}
	free(devices);
	if (!s->physical) return -ENODEV;
	uint32_t ne = 0;
	TRY(vkEnumerateDeviceExtensionProperties(s->physical, NULL, &ne, NULL));
	VkExtensionProperties *ext = calloc(ne, sizeof(*ext));
	if (!ext) return -ENOMEM;
	result = vkEnumerateDeviceExtensionProperties(s->physical, NULL, &ne, ext);
	if (result != VK_SUCCESS) { free(ext); return vkerror(result, "enumerate extensions"); }
	const char *names[] = { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME };
	s->dma = has_extension(ext, ne, names[0]) && has_extension(ext, ne, names[1]) && has_extension(ext, ne, names[2]);
	free(ext);
	if (s->dma) {
		VkPhysicalDeviceExternalBufferInfo info = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
			.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
		VkExternalBufferProperties prop = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
		vkGetPhysicalDeviceExternalBufferProperties(s->physical, &info, &prop);
		s->dma = prop.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
	}
	float priority = 1;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = s->family, .queueCount = 1, .pQueuePriorities = &priority };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
		.enabledExtensionCount = s->dma ? 3 : 0, .ppEnabledExtensionNames = names };
	TRY(vkCreateDevice(s->physical, &dci, NULL, &s->device));
	vkGetDeviceQueue(s->device, s->family, 0, &s->queue);
	vkGetPhysicalDeviceMemoryProperties(s->physical, &s->memory);
	if (s->dma) s->fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(s->device, "vkGetMemoryFdPropertiesKHR");
	if (!s->fd_properties) s->dma = false;
	VkPhysicalDeviceProperties prop;
	vkGetPhysicalDeviceProperties(s->physical, &prop);
	s->max_range = prop.limits.maxStorageBufferRange;
	fprintf(stderr, "camera-denoise: gpu: %s, linear DMA-BUF import %s\n", prop.deviceName, s->dma ? "available" : "unavailable");
	VkDescriptorSetLayoutBinding bindings[6] = {0};
	for (unsigned i = 0; i < 6; i++) bindings[i] = (VkDescriptorSetLayoutBinding) {
		.binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
	VkDescriptorSetLayoutCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 6, .pBindings = bindings };
	TRY(vkCreateDescriptorSetLayout(s->device, &sci, NULL, &s->set_layout));
	VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct push) };
	VkPipelineLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &s->set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push };
	TRY(vkCreatePipelineLayout(s->device, &lci, NULL, &s->layout));
	VkDescriptorPoolSize ds = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 };
	VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ds };
	TRY(vkCreateDescriptorPool(s->device, &pci, NULL, &s->descriptors));
	VkDescriptorSetAllocateInfo sai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = s->descriptors, .descriptorSetCount = 1, .pSetLayouts = &s->set_layout };
	TRY(vkAllocateDescriptorSets(s->device, &sai, &s->set));
	for (int i = 0; i < 10; i++) {
		VkShaderModuleCreateInfo mci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = i == 9 ? sizeof(noise_spv) : sizeof(filter_spv),
			.pCode = i == 9 ? noise_spv : filter_spv };
		VkShaderModule module;
		TRY(vkCreateShaderModule(s->device, &mci, NULL, &module));
		VkSpecializationMapEntry entry = { 0, 0, sizeof(int) };
		VkSpecializationInfo spec = { 1, &entry, sizeof(i), &i };
		VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main",
				.pSpecializationInfo = i == 9 ? NULL : &spec }, .layout = s->layout };
		result = vkCreateComputePipelines(s->device, VK_NULL_HANDLE, 1, &ci, NULL, &s->pipeline[i]);
		vkDestroyShaderModule(s->device, module, NULL);
		if (result != VK_SUCCESS) return vkerror(result, "create compute pipeline");
	}
	VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = s->family };
	TRY(vkCreateCommandPool(s->device, &cpi, NULL, &s->pool));
	VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = s->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	TRY(vkAllocateCommandBuffers(s->device, &cai, &s->cmd));
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	TRY(vkCreateFence(s->device, &fci, NULL, &s->fence));
	size_t n = (size_t)s->w*s->h;
	const size_t sizes[] = { (n*3/2+3)&~(size_t)3, n*10, n*20, (n*3/2+3)&~(size_t)3, PARAM_BYTES };
	for (unsigned i = 0; i < 5; i++) {
		int rc = make_buffer(s, &s->b[i], sizes[i], i == 0 || i >= 3, -1);
		if (rc) return rc;
	}
	int32_t *par = s->b[4].map;
	memset(par, 0, PARAM_BYTES); par[0] = 32; par[1] = 16;
	return cache_sync(s, &s->b[4], false);
}

static void *gpu_create(int w, int h, int threads)
{
	(void)threads;
	if (w < 16 || h < 16 || (w&1) || (h&1) || w > 8192 || h > 8192) return NULL;
	struct gpu *s = calloc(1, sizeof(*s));
	if (!s) return NULL;
	s->w = w; s->h = h;
	if (init(s)) { gpu_destroy(s); return NULL; }
	return s;
}

static void descriptors(struct gpu *s, struct buffer *y, struct buffer *uv)
{
	struct buffer *buf[] = { y, uv, &s->b[1], &s->b[2], &s->b[3], &s->b[4] };
	VkDescriptorBufferInfo info[6];
	VkWriteDescriptorSet write[6];
	for (unsigned i = 0; i < 6; i++) {
		info[i] = (VkDescriptorBufferInfo){ buf[i]->buf, 0, buf[i]->size };
		write[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = s->set, .dstBinding = i, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &info[i] };
	}
	vkUpdateDescriptorSets(s->device, 6, write, 0, NULL);
}

static void barrier(struct gpu *s, VkPipelineStageFlags src, VkPipelineStageFlags dst,
		    VkAccessFlags reads, VkAccessFlags writes)
{
	VkMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = reads, .dstAccessMask = writes };
	vkCmdPipelineBarrier(s->cmd, src, dst, 0, 1, &b, 0, NULL, 0, NULL);
}

static void ownership(struct gpu *s, struct buffer *y, struct buffer *uv, bool acquire)
{
	VkBufferMemoryBarrier b[2];
	struct buffer *buf[] = { y, uv };
	unsigned n = y == uv ? 1 : 2;
	for (unsigned i = 0; i < n; i++) b[i] = (VkBufferMemoryBarrier) {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = acquire ? 0 : VK_ACCESS_SHADER_READ_BIT,
		.dstAccessMask = acquire ? VK_ACCESS_SHADER_READ_BIT : 0,
		.srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_FOREIGN_EXT : s->family,
		.dstQueueFamilyIndex = acquire ? s->family : VK_QUEUE_FAMILY_FOREIGN_EXT,
		.buffer = buf[i]->buf, .size = VK_WHOLE_SIZE };
	vkCmdPipelineBarrier(s->cmd, acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		acquire ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0, 0, NULL, n, b, 0, NULL);
}

static void dispatch(struct gpu *s, int pass, uint32_t groups)
{
	vkCmdBindPipeline(s->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->pipeline[pass]);
	vkCmdDispatch(s->cmd, groups, 1, 1);
	barrier(s, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

static int execute(struct gpu *s, struct buffer *y, struct buffer *uv, const struct push *pc,
		   struct cdn_frame *out, bool imported)
{
	descriptors(s, y, uv);
	TRY(vkResetCommandBuffer(s->cmd, 0));
	VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	TRY(vkBeginCommandBuffer(s->cmd, &begin));
	barrier(s, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
	if (imported) ownership(s, y, uv, true);
	vkCmdBindDescriptorSets(s->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->layout, 0, 1, &s->set, 0, NULL);
	vkCmdPushConstants(s->cmd, s->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(*pc), pc);
	uint32_t n = (uint32_t)s->w*s->h;
	if (!s->valid) dispatch(s, 0, (n+127)/128);
	else {
		dispatch(s, 9, 288); // block histograms
		dispatch(s, 8, 1);   // medians, noise smoothing and thresholds
		for (int pass = 1; pass <= 7; pass++) {
			uint32_t size = pass == 5 ? n/4 : pass == 7 ? (n/2+3)/4 :
				(pass == 4 || pass == 6) ? n/2 : n;
			dispatch(s, pass, (size+127)/128);
		}
	}
	if (imported) ownership(s, y, uv, false);
	barrier(s, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
		VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
	TRY(vkEndCommandBuffer(s->cmd));
	TRY(vkResetFences(s->device, 1, &s->fence));
	VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1, .pCommandBuffers = &s->cmd };
	TRY(vkQueueSubmit(s->queue, 1, &submit, s->fence));
	// Sleep on the fence, never spin. The caller retains the PipeWire buffer
	// until all GPU reads finish, so no release sync_file needs exporting.
	TRY(vkWaitForFences(s->device, 1, &s->fence, VK_TRUE, UINT64_MAX));
	int rc = cache_sync(s, &s->b[3], true);
	if (!rc) rc = cache_sync(s, &s->b[4], true);
	if (rc) return rc;
	struct cdn_frame result = { s->b[3].map, (uint8_t *)s->b[3].map+n,
		s->w, s->h, s->w, s->w };
	copy_frame(&result, out);
	if (s->valid) s->sigma = ((int32_t *)s->b[4].map)[0]*1.75f/16;
	s->valid = true;
	return 0;
}

static bool valid_output(struct gpu *s, struct cdn_frame *out)
{
	return !s->dead && out && out->y && out->uv && out->width == s->w && out->height == s->h &&
		out->stride_y >= s->w && out->stride_uv >= s->w;
}

static struct push parameters(struct gpu *s, float strength)
{
	if (!isfinite(strength)) strength = 0;
	if (strength < 0) strength = 0;
	if (strength > 1) strength = 1;
	return (struct push){ s->w, s->h, s->w, s->w, 0, s->w*s->h,
		(int32_t)(strength*256+0.5f), !s->valid };
}

static int gpu_process(void *p, const struct cdn_frame *in, struct cdn_frame *out,
		       const struct cdn_frame_info *info, float strength)
{
	struct gpu *s = p;
	(void)info;
	if (!valid_output(s, out) || !in || !in->y || !in->uv || in->width != s->w ||
	    in->height != s->h || in->stride_y < s->w || in->stride_uv < s->w) return -EINVAL;
	struct cdn_frame upload = { s->b[0].map, (uint8_t *)s->b[0].map+(size_t)s->w*s->h,
		s->w, s->h, s->w, s->w };
	copy_frame(in, &upload);
	int rc = cache_sync(s, &s->b[0], false);
	struct push pc = parameters(s, strength);
	if (!rc) rc = execute(s, &s->b[0], &s->b[0], &pc, out, false);
	if (!rc && !s->said_upload) {
		fprintf(stderr, "camera-denoise: gpu: using one upload for mapped NV12 input\n");
		s->said_upload = true;
	}
	if (rc) { s->valid = false; s->dead = true; }
	return rc;
}

static struct buffer *import_buffer(struct gpu *s, int fd, const struct buffer *pinned)
{
	struct stat st;
	if (fd < 0 || fstat(fd, &st)) return NULL;
	for (unsigned i = 0; i < CACHE; i++)
		if (s->imported[i].b.buf && s->imported[i].dev == st.st_dev && s->imported[i].ino == st.st_ino)
			return &s->imported[i].b;
	off_t size = lseek(fd, 0, SEEK_END);
	if (size <= 0 || size > INT_MAX || (uint64_t)size > s->max_range || (size & 3)) return NULL;
	// The previous synchronous call has finished. Retire only unreferenced imports.
	struct imported *imp;
	do { imp = &s->imported[s->next_import++ % CACHE]; } while (&imp->b == pinned);
	free_buffer(s, &imp->b);
	if (make_buffer(s, &imp->b, (VkDeviceSize)size, false, fd)) {
		free_buffer(s, &imp->b);
		return NULL;
	}
	imp->dev = st.st_dev; imp->ino = st.st_ino;
	return &imp->b;
}

static int wait_producer(int fd)
{
	// POLLIN waits for the implicit writer fence. PipeWire gives us exclusive
	// buffer ownership until we queue it back; no producer can start meanwhile.
	struct pollfd p = { .fd = fd, .events = POLLIN };
	int rc;
	do { rc = poll(&p, 1, 1000); } while (rc < 0 && errno == EINTR);
	return rc > 0 && (p.revents & POLLIN) && !(p.revents & (POLLERR | POLLNVAL)) ? 0 : -EIO;
}

static int gpu_dmabuf(void *p, const struct cdn_dmabuf *in, struct cdn_frame *out,
		      const struct cdn_frame_info *info, float strength)
{
	struct gpu *s = p;
	(void)info;
	if (!valid_output(s, out)) return -EINVAL;
	if (!s->dma || !in || in->y.fd < 0 || in->uv.fd < 0 || in->width != s->w || in->height != s->h ||
	    in->y.stride < (uint32_t)s->w || in->uv.stride < (uint32_t)s->w ||
	    in->y.offset > INT_MAX || in->uv.offset > INT_MAX ||
	    in->y.stride > INT_MAX / (uint32_t)s->h || in->uv.stride > INT_MAX / (uint32_t)s->h)
		return -ENOTSUP;
	struct buffer *y = import_buffer(s, in->y.fd, NULL);
	struct buffer *uv = in->uv.fd == in->y.fd ? y : import_buffer(s, in->uv.fd, y);
	if (!y || !uv) {
		// A camera pool uses one allocator. Avoid retrying unsupported imports
		// on every frame; a new backend/format will probe again.
		s->dma = false;
		fprintf(stderr, "camera-denoise: gpu: camera allocation import unsupported, using upload\n");
		return -ENOTSUP;
	}
	if ((uint64_t)in->y.offset+(uint64_t)in->y.stride*(s->h-1)+s->w > y->size ||
	    (uint64_t)in->uv.offset+(uint64_t)in->uv.stride*(s->h/2-1)+s->w > uv->size) return -ENOTSUP;
	int rc = wait_producer(in->y.fd);
	if (!rc && in->uv.fd != in->y.fd) rc = wait_producer(in->uv.fd);
	if (rc) return -EAGAIN; // leave history intact; do not read an unfinished frame
	struct push pc = parameters(s, strength);
	pc.sy = (int32_t)in->y.stride; pc.sc = (int32_t)in->uv.stride;
	pc.oy = (int32_t)in->y.offset; pc.oc = (int32_t)in->uv.offset;
	if (!rc) rc = execute(s, y, uv, &pc, out, true);
	if (!rc && !s->said_import) {
		fprintf(stderr, "camera-denoise: gpu: using imported linear NV12 DMA-BUFs (no upload)\n");
		s->said_import = true;
	}
	if (rc) { s->valid = false; s->dead = true; }
	return rc;
}

static void gpu_reset(void *p) { ((struct gpu *)p)->valid = false; }
static float gpu_sigma(void *p) { return ((struct gpu *)p)->sigma; }
static const struct cdn_backend_ops ops = {
	.abi = CDN_BACKEND_ABI, .name = "gpu", .create = gpu_create,
	.process = gpu_process, .reset = gpu_reset, .noise_sigma = gpu_sigma, .destroy = gpu_destroy };
static const struct cdn_backend_dmabuf_ops dma_ops = {
	.abi = CDN_DMABUF_ABI, .flags = CDN_BACKEND_SINGLE_UPLOAD, .process = gpu_dmabuf };
const struct cdn_backend_ops *cdn_backend_entry(void) { return &ops; }
const struct cdn_backend_dmabuf_ops *cdn_backend_dmabuf_entry(void) { return &dma_ops; }
