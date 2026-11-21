// SPDX-License-Identifier: MIT
/*
 * Apple effects' full-size work on the GPU: Vulkan 1.1 compute, synchronous,
 * buffers only (as src/gpu). Two passes over the frame per frame, on one
 * upload of it: the quarter-size sums Portrait's refine and blur start from
 * (quarter.comp), then the composite (Portrait blur + Studio Light,
 * fx.comp). The CPU keeps only the quarter-size work in between (the guided
 * filter, the box blurs, the light map). This replaces the per-pixel loops
 * over the whole frame, which were most of the CPU time (2026-11-21 on the
 * J815: refine 4-10 ms, blur 5-10 ms, light 5-8 ms per 1242 x 700 frame,
 * rising with heat).
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "gpu_fx.h"
#include "fx_shaders.h"

struct buf { VkBuffer b; VkDeviceMemory mem; void *map; VkDeviceSize size; bool coherent; };
struct push { int32_t w, h, stride, qw, qh, blur, light; float g; };

struct apple_gpu {
	int w, h, stride, qw, qh;
	VkInstance instance;
	VkPhysicalDevice physical;
	VkPhysicalDeviceMemoryProperties memory;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkPipeline pipe[2], pipe_q;
	VkCommandPool cmd_pool;
	VkCommandBuffer cmd;
	VkFence fence;
	struct buf frame, maps, lut, quarter;
	char name[256];
};

static int vkfail(VkResult r, const char *where)
{
	fprintf(stderr, "camera-effects: apple: gpu: %s: VkResult %d\n", where, r);
	return r == VK_ERROR_DEVICE_LOST ? -EIO : -ENOMEM;
}
#define TRY(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) return vkfail(r_, #call); } while (0)

static int make_buf(struct apple_gpu *g, struct buf *b, VkDeviceSize size)
{
	VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
	VkMemoryRequirements req;
	int best = -1;

	TRY(vkCreateBuffer(g->device, &ci, NULL, &b->b));
	vkGetBufferMemoryRequirements(g->device, b->b, &req);
	for (uint32_t i = 0; i < g->memory.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = g->memory.memoryTypes[i].propertyFlags;

		if (!(req.memoryTypeBits & (1u << i)) || !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
			continue;
		/* coherent + cached first (the CPU writes and reads every frame) */
		if ((f & (VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
		    (VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
			best = (int)i;
			break;
		}
		if (best < 0)
			best = (int)i;
	}
	if (best < 0)
		return -ENOTSUP;
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size, .memoryTypeIndex = (uint32_t)best };
	TRY(vkAllocateMemory(g->device, &ai, NULL, &b->mem));
	TRY(vkBindBufferMemory(g->device, b->b, b->mem, 0));
	TRY(vkMapMemory(g->device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map));
	b->size = size;
	b->coherent = g->memory.memoryTypes[best].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	return 0;
}

static int sync_buf(struct apple_gpu *g, struct buf *b, bool from_device)
{
	VkMappedMemoryRange r = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = b->mem,
		.size = VK_WHOLE_SIZE };

	if (b->coherent)
		return 0;
	if (from_device)
		TRY(vkInvalidateMappedMemoryRanges(g->device, 1, &r));
	else
		TRY(vkFlushMappedMemoryRanges(g->device, 1, &r));
	return 0;
}

static void free_buf(struct apple_gpu *g, struct buf *b)
{
	if (b->map)
		vkUnmapMemory(g->device, b->mem);
	if (b->b)
		vkDestroyBuffer(g->device, b->b, NULL);
	if (b->mem)
		vkFreeMemory(g->device, b->mem, NULL);
	memset(b, 0, sizeof(*b));
}

