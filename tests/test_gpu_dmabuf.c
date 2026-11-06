// SPDX-License-Identifier: MIT
/* Real DMA-BUF transport test, using Vulkan-exported allocations as a camera
 * pool. Requires no camera, display server or system configuration changes. */
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "../src/backend.h"
#define VK(call) do { VkResult r=(call); if(r!=VK_SUCCESS) { fprintf(stderr,"%s: %d\n",#call,r); return 1; } } while(0)
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
struct context {
	VkInstance instance; VkPhysicalDevice physical; VkDevice dev; VkQueue queue;
	uint32_t family; VkPhysicalDeviceMemoryProperties memory;
	VkCommandPool pool; VkCommandBuffer cmd; VkFence fence;
	PFN_vkGetMemoryFdKHR get_fd;
};
struct surface { VkBuffer buf; VkDeviceMemory mem; void *map; VkDeviceSize size; int fd; int foreign; };
static int extension(const VkExtensionProperties *e, uint32_t n, const char *name)
{ for(uint32_t i=0;i<n;i++) if(!strcmp(e[i].extensionName,name)) return 1; return 0; }
static int setup(struct context *c)
{
	VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
	VK(vkCreateInstance(&ici,NULL,&c->instance));
	uint32_t n=0; VK(vkEnumeratePhysicalDevices(c->instance,&n,NULL));
	VkPhysicalDevice *dev=calloc(n,sizeof(*dev)); if(!dev || !n) { free(dev); return 77; }
	VK(vkEnumeratePhysicalDevices(c->instance,&n,dev));
	const char *names[]={VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
	int best=-1;
	for(uint32_t i=0;i<n;i++) {
		VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(dev[i],&p);
		if(p.apiVersion<VK_API_VERSION_1_1) continue;
		uint32_t ne=0; VK(vkEnumerateDeviceExtensionProperties(dev[i],NULL,&ne,NULL));
		VkExtensionProperties *e=calloc(ne,sizeof(*e)); CHECK(e);
		VK(vkEnumerateDeviceExtensionProperties(dev[i],NULL,&ne,e));
		int supported=extension(e,ne,names[0]) && extension(e,ne,names[1]) && extension(e,ne,names[2]); free(e);
		if(!supported) continue;
		VkPhysicalDeviceExternalBufferInfo info={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
			.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
		VkExternalBufferProperties ext={.sType=VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
		vkGetPhysicalDeviceExternalBufferProperties(dev[i],&info,&ext);
		if(!(ext.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) continue;
		uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(dev[i],&nq,NULL);
		VkQueueFamilyProperties *q=calloc(nq,sizeof(*q)); CHECK(q);
		vkGetPhysicalDeviceQueueFamilyProperties(dev[i],&nq,q);
		for(uint32_t j=0;j<nq;j++) if(q[j].queueFlags & VK_QUEUE_COMPUTE_BIT) {
			int score=p.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU?3:p.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU?2:1;
			if(score>best) { best=score; c->physical=dev[i]; c->family=j; }
			break;
		}
		free(q);
	}
	free(dev); if(!c->physical) return 77;
	float priority=1;
	VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=c->family,.queueCount=1,.pQueuePriorities=&priority};
	VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.enabledExtensionCount=3,.ppEnabledExtensionNames=names};
	VK(vkCreateDevice(c->physical,&dci,NULL,&c->dev));
	vkGetDeviceQueue(c->dev,c->family,0,&c->queue);
	vkGetPhysicalDeviceMemoryProperties(c->physical,&c->memory);
	c->get_fd=(PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(c->dev,"vkGetMemoryFdKHR"); CHECK(c->get_fd);
	VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=c->family};
	VK(vkCreateCommandPool(c->dev,&pci,NULL,&c->pool));
	VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=c->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
	VK(vkAllocateCommandBuffers(c->dev,&ai,&c->cmd));
	VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VK(vkCreateFence(c->dev,&fci,NULL,&c->fence));
	return 0;
}
static int make(struct context *c,struct surface *s,size_t size)
{
	size=(size+4095)&~(size_t)4095;
	VkExternalMemoryBufferCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
	VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.pNext=&external,.size=size,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
	VK(vkCreateBuffer(c->dev,&bci,NULL,&s->buf));
	VkMemoryRequirements req; vkGetBufferMemoryRequirements(c->dev,s->buf,&req);
	int mt=-1;
	for(uint32_t i=0;i<c->memory.memoryTypeCount;i++)
		if((req.memoryTypeBits&(1u<<i)) && (c->memory.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt=(int)i; break; }
	if(mt<0) return 77;
	VkMemoryDedicatedAllocateInfo dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.buffer=s->buf};
	VkExportMemoryAllocateInfo export={.sType=VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,.pNext=&dedicated,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
	VkMemoryAllocateInfo mai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&export,.allocationSize=req.size,.memoryTypeIndex=(uint32_t)mt};
	VK(vkAllocateMemory(c->dev,&mai,NULL,&s->mem)); VK(vkBindBufferMemory(c->dev,s->buf,s->mem,0));
	VK(vkMapMemory(c->dev,s->mem,0,VK_WHOLE_SIZE,0,&s->map)); s->size=req.size;
	VkMemoryGetFdInfoKHR fi={.sType=VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,.memory=s->mem,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
	VK(c->get_fd(c->dev,&fi,&s->fd));
	return 0;
}
static int transfer(struct context *c,struct surface *s,int foreign)
{
	VK(vkResetCommandBuffer(c->cmd,0));
	VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; VK(vkBeginCommandBuffer(c->cmd,&begin));
	VkBufferMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask=foreign?VK_ACCESS_HOST_WRITE_BIT:0,.dstAccessMask=foreign?0:VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex=foreign?c->family:VK_QUEUE_FAMILY_FOREIGN_EXT,
		.dstQueueFamilyIndex=foreign?VK_QUEUE_FAMILY_FOREIGN_EXT:c->family,.buffer=s->buf,.size=VK_WHOLE_SIZE};
	vkCmdPipelineBarrier(c->cmd,foreign?VK_PIPELINE_STAGE_HOST_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		foreign?VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&b,0,NULL);
	VK(vkEndCommandBuffer(c->cmd)); VK(vkResetFences(c->dev,1,&c->fence));
	VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&c->cmd};
	VK(vkQueueSubmit(c->queue,1,&submit,c->fence)); VK(vkWaitForFences(c->dev,1,&c->fence,VK_TRUE,UINT64_MAX));
	s->foreign=foreign; return 0;
}
static void free_surface(struct context *c,struct surface *s)
{
	if(s->map) vkUnmapMemory(c->dev,s->mem);
	if(s->fd>=0) close(s->fd);
	if(s->buf) vkDestroyBuffer(c->dev,s->buf,NULL);
	if(s->mem) vkFreeMemory(c->dev,s->mem,NULL);
}
static int run(struct context *c,const struct cdn_backend_ops *gpu,const struct cdn_backend_dmabuf_ops *dma,int split)
{
	enum {W=318,H=182,SY=344,SC=352,STRIDE=331};
	const uint32_t yo=4099, uo=split?8197:((yo+SY*H+4095)&~4095u)+7;
	struct surface pool[2][2]={0};
	for(int j=0;j<2;j++) for(int p=0;p<2;p++) pool[j][p].fd=-1;
	for(int j=0;j<2;j++) for(int p=0;p<(split?2:1);p++) {
		int rc=make(c,&pool[j][p],p?uo+SC*(H/2):split?yo+SY*H:uo+SC*(H/2));
		if(rc) {
			for(int k=0;k<2;k++) for(int q=0;q<(split?2:1);q++) free_surface(c,&pool[k][q]);
			return rc;
		}
	}
	const struct cdn_backend_ops *cpu=cdn_cpu_backend();
	void *a=cpu->create(W,H,1), *b=gpu->create(W,H,0); CHECK(a && b);
	size_t n=STRIDE*H, fs=n*3/2;
	uint8_t *oa=malloc(fs), *ob=malloc(fs); CHECK(oa && ob);
	struct cdn_frame fa={oa,oa+n,W,H,STRIDE,STRIDE}, fb={ob,ob+n,W,H,STRIDE,STRIDE};
	struct cdn_frame_info info={0}; uint32_t rng=4300;
	for(int frame=0;frame<24;frame++) {
		if(frame==12) { cpu->reset(a); gpu->reset(b); }
		struct surface *y=&pool[frame%2][0], *uv=split?&pool[frame%2][1]:y;
		if(y->foreign) CHECK(!transfer(c,y,0));
		if(uv!=y && uv->foreign) CHECK(!transfer(c,uv,0));
		memset(y->map,0xa5,(size_t)y->size); if(uv!=y) memset(uv->map,0xa5,(size_t)uv->size);
		struct cdn_frame fi={(uint8_t *)y->map+yo,(uint8_t *)uv->map+uo,W,H,SY,SC};
		for(int p=0;p<2;p++) for(int yy=0;yy<(p?H/2:H);yy++) for(int x=0;x<W;x++) {
			rng=rng*1664525u+1013904223u;
			int v=(p?128+12*(x&1):40+x/3+20*((yy/30)&1))+(int)((rng>>24)&31)-16;
			if(!p && x>frame*4 && x<frame*4+50 && yy>50 && yy<120) v=25;
			if(frame==9) v=(x&1)?255:0;
			(p?fi.uv:fi.y)[(size_t)yy*(p?SC:SY)+x]=(uint8_t)(v<0?0:v>255?255:v);
		}
		memset(oa,0xcd,fs); memset(ob,0xcd,fs);
		float strength=frame>=8 && frame<11?0:frame>=11 && frame<15?0.375f:1;
		CHECK(!cpu->process(a,&fi,&fa,&info,strength));
		CHECK(!transfer(c,y,1)); if(uv!=y) CHECK(!transfer(c,uv,1));
		struct cdn_dmabuf in={W,H,{y->fd,yo,SY},{uv->fd,uo,SC}};
		if(frame==5) { struct cdn_dmabuf bad=in; bad.y.offset=UINT32_MAX; CHECK(dma->process(b,&bad,&fb,&info,strength)==-ENOTSUP); }
		int rc=dma->process(b,&in,&fb,&info,strength);
		if(rc) { fprintf(stderr,"DMA-BUF mode %d frame %d returned %d\n",split,frame,rc); return 1; }
		if(memcmp(oa,ob,fs)) {
			size_t k=0; while(oa[k]==ob[k]) k++;
			fprintf(stderr,"DMA-BUF mode %d frame %d byte %zu: %u != %u\n",split,frame,k,oa[k],ob[k]); return 1;
		}
		CHECK(cpu->noise_sigma(a)==gpu->noise_sigma(b));
	}
	gpu->destroy(b); cpu->destroy(a); free(oa); free(ob);
	for(int j=0;j<2;j++) for(int p=0;p<(split?2:1);p++) free_surface(c,&pool[j][p]);
	printf("DMA-BUF %s fds: 24 frames bit-exact, offsets/strides, pool reuse and reset passed\n",split?"split":"shared");
	return 0;
}
int main(int argc,char **argv)
{
	CHECK(argc==2);
	struct context c={0}; int rc=setup(&c);
	if(!rc) {
		void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); CHECK(lib);
		cdn_backend_entry_fn entry=(cdn_backend_entry_fn)dlsym(lib,CDN_BACKEND_ENTRY);
		cdn_backend_dmabuf_entry_fn de=(cdn_backend_dmabuf_entry_fn)dlsym(lib,CDN_DMABUF_ENTRY); CHECK(entry && de);
		rc=run(&c,entry(),de(),0); if(!rc) rc=run(&c,entry(),de(),1);
		dlclose(lib);
	}
	if(c.dev) { vkDeviceWaitIdle(c.dev); vkDestroyFence(c.dev,c.fence,NULL); vkDestroyCommandPool(c.dev,c.pool,NULL); vkDestroyDevice(c.dev,NULL); }
	if(c.instance) vkDestroyInstance(c.instance,NULL);
	return rc==77 && getenv("CDN_TEST_REQUIRE_DMABUF")?1:rc;
}