void apple_gpu_free(struct apple_gpu *g)
{
	if (!g)
		return;
	if (g->device) {
		vkDeviceWaitIdle(g->device);
		free_buf(g, &g->frame);
		free_buf(g, &g->maps);
		free_buf(g, &g->lut);
		free_buf(g, &g->quarter);
		for (int i = 0; i < 2; i++)
			if (g->pipe[i])
				vkDestroyPipeline(g->device, g->pipe[i], NULL);
		if (g->pipe_q)
			vkDestroyPipeline(g->device, g->pipe_q, NULL);
		if (g->fence)
			vkDestroyFence(g->device, g->fence, NULL);
		if (g->cmd_pool)
			vkDestroyCommandPool(g->device, g->cmd_pool, NULL);
		if (g->pool)
			vkDestroyDescriptorPool(g->device, g->pool, NULL);
		if (g->layout)
			vkDestroyPipelineLayout(g->device, g->layout, NULL);
		if (g->set_layout)
			vkDestroyDescriptorSetLayout(g->device, g->set_layout, NULL);
		vkDestroyDevice(g->device, NULL);
	}
	if (g->instance)
		vkDestroyInstance(g->instance, NULL);
	free(g);
}

static int pick_device(struct apple_gpu *g)
{
	uint32_t n = 0;
	VkPhysicalDevice *devs;
	int best = -1;

	TRY(vkEnumeratePhysicalDevices(g->instance, &n, NULL));
	if (!n)
		return -ENODEV;
	devs = calloc(n, sizeof(*devs));
	if (!devs)
		return -ENOMEM;
	if (vkEnumeratePhysicalDevices(g->instance, &n, devs) != VK_SUCCESS) {
		free(devs);
		return -ENODEV;
	}
	for (uint32_t i = 0; i < n; i++) {
		VkPhysicalDeviceProperties prop;
		uint32_t nq = 0;
		VkQueueFamilyProperties *q;

		vkGetPhysicalDeviceProperties(devs[i], &prop);
		/* a software rasterizer is slower than the CPU loops it replaces */
		if (prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !getenv("CDN_GPU_ALLOW_SOFTWARE"))
			continue;
		if (prop.apiVersion < VK_API_VERSION_1_1 ||
		    prop.limits.maxStorageBufferRange < (uint64_t)g->stride * g->h * 3 / 2)
			continue;
		vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, NULL);
		q = calloc(nq, sizeof(*q));
		if (!q)
			continue;
		vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, q);
		for (uint32_t j = 0; j < nq; j++)
			if (q[j].queueFlags & VK_QUEUE_COMPUTE_BIT) {
				int score = prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3 :
					    prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;

				if (score > best) {
					best = score;
					g->physical = devs[i];
					g->family = j;
					snprintf(g->name, sizeof(g->name), "%s", prop.deviceName);
				}
				break;
			}
		free(q);
	}
	free(devs);
	return g->physical ? 0 : -ENODEV;
}

static int init(struct apple_gpu *g)
{
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "camera-effects-apple", .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	float prio = 1;
	int rc;

	TRY(vkCreateInstance(&ici, NULL, &g->instance));
	rc = pick_device(g);
	if (rc)
		return rc;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = g->family, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	TRY(vkCreateDevice(g->physical, &dci, NULL, &g->device));
	vkGetDeviceQueue(g->device, g->family, 0, &g->queue);
	vkGetPhysicalDeviceMemoryProperties(g->physical, &g->memory);

	VkDescriptorSetLayoutBinding bind[4];
	for (unsigned i = 0; i < 4; i++)
		bind[i] = (VkDescriptorSetLayoutBinding) { .binding = i,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
	VkDescriptorSetLayoutCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 4, .pBindings = bind };
	TRY(vkCreateDescriptorSetLayout(g->device, &sci, NULL, &g->set_layout));
	VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct push) };
	VkPipelineLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &g->set_layout, .pushConstantRangeCount = 1,
		.pPushConstantRanges = &pr };
	TRY(vkCreatePipelineLayout(g->device, &lci, NULL, &g->layout));
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 };
	VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
	TRY(vkCreateDescriptorPool(g->device, &pci, NULL, &g->pool));
	VkDescriptorSetAllocateInfo sai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = g->pool, .descriptorSetCount = 1, .pSetLayouts = &g->set_layout };
	TRY(vkAllocateDescriptorSets(g->device, &sai, &g->set));

	VkShaderModuleCreateInfo mci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(fx_spv), .pCode = fx_spv };
	VkShaderModule mod;
	TRY(vkCreateShaderModule(g->device, &mci, NULL, &mod));
	for (int i = 0; i < 2; i++) {
		VkSpecializationMapEntry e = { 0, 0, sizeof(int) };
		VkSpecializationInfo spec = { 1, &e, sizeof(i), &i };
		VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main",
				   .pSpecializationInfo = &spec },
			.layout = g->layout };
		VkResult r = vkCreateComputePipelines(g->device, VK_NULL_HANDLE, 1, &ci, NULL, &g->pipe[i]);

		if (r != VK_SUCCESS) {
			vkDestroyShaderModule(g->device, mod, NULL);
			return vkfail(r, "create compute pipeline");
		}
	}
	vkDestroyShaderModule(g->device, mod, NULL);
	mci.codeSize = sizeof(quarter_spv);
	mci.pCode = quarter_spv;
	TRY(vkCreateShaderModule(g->device, &mci, NULL, &mod));
	{
		VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main" },
			.layout = g->layout };
		VkResult r = vkCreateComputePipelines(g->device, VK_NULL_HANDLE, 1, &ci, NULL, &g->pipe_q);

		vkDestroyShaderModule(g->device, mod, NULL);
		if (r != VK_SUCCESS)
			return vkfail(r, "create quarter pipeline");
	}

	VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g->family };
	TRY(vkCreateCommandPool(g->device, &cpi, NULL, &g->cmd_pool));
	VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = g->cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	TRY(vkAllocateCommandBuffers(g->device, &cai, &g->cmd));
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	TRY(vkCreateFence(g->device, &fci, NULL, &g->fence));

	rc = make_buf(g, &g->frame, (VkDeviceSize)g->stride * g->h * 3 / 2);
	if (!rc)
		rc = make_buf(g, &g->maps, (VkDeviceSize)g->qw * g->qh * 6 * sizeof(float));
	if (!rc)
		rc = make_buf(g, &g->lut, 64 * 256);
	if (!rc)
		rc = make_buf(g, &g->quarter, (VkDeviceSize)g->qw * g->qh * 4 * sizeof(float));
	if (rc)
		return rc;
	struct buf *b[4] = { &g->frame, &g->maps, &g->lut, &g->quarter };
	VkDescriptorBufferInfo info[4];
	VkWriteDescriptorSet w[4];
	for (int i = 0; i < 4; i++) {
		info[i] = (VkDescriptorBufferInfo) { b[i]->b, 0, VK_WHOLE_SIZE };
		w[i] = (VkWriteDescriptorSet) { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = g->set, .dstBinding = (uint32_t)i, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &info[i] };
	}
	vkUpdateDescriptorSets(g->device, 4, w, 0, NULL);
	return 0;
}

struct apple_gpu *apple_gpu_new(int width, int height, int qw, int qh)
{
	struct apple_gpu *g;

	if (width < 16 || height < 16 || (width & 1) || (height & 1) || width > 8192 || height > 8192 ||
	    qw < 1 || qh < 1)
		return NULL;
	g = calloc(1, sizeof(*g));
	if (!g)
		return NULL;
	g->w = width;
	g->h = height;
	g->stride = (width + 3) & ~3;
	g->qw = qw;
	g->qh = qh;
	if (init(g)) {
		apple_gpu_free(g);
		return NULL;
	}
	return g;
}

const char *apple_gpu_name(const struct apple_gpu *g)
{
	return g ? g->name : "none";
}

static void upload_frame(struct apple_gpu *g, const struct cdn_frame *f)
{
	uint8_t *fb = g->frame.map;

	for (int y = 0; y < g->h; y++)
		memcpy(fb + (size_t)y * g->stride, f->y + (size_t)y * f->stride_y, (size_t)g->w);
	for (int y = 0; y < g->h / 2; y++)
		memcpy(fb + (size_t)(g->h + y) * g->stride, f->uv + (size_t)y * f->stride_uv, (size_t)g->w);
}

/* One recorded pass, submitted and waited for. */
static int run(struct apple_gpu *g, VkPipeline pipe, uint32_t groups, const struct push *pc,
	       VkPipeline pipe2, uint32_t groups2)
{
	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	VkMemoryBarrier in = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
	VkMemoryBarrier out = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };

	TRY(vkResetCommandBuffer(g->cmd, 0));
	TRY(vkBeginCommandBuffer(g->cmd, &bi));
	vkCmdPipelineBarrier(g->cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
			     1, &in, 0, NULL, 0, NULL);
	vkCmdBindDescriptorSets(g->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->layout, 0, 1, &g->set, 0, NULL);
	vkCmdPushConstants(g->cmd, g->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(*pc), pc);
	vkCmdBindPipeline(g->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdDispatch(g->cmd, groups, 1, 1);
	/* luma and chroma touch disjoint words: no barrier between */
	if (pipe2) {
		vkCmdBindPipeline(g->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe2);
		vkCmdDispatch(g->cmd, groups2, 1, 1);
	}
	vkCmdPipelineBarrier(g->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
			     1, &out, 0, NULL, 0, NULL);
	TRY(vkEndCommandBuffer(g->cmd));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
		.pCommandBuffers = &g->cmd };
	TRY(vkResetFences(g->device, 1, &g->fence));
	TRY(vkQueueSubmit(g->queue, 1, &si, g->fence));
	TRY(vkWaitForFences(g->device, 1, &g->fence, VK_TRUE, 1000000000ull));
	return 0;
}

int apple_gpu_quarter(struct apple_gpu *g, const struct cdn_frame *f, struct apple_quarter *q)
{
	size_t qn;
	const float *qb;
	int rc;

	if (!g || !f || !q || f->width != g->w || f->height != g->h)
		return -EINVAL;
	qn = (size_t)g->qw * g->qh;
	upload_frame(g, f);
	if ((rc = sync_buf(g, &g->frame, false)))
		return rc;
	struct push pc = { g->w, g->h, g->stride, g->qw, g->qh, 0, 0, 0 };

	if ((rc = run(g, g->pipe_q, (uint32_t)((qn + 63) / 64), &pc, VK_NULL_HANDLE, 0)))
		return rc;
	if ((rc = sync_buf(g, &g->quarter, true)))
		return rc;
	qb = g->quarter.map;
	*q = (struct apple_quarter){ qb, qb + qn, qb + 2 * qn, qb + 3 * qn };
	return 0;
}

int apple_gpu_apply(struct apple_gpu *g, struct cdn_frame *f, const struct apple_fx_maps *m,
		    bool blur, bool light, bool loaded)
{
	size_t qn;
	uint8_t *fb;
	float *mp;
	int rc;

	if (!g || !f || !m || f->width != g->w || f->height != g->h || m->qw != g->qw || m->qh != g->qh)
		return -EINVAL;
	qn = (size_t)g->qw * g->qh;
	fb = g->frame.map;
	mp = g->maps.map;
	if (!blur && !light)
		return 0;
	/* loaded: apple_gpu_quarter() put this very frame there, unchanged since */
	if (!loaded)
		upload_frame(g, f);
	if (blur) {
		memcpy(mp, m->alpha, qn * sizeof(float));
		memcpy(mp + qn, m->by, qn * sizeof(float));
		memcpy(mp + 2 * qn, m->bu, qn * sizeof(float));
		memcpy(mp + 3 * qn, m->bv, qn * sizeof(float));
	}
	if (light) {
		memcpy(mp + 4 * qn, m->light, qn * sizeof(float));
		memcpy(mp + 5 * qn, m->bg, qn * sizeof(float));
		memcpy(g->lut.map, m->lut, 64 * 256);
	}
	if ((!loaded && (rc = sync_buf(g, &g->frame, false))) || (rc = sync_buf(g, &g->maps, false)) ||
	    (rc = sync_buf(g, &g->lut, false)))
		return rc;

	struct push pc = { g->w, g->h, g->stride, g->qw, g->qh, blur, light, m->g };
	uint32_t per_row = (uint32_t)(g->w + 3) / 4;

	if ((rc = run(g, g->pipe[0], (per_row * (uint32_t)g->h + 63) / 64, &pc,
		      g->pipe[1], (per_row * (uint32_t)(g->h / 2) + 63) / 64)))
		return rc;
	if ((rc = sync_buf(g, &g->frame, true)))
		return rc;
	for (int y = 0; y < g->h; y++)
		memcpy(f->y + (size_t)y * f->stride_y, fb + (size_t)y * g->stride, (size_t)g->w);
	for (int y = 0; y < g->h / 2; y++)
		memcpy(f->uv + (size_t)y * f->stride_uv, fb + (size_t)(g->h + y) * g->stride, (size_t)g->w);
	return 0;
}
