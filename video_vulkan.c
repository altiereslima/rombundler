#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "aspect.h"
#include "config.h"
#include "font.h"
#include "libretro_vulkan.h"
#include "menu.h"
#include "utils.h"
#include "video_vulkan.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

extern config g_cfg;

typedef struct {
	bool initialized;
	bool swapchain_dirty;
	bool frame_acquired;
	bool warned_software_upload;
	bool pending_image_valid;
	bool active_image_valid;
	bool device_created_by_core;
	bool fast_forward;

	GLFWwindow *window;

	struct retro_hw_render_callback hw;
	struct retro_game_geometry geom;
	float aspect_ratio;

	struct retro_hw_render_interface_vulkan iface;
	struct retro_hw_render_context_negotiation_interface_vulkan negotiation;
	bool negotiation_set;

	VkInstance instance;
	VkPhysicalDevice gpu;
	VkDevice device;
	VkQueue graphics_queue;
	uint32_t graphics_queue_family;
	VkSurfaceKHR surface;

	VkCommandPool command_pool;

	VkSwapchainKHR swapchain;
	VkFormat swapchain_format;
	VkExtent2D swapchain_extent;
	VkImage *swapchain_images;
	bool *swapchain_image_initialized;
	uint32_t swapchain_image_count;

	VkCommandBuffer *command_buffers;
	VkSemaphore *image_available;
	VkSemaphore *render_finished;
	VkFence *frame_fences;
	uint32_t *image_fence_slots;
	bool *sync_index_wait_ready;
	uint32_t next_frame_slot;
	uint32_t current_frame_slot;
	uint32_t current_image_index;

	struct retro_vulkan_image pending_image;
	struct retro_vulkan_image active_image;
	uint32_t pending_width;
	uint32_t pending_height;
	uint32_t active_width;
	uint32_t active_height;
	uint32_t src_queue_family;

	VkSemaphore *wait_semaphores;
	VkPipelineStageFlags *wait_stages;
	uint32_t wait_count;
	uint32_t wait_capacity;

	VkCommandBuffer *core_commands;
	uint32_t core_command_count;
	uint32_t core_command_capacity;

	VkSemaphore signal_semaphore;
} video_vulkan_state;

static video_vulkan_state g_vk = {0};
static bool g_vk_logged_ignored_image_waits = false;

#ifdef _WIN32
static CRITICAL_SECTION g_vk_queue_lock;
static bool g_vk_queue_lock_initialized = false;
#else
static pthread_mutex_t g_vk_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_vk_queue_lock_initialized = false;
#endif

#ifdef _WIN32
static void video_vulkan_log_problematic_overlay_hooks(void)
{
	static bool logged = false;
	HMODULE rtss_hooks = NULL;
	HMODULE rtss_vk = NULL;

	if (logged)
		return;
	logged = true;

	rtss_hooks = GetModuleHandleA("RTSSHooks64.dll");
	rtss_vk = GetModuleHandleA("RTSSVkLayer64.dll");
	if (rtss_hooks || rtss_vk) {
		log_printf("video",
			"RTSS/MSI Afterburner hooks detected in-process; some Vulkan cores may stall until the overlay is disabled");
	}
}
#else
static void video_vulkan_log_problematic_overlay_hooks(void)
{
}
#endif

static void video_vulkan_queue_lock_init(void)
{
	if (g_vk_queue_lock_initialized)
		return;

#ifdef _WIN32
	InitializeCriticalSection(&g_vk_queue_lock);
#else
	pthread_mutex_init(&g_vk_queue_lock, NULL);
#endif
	g_vk_queue_lock_initialized = true;
}

static void video_vulkan_queue_lock_deinit(void)
{
	if (!g_vk_queue_lock_initialized)
		return;

#ifdef _WIN32
	DeleteCriticalSection(&g_vk_queue_lock);
#else
	pthread_mutex_destroy(&g_vk_queue_lock);
#endif
	g_vk_queue_lock_initialized = false;
}

static void video_vulkan_lock_queue(void *handle)
{
	(void)handle;

	if (!g_vk_queue_lock_initialized)
		return;

#ifdef _WIN32
	EnterCriticalSection(&g_vk_queue_lock);
#else
	pthread_mutex_lock(&g_vk_queue_lock);
#endif
}

static void video_vulkan_unlock_queue(void *handle)
{
	(void)handle;

	if (!g_vk_queue_lock_initialized)
		return;

#ifdef _WIN32
	LeaveCriticalSection(&g_vk_queue_lock);
#else
	pthread_mutex_unlock(&g_vk_queue_lock);
#endif
}

static void *video_vulkan_realloc(void *ptr, size_t size)
{
	void *out = realloc(ptr, size);
	if (size && !out)
		die("Out of memory while growing Vulkan state.");
	return out;
}

static bool video_vulkan_wait_for_fence_with_timeout(const char *reason,
	VkFence fence, uint32_t sync_index, uint32_t slot, uint64_t timeout_ns)
{
	VkResult res;

	if (fence == VK_NULL_HANDLE)
		return true;

	res = vkWaitForFences(g_vk.device, 1, &fence, VK_TRUE, timeout_ns);
	if (res == VK_SUCCESS)
		return true;

	if (res == VK_TIMEOUT) {
		log_printf("video",
			"%s fence wait timed out sync=%u slot=%u timeout_ms=%llu",
			reason ? reason : "vulkan",
			sync_index,
			slot,
			(unsigned long long)(timeout_ns / 1000000ULL));
		return false;
	}

	log_printf("video",
		"%s fence wait failed sync=%u slot=%u result=%d",
		reason ? reason : "vulkan",
		sync_index,
		slot,
		(int)res);
	return false;
}

static bool video_vulkan_string_exists(const char *const *values, unsigned count, const char *needle)
{
	unsigned i;

	if (!needle)
		return false;

	for (i = 0; i < count; i++) {
		if (values[i] && strcmp(values[i], needle) == 0)
			return true;
	}

	return false;
}

static void video_vulkan_append_unique_name(const char ***names, unsigned *count, const char *name)
{
	const char **out;

	if (!names || !count || !name)
		return;

	if (video_vulkan_string_exists(*names, *count, name))
		return;

	out = (const char **)video_vulkan_realloc((void *)*names,
		sizeof(const char *) * (*count + 1));
	out[*count] = name;
	*names = out;
	(*count)++;
}

static bool video_vulkan_append_required_instance_extensions(const char ***names, unsigned *count)
{
	uint32_t glfw_count = 0;
	const char **glfw_exts = glfwGetRequiredInstanceExtensions(&glfw_count);
	uint32_t i;

	if (!glfw_exts || !glfw_count) {
		log_printf("video", "glfwGetRequiredInstanceExtensions failed");
		return false;
	}

	for (i = 0; i < glfw_count; i++)
		video_vulkan_append_unique_name(names, count, glfw_exts[i]);

	return true;
}

static void video_vulkan_append_required_device_extensions(const char ***names, unsigned *count)
{
	video_vulkan_append_unique_name(names, count, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
}

static VkImageSubresourceRange video_vulkan_source_range(void)
{
	VkImageSubresourceRange range = g_vk.active_image.create_info.subresourceRange;

	if (!range.aspectMask)
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	if (!range.levelCount)
		range.levelCount = 1;
	if (!range.layerCount)
		range.layerCount = 1;

	return range;
}

static float video_vulkan_resolve_aspect_ratio(const struct retro_game_geometry *geom)
{
	if (geom && geom->aspect_ratio > 0.0f)
		return geom->aspect_ratio;
	if (geom && geom->base_width > 0 && geom->base_height > 0)
		return (float)geom->base_width / (float)geom->base_height;
	return 4.0f / 3.0f;
}

static void video_vulkan_reset_submission_state(void)
{
	g_vk.pending_image_valid = false;
	g_vk.wait_count = 0;
	g_vk.core_command_count = 0;
	g_vk.signal_semaphore = VK_NULL_HANDLE;
	g_vk.src_queue_family = VK_QUEUE_FAMILY_IGNORED;
}

static void video_vulkan_set_signal_semaphore(void *handle, VkSemaphore semaphore)
{
	(void)handle;
	g_vk.signal_semaphore = semaphore;
}

static uint32_t video_vulkan_get_sync_index(void *handle)
{
	(void)handle;
	return g_vk.current_image_index;
}

static uint32_t video_vulkan_get_sync_index_mask(void *handle)
{
	(void)handle;
	if (!g_vk.swapchain_image_count)
		return 0;
	if (g_vk.swapchain_image_count >= 32)
		return UINT32_MAX;
	return (1u << g_vk.swapchain_image_count) - 1u;
}

static void video_vulkan_wait_sync_index(void *handle)
{
	uint32_t slot;

	(void)handle;

	if (!g_vk.initialized || !g_vk.frame_fences || !g_vk.image_fence_slots ||
	    g_vk.current_image_index >= g_vk.swapchain_image_count)
		return;

	if (g_vk.sync_index_wait_ready &&
	    g_vk.sync_index_wait_ready[g_vk.current_image_index])
		return;

	slot = g_vk.image_fence_slots[g_vk.current_image_index];
	if (slot == UINT32_MAX || slot >= g_vk.swapchain_image_count)
		return;

	if (!video_vulkan_wait_for_fence_with_timeout("wait_sync_index",
		g_vk.frame_fences[slot],
		g_vk.current_image_index,
		slot,
		1000000000ULL)) {
		g_vk.image_fence_slots[g_vk.current_image_index] = UINT32_MAX;
	}
}

static void video_vulkan_set_command_buffers(void *handle, uint32_t num_cmd,
	const VkCommandBuffer *cmd)
{
	VkCommandBuffer *out;

	(void)handle;

	if (!num_cmd || !cmd) {
		g_vk.core_command_count = 0;
		return;
	}

	if (num_cmd > g_vk.core_command_capacity) {
		out = (VkCommandBuffer *)video_vulkan_realloc(g_vk.core_commands,
			sizeof(VkCommandBuffer) * num_cmd);
		g_vk.core_commands = out;
		g_vk.core_command_capacity = num_cmd;
	}

	memcpy(g_vk.core_commands, cmd, sizeof(VkCommandBuffer) * num_cmd);
	g_vk.core_command_count = num_cmd;
}

static void video_vulkan_set_image(void *handle,
	const struct retro_vulkan_image *image,
	uint32_t num_semaphores,
	const VkSemaphore *semaphores,
	uint32_t src_queue_family)
{
	VkSemaphore *new_semaphores;
	VkPipelineStageFlags *new_stages;
	uint32_t i;

	(void)handle;

	g_vk.pending_image_valid = false;
	g_vk.wait_count = 0;
	g_vk.src_queue_family = VK_QUEUE_FAMILY_IGNORED;

	if (!image)
		return;

	g_vk.pending_image = *image;
	g_vk.pending_image.create_info.pNext = NULL;
	g_vk.pending_image_valid = true;
	g_vk.src_queue_family = src_queue_family;

	if (!num_semaphores || !semaphores)
		return;

	if (num_semaphores > g_vk.wait_capacity) {
		new_semaphores = (VkSemaphore *)video_vulkan_realloc(
			g_vk.wait_semaphores, sizeof(VkSemaphore) * num_semaphores);
		new_stages = (VkPipelineStageFlags *)video_vulkan_realloc(
			g_vk.wait_stages, sizeof(VkPipelineStageFlags) * num_semaphores);
		g_vk.wait_semaphores = new_semaphores;
		g_vk.wait_stages = new_stages;
		g_vk.wait_capacity = num_semaphores;
	}

	for (i = 0; i < num_semaphores; i++) {
		g_vk.wait_semaphores[i] = semaphores[i];
		g_vk.wait_stages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}

	g_vk.wait_count = num_semaphores;
}

static VkInstance video_vulkan_create_instance_wrapper(void *opaque,
	const VkInstanceCreateInfo *create_info)
{
	VkInstanceCreateInfo info = *create_info;
	const char **extensions = NULL;
	unsigned ext_count = 0;
	unsigned i;
	VkInstance instance = VK_NULL_HANDLE;
	VkResult res;

	(void)opaque;

	for (i = 0; i < info.enabledExtensionCount; i++)
		video_vulkan_append_unique_name(&extensions, &ext_count,
			info.ppEnabledExtensionNames[i]);

	if (!video_vulkan_append_required_instance_extensions(&extensions, &ext_count)) {
		free((void *)extensions);
		return VK_NULL_HANDLE;
	}

	info.enabledExtensionCount = ext_count;
	info.ppEnabledExtensionNames = extensions;

	res = vkCreateInstance(&info, NULL, &instance);
	if (res != VK_SUCCESS) {
		log_printf("video", "vkCreateInstance failed (%d)", (int)res);
		instance = VK_NULL_HANDLE;
	}

	free((void *)extensions);
	return instance;
}

static VkDevice video_vulkan_create_device_wrapper(VkPhysicalDevice gpu, void *opaque,
	const VkDeviceCreateInfo *create_info)
{
	VkDeviceCreateInfo info = *create_info;
	const char **extensions = NULL;
	unsigned ext_count = 0;
	unsigned i;
	VkDevice device = VK_NULL_HANDLE;
	VkResult res;

	(void)opaque;

	for (i = 0; i < info.enabledExtensionCount; i++)
		video_vulkan_append_unique_name(&extensions, &ext_count,
			info.ppEnabledExtensionNames[i]);

	video_vulkan_append_required_device_extensions(&extensions, &ext_count);

	info.enabledExtensionCount = ext_count;
	info.ppEnabledExtensionNames = extensions;

	res = vkCreateDevice(gpu, &info, NULL, &device);
	if (res != VK_SUCCESS) {
		log_printf("video", "vkCreateDevice failed (%d)", (int)res);
		device = VK_NULL_HANDLE;
	}

	free((void *)extensions);
	return device;
}

static void video_vulkan_fill_application_info(VkApplicationInfo *app)
{
	PFN_vkEnumerateInstanceVersion enumerate_instance_version;
	uint32_t supported_api = VK_API_VERSION_1_0;

	memset(app, 0, sizeof(*app));
	app->sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app->pApplicationName = g_cfg.title ? g_cfg.title : "ROMBundler";
	app->pEngineName = "ROMBundler";
	app->apiVersion = VK_API_VERSION_1_0;

	if (g_vk.negotiation_set && g_vk.negotiation.get_application_info) {
		const VkApplicationInfo *core_app = g_vk.negotiation.get_application_info();
		if (core_app)
			*app = *core_app;
	}

	enumerate_instance_version = (PFN_vkEnumerateInstanceVersion)
		vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
	if (enumerate_instance_version &&
	    enumerate_instance_version(&supported_api) == VK_SUCCESS &&
	    supported_api >= VK_API_VERSION_1_1 &&
	    app->apiVersion < VK_API_VERSION_1_1)
		app->apiVersion = VK_API_VERSION_1_1;
}

static bool video_vulkan_create_instance(void)
{
	VkApplicationInfo app;

	video_vulkan_fill_application_info(&app);

	if (g_vk.negotiation_set &&
	    g_vk.negotiation.interface_version >= 2 &&
	    g_vk.negotiation.create_instance) {
		g_vk.instance = g_vk.negotiation.create_instance(
			vkGetInstanceProcAddr,
			&app,
			video_vulkan_create_instance_wrapper,
			&g_vk);
		if (g_vk.instance != VK_NULL_HANDLE)
			return true;

		log_printf("video", "core create_instance returned VK_NULL_HANDLE, falling back to frontend instance creation");
	}

	g_vk.instance = video_vulkan_create_instance_wrapper(&g_vk,
		&(VkInstanceCreateInfo) {
			.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			.pApplicationInfo = &app,
		});

	return g_vk.instance != VK_NULL_HANDLE;
}

static bool video_vulkan_create_surface(void)
{
	VkResult res;

	if (g_vk.surface != VK_NULL_HANDLE)
		return true;

	res = glfwCreateWindowSurface(g_vk.instance, g_vk.window, NULL, &g_vk.surface);
	if (res != VK_SUCCESS) {
		log_printf("video", "glfwCreateWindowSurface failed (%d)", (int)res);
		return false;
	}

	return true;
}

static bool video_vulkan_pick_physical_device(VkPhysicalDevice preferred,
	uint32_t *queue_family)
{
	VkPhysicalDevice *devices = NULL;
	uint32_t device_count = 0;
	uint32_t i;

	if (!queue_family)
		return false;

	if (vkEnumeratePhysicalDevices(g_vk.instance, &device_count, NULL) != VK_SUCCESS ||
	    !device_count) {
		log_printf("video", "No Vulkan physical devices available");
		return false;
	}

	devices = (VkPhysicalDevice *)malloc(sizeof(VkPhysicalDevice) * device_count);
	if (!devices)
		die("Out of memory while enumerating Vulkan devices.");

	if (vkEnumeratePhysicalDevices(g_vk.instance, &device_count, devices) != VK_SUCCESS) {
		free(devices);
		log_printf("video", "vkEnumeratePhysicalDevices failed");
		return false;
	}

	for (i = 0; i < device_count; i++) {
		VkPhysicalDevice gpu = devices[i];
		VkQueueFamilyProperties *families = NULL;
		uint32_t family_count = 0;
		uint32_t j;

		if (preferred != VK_NULL_HANDLE && gpu != preferred)
			continue;

		vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, NULL);
		if (!family_count)
			continue;

		families = (VkQueueFamilyProperties *)malloc(
			sizeof(VkQueueFamilyProperties) * family_count);
		if (!families)
			die("Out of memory while enumerating Vulkan queue families.");

		vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, families);

		for (j = 0; j < family_count; j++) {
			VkBool32 present_supported = VK_FALSE;
			bool graphics_supported =
				(families[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;

			if (!graphics_supported)
				continue;

			if (vkGetPhysicalDeviceSurfaceSupportKHR(gpu, j, g_vk.surface,
			    &present_supported) != VK_SUCCESS)
				continue;

			if (!present_supported)
				continue;

			*queue_family = j;
			g_vk.gpu = gpu;
			free(families);
			free(devices);
			return true;
		}

		free(families);
	}

	free(devices);
	return false;
}

static bool video_vulkan_create_device_default(void)
{
	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info;
	VkDeviceCreateInfo device_info;
	const char **extensions = NULL;
	unsigned extension_count = 0;
	VkResult res;

	memset(&queue_info, 0, sizeof(queue_info));
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = g_vk.graphics_queue_family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;

	memset(&device_info, 0, sizeof(device_info));
	device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;

	video_vulkan_append_required_device_extensions(&extensions, &extension_count);
	device_info.enabledExtensionCount = extension_count;
	device_info.ppEnabledExtensionNames = extensions;

	res = vkCreateDevice(g_vk.gpu, &device_info, NULL, &g_vk.device);
	free((void *)extensions);

	if (res != VK_SUCCESS) {
		log_printf("video", "vkCreateDevice default path failed (%d)", (int)res);
		return false;
	}

	vkGetDeviceQueue(g_vk.device, g_vk.graphics_queue_family, 0, &g_vk.graphics_queue);
	g_vk.device_created_by_core = false;
	return true;
}

static bool video_vulkan_create_device_from_core(void)
{
	struct retro_vulkan_context context;
	bool ok = false;
	VkBool32 present_supported = VK_FALSE;

	if (!g_vk.negotiation_set ||
	    g_vk.negotiation.interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN ||
	    g_vk.negotiation.interface_version == 0)
		return false;

	memset(&context, 0, sizeof(context));

	if (g_vk.negotiation.interface_version >= 2 && g_vk.negotiation.create_device2) {
		ok = g_vk.negotiation.create_device2(&context,
			g_vk.instance,
			g_vk.gpu,
			g_vk.surface,
			vkGetInstanceProcAddr,
			video_vulkan_create_device_wrapper,
			&g_vk);

		if (!ok && g_vk.gpu != VK_NULL_HANDLE) {
			memset(&context, 0, sizeof(context));
			ok = g_vk.negotiation.create_device2(&context,
				g_vk.instance,
				VK_NULL_HANDLE,
				g_vk.surface,
				vkGetInstanceProcAddr,
				video_vulkan_create_device_wrapper,
				&g_vk);
		}
	} else if (g_vk.negotiation.create_device) {
		static const char *required_extensions[] = {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME
		};
		VkPhysicalDeviceFeatures required_features;
		memset(&required_features, 0, sizeof(required_features));

		ok = g_vk.negotiation.create_device(&context,
			g_vk.instance,
			g_vk.gpu,
			g_vk.surface,
			vkGetInstanceProcAddr,
			required_extensions,
			1,
			NULL,
			0,
			&required_features);

		if (!ok && g_vk.gpu != VK_NULL_HANDLE) {
			memset(&context, 0, sizeof(context));
			ok = g_vk.negotiation.create_device(&context,
				g_vk.instance,
				VK_NULL_HANDLE,
				g_vk.surface,
				vkGetInstanceProcAddr,
				required_extensions,
				1,
				NULL,
				0,
				&required_features);
		}
	}

	if (!ok)
		return false;

	if (context.device == VK_NULL_HANDLE || context.queue == VK_NULL_HANDLE || context.gpu == VK_NULL_HANDLE) {
		log_printf("video", "core Vulkan negotiation succeeded without returning device/queue/gpu");
		return false;
	}

	if (vkGetPhysicalDeviceSurfaceSupportKHR(context.gpu, context.queue_family_index,
	    g_vk.surface, &present_supported) != VK_SUCCESS || !present_supported) {
		log_printf("video", "core Vulkan queue does not support presenting to the frontend surface");
		return false;
	}

	if (context.presentation_queue != VK_NULL_HANDLE &&
	    context.presentation_queue != context.queue) {
		log_printf("video", "separate presentation queue is not supported by this frontend yet");
		return false;
	}

	g_vk.gpu = context.gpu;
	g_vk.device = context.device;
	g_vk.graphics_queue = context.queue;
	g_vk.graphics_queue_family = context.queue_family_index;
	g_vk.device_created_by_core = true;
	return true;
}

static bool video_vulkan_create_device(void)
{
	uint32_t queue_family = 0;

	if (!video_vulkan_pick_physical_device(VK_NULL_HANDLE, &queue_family)) {
		log_printf("video", "failed to find a Vulkan GPU/queue family with graphics + present support");
		return false;
	}

	g_vk.graphics_queue_family = queue_family;

	if (video_vulkan_create_device_from_core())
		return true;

	return video_vulkan_create_device_default();
}

static bool video_vulkan_create_command_pool(void)
{
	VkCommandPoolCreateInfo info;
	VkResult res;

	if (g_vk.command_pool != VK_NULL_HANDLE)
		return true;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	info.queueFamilyIndex = g_vk.graphics_queue_family;
	info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	res = vkCreateCommandPool(g_vk.device, &info, NULL, &g_vk.command_pool);
	if (res != VK_SUCCESS) {
		log_printf("video", "vkCreateCommandPool failed (%d)", (int)res);
		return false;
	}

	return true;
}

static VkSurfaceFormatKHR video_vulkan_choose_surface_format(
	const VkSurfaceFormatKHR *formats, uint32_t count)
{
	VkSurfaceFormatKHR chosen;
	uint32_t i;

	chosen = formats[0];

	for (i = 0; i < count; i++) {
		if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
		    formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			return formats[i];
	}

	for (i = 0; i < count; i++) {
		if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
		    formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			return formats[i];
	}

	return chosen;
}

static VkPresentModeKHR video_vulkan_choose_present_mode(
	const VkPresentModeKHR *modes, uint32_t count)
{
	uint32_t i;
	bool prefer_unblocked = g_vk.fast_forward || g_cfg.swap_interval == 0;

	if (prefer_unblocked) {
		for (i = 0; i < count; i++) {
			if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
				return modes[i];
		}

		for (i = 0; i < count; i++) {
			if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
				return modes[i];
		}
	}

	return VK_PRESENT_MODE_FIFO_KHR;
}

static uint32_t video_vulkan_clamp_uint32(uint32_t value, uint32_t min_value, uint32_t max_value)
{
	if (value < min_value)
		return min_value;
	if (max_value && value > max_value)
		return max_value;
	return value;
}

static void video_vulkan_destroy_swapchain(void)
{
	uint32_t i;

	if (!g_vk.device)
		return;

	if (g_vk.command_buffers && g_vk.command_pool && g_vk.swapchain_image_count)
		vkFreeCommandBuffers(g_vk.device, g_vk.command_pool,
			g_vk.swapchain_image_count, g_vk.command_buffers);

	if (g_vk.image_available) {
		for (i = 0; i < g_vk.swapchain_image_count; i++) {
			if (g_vk.image_available[i] != VK_NULL_HANDLE)
				vkDestroySemaphore(g_vk.device, g_vk.image_available[i], NULL);
		}
	}

	if (g_vk.render_finished) {
		for (i = 0; i < g_vk.swapchain_image_count; i++) {
			if (g_vk.render_finished[i] != VK_NULL_HANDLE)
				vkDestroySemaphore(g_vk.device, g_vk.render_finished[i], NULL);
		}
	}

	if (g_vk.frame_fences) {
		for (i = 0; i < g_vk.swapchain_image_count; i++) {
			if (g_vk.frame_fences[i] != VK_NULL_HANDLE)
				vkDestroyFence(g_vk.device, g_vk.frame_fences[i], NULL);
		}
	}

	if (g_vk.swapchain != VK_NULL_HANDLE)
		vkDestroySwapchainKHR(g_vk.device, g_vk.swapchain, NULL);

	free(g_vk.swapchain_images);
	free(g_vk.swapchain_image_initialized);
	free(g_vk.command_buffers);
	free(g_vk.image_available);
	free(g_vk.render_finished);
	free(g_vk.frame_fences);
	free(g_vk.image_fence_slots);
	free(g_vk.sync_index_wait_ready);

	g_vk.swapchain = VK_NULL_HANDLE;
	g_vk.swapchain_images = NULL;
	g_vk.swapchain_image_initialized = NULL;
	g_vk.command_buffers = NULL;
	g_vk.image_available = NULL;
	g_vk.render_finished = NULL;
	g_vk.frame_fences = NULL;
	g_vk.image_fence_slots = NULL;
	g_vk.sync_index_wait_ready = NULL;
	g_vk.swapchain_image_count = 0;
	g_vk.frame_acquired = false;
	g_vk.current_image_index = 0;
	g_vk.current_frame_slot = 0;
}

static bool video_vulkan_create_swapchain(void)
{
	VkSurfaceCapabilitiesKHR caps;
	VkSurfaceFormatKHR *formats = NULL;
	VkPresentModeKHR *present_modes = NULL;
	VkSurfaceFormatKHR chosen_format;
	VkPresentModeKHR present_mode;
	VkSwapchainCreateInfoKHR info;
	VkSemaphoreCreateInfo semaphore_info;
	VkFenceCreateInfo fence_info;
	VkCommandBufferAllocateInfo alloc_info;
	uint32_t format_count = 0;
	uint32_t present_mode_count = 0;
	uint32_t i;
	uint32_t image_count;
	int fbw = 0;
	int fbh = 0;
	VkResult res;

	if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.gpu, g_vk.surface, &caps) != VK_SUCCESS) {
		log_printf("video", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");
		return false;
	}

	if (vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.gpu, g_vk.surface, &format_count, NULL) != VK_SUCCESS ||
	    !format_count) {
		log_printf("video", "vkGetPhysicalDeviceSurfaceFormatsKHR failed");
		return false;
	}

	formats = (VkSurfaceFormatKHR *)malloc(sizeof(VkSurfaceFormatKHR) * format_count);
	if (!formats)
		die("Out of memory while enumerating Vulkan surface formats.");

	if (vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.gpu, g_vk.surface, &format_count, formats) != VK_SUCCESS) {
		free(formats);
		log_printf("video", "vkGetPhysicalDeviceSurfaceFormatsKHR returned an error");
		return false;
	}

	chosen_format = video_vulkan_choose_surface_format(formats, format_count);
	free(formats);
	formats = NULL;

	if (vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.gpu, g_vk.surface, &present_mode_count, NULL) != VK_SUCCESS ||
	    !present_mode_count) {
		log_printf("video", "vkGetPhysicalDeviceSurfacePresentModesKHR failed");
		return false;
	}

	present_modes = (VkPresentModeKHR *)malloc(sizeof(VkPresentModeKHR) * present_mode_count);
	if (!present_modes)
		die("Out of memory while enumerating Vulkan present modes.");

	if (vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.gpu, g_vk.surface, &present_mode_count, present_modes) != VK_SUCCESS) {
		free(present_modes);
		log_printf("video", "vkGetPhysicalDeviceSurfacePresentModesKHR returned an error");
		return false;
	}

	present_mode = video_vulkan_choose_present_mode(present_modes, present_mode_count);
	free(present_modes);
	present_modes = NULL;

	glfwGetFramebufferSize(g_vk.window, &fbw, &fbh);
	if (fbw <= 0 || fbh <= 0) {
		log_printf("video", "skipping swapchain creation while framebuffer size is zero");
		g_vk.swapchain_dirty = true;
		return true;
	}

	if (caps.currentExtent.width == UINT32_MAX) {
		g_vk.swapchain_extent.width = video_vulkan_clamp_uint32(
			(uint32_t)fbw,
			caps.minImageExtent.width,
			caps.maxImageExtent.width);
		g_vk.swapchain_extent.height = video_vulkan_clamp_uint32(
			(uint32_t)fbh,
			caps.minImageExtent.height,
			caps.maxImageExtent.height);
	} else {
		g_vk.swapchain_extent = caps.currentExtent;
	}

	image_count = caps.minImageCount + 1;
	if (caps.maxImageCount && image_count > caps.maxImageCount)
		image_count = caps.maxImageCount;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	info.surface = g_vk.surface;
	info.minImageCount = image_count;
	info.imageFormat = chosen_format.format;
	info.imageColorSpace = chosen_format.colorSpace;
	info.imageExtent = g_vk.swapchain_extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = caps.currentTransform;
	info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	info.presentMode = present_mode;
	info.clipped = VK_TRUE;

	log_printf("video", "creating Vulkan swapchain present_mode=%s fast_forward=%d",
		present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" :
		(present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO"),
		g_vk.fast_forward ? 1 : 0);

	res = vkCreateSwapchainKHR(g_vk.device, &info, NULL, &g_vk.swapchain);
	if (res != VK_SUCCESS) {
		log_printf("video", "vkCreateSwapchainKHR failed (%d)", (int)res);
		return false;
	}

	if (vkGetSwapchainImagesKHR(g_vk.device, g_vk.swapchain, &image_count, NULL) != VK_SUCCESS ||
	    !image_count) {
		log_printf("video", "vkGetSwapchainImagesKHR failed");
		video_vulkan_destroy_swapchain();
		return false;
	}

	g_vk.swapchain_image_count = image_count;
	g_vk.swapchain_format = chosen_format.format;
	g_vk.swapchain_images = (VkImage *)calloc(image_count, sizeof(VkImage));
	g_vk.swapchain_image_initialized = (bool *)calloc(image_count, sizeof(bool));
	g_vk.command_buffers = (VkCommandBuffer *)calloc(image_count, sizeof(VkCommandBuffer));
	g_vk.image_available = (VkSemaphore *)calloc(image_count, sizeof(VkSemaphore));
	g_vk.render_finished = (VkSemaphore *)calloc(image_count, sizeof(VkSemaphore));
	g_vk.frame_fences = (VkFence *)calloc(image_count, sizeof(VkFence));
	g_vk.image_fence_slots = (uint32_t *)calloc(image_count, sizeof(uint32_t));
	g_vk.sync_index_wait_ready = (bool *)calloc(image_count, sizeof(bool));

	if (!g_vk.swapchain_images || !g_vk.swapchain_image_initialized ||
	    !g_vk.command_buffers || !g_vk.image_available ||
	    !g_vk.render_finished || !g_vk.frame_fences ||
	    !g_vk.image_fence_slots || !g_vk.sync_index_wait_ready)
		die("Out of memory while allocating Vulkan swapchain objects.");

	if (vkGetSwapchainImagesKHR(g_vk.device, g_vk.swapchain, &image_count,
	    g_vk.swapchain_images) != VK_SUCCESS) {
		log_printf("video", "vkGetSwapchainImagesKHR data fetch failed");
		video_vulkan_destroy_swapchain();
		return false;
	}

	if (!video_vulkan_create_command_pool()) {
		video_vulkan_destroy_swapchain();
		return false;
	}

	memset(&alloc_info, 0, sizeof(alloc_info));
	alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc_info.commandPool = g_vk.command_pool;
	alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc_info.commandBufferCount = image_count;

	res = vkAllocateCommandBuffers(g_vk.device, &alloc_info, g_vk.command_buffers);
	if (res != VK_SUCCESS) {
		log_printf("video", "vkAllocateCommandBuffers failed (%d)", (int)res);
		video_vulkan_destroy_swapchain();
		return false;
	}

	memset(&semaphore_info, 0, sizeof(semaphore_info));
	semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	for (i = 0; i < image_count; i++) {
		g_vk.image_fence_slots[i] = UINT32_MAX;

		if (vkCreateSemaphore(g_vk.device, &semaphore_info, NULL,
		    &g_vk.image_available[i]) != VK_SUCCESS ||
		    vkCreateSemaphore(g_vk.device, &semaphore_info, NULL,
		    &g_vk.render_finished[i]) != VK_SUCCESS ||
		    vkCreateFence(g_vk.device, &fence_info, NULL,
		    &g_vk.frame_fences[i]) != VK_SUCCESS) {
			log_printf("video", "failed to create Vulkan sync objects for frame %u", i);
			video_vulkan_destroy_swapchain();
			return false;
		}
	}

	g_vk.next_frame_slot = 0;
	g_vk.swapchain_dirty = false;
	log_printf("video", "Vulkan swapchain ready: %ux%u images=%u format=%u",
		g_vk.swapchain_extent.width,
		g_vk.swapchain_extent.height,
		g_vk.swapchain_image_count,
		g_vk.swapchain_format);
	return true;
}

static bool video_vulkan_recreate_swapchain(void)
{
	if (!g_vk.initialized)
		return false;

	vkDeviceWaitIdle(g_vk.device);
	video_vulkan_destroy_swapchain();
	return video_vulkan_create_swapchain();
}

static bool video_vulkan_ensure_swapchain(void)
{
	int fbw = 0;
	int fbh = 0;

	if (!g_vk.initialized)
		return false;

	glfwGetFramebufferSize(g_vk.window, &fbw, &fbh);
	if (fbw <= 0 || fbh <= 0)
		return false;

	if (g_vk.swapchain == VK_NULL_HANDLE ||
	    g_vk.swapchain_dirty ||
	    g_vk.swapchain_extent.width != (uint32_t)fbw ||
	    g_vk.swapchain_extent.height != (uint32_t)fbh)
		return video_vulkan_recreate_swapchain() &&
			g_vk.swapchain != VK_NULL_HANDLE &&
			g_vk.swapchain_image_count > 0;

	return true;
}

static void video_vulkan_init_interface(void)
{
	memset(&g_vk.iface, 0, sizeof(g_vk.iface));
	g_vk.iface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
	g_vk.iface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
	g_vk.iface.handle = &g_vk;
	g_vk.iface.instance = g_vk.instance;
	g_vk.iface.gpu = g_vk.gpu;
	g_vk.iface.device = g_vk.device;
	g_vk.iface.get_device_proc_addr = vkGetDeviceProcAddr;
	g_vk.iface.get_instance_proc_addr = vkGetInstanceProcAddr;
	g_vk.iface.queue = g_vk.graphics_queue;
	g_vk.iface.queue_index = g_vk.graphics_queue_family;
	g_vk.iface.set_image = video_vulkan_set_image;
	g_vk.iface.get_sync_index = video_vulkan_get_sync_index;
	g_vk.iface.get_sync_index_mask = video_vulkan_get_sync_index_mask;
	g_vk.iface.set_command_buffers = video_vulkan_set_command_buffers;
	g_vk.iface.wait_sync_index = video_vulkan_wait_sync_index;
	g_vk.iface.lock_queue = video_vulkan_lock_queue;
	g_vk.iface.unlock_queue = video_vulkan_unlock_queue;
	g_vk.iface.set_signal_semaphore = video_vulkan_set_signal_semaphore;
}

static VkFilter video_vulkan_choose_filter(VkFormat src_format)
{
	VkFormatProperties props;

	if (strcmp(g_cfg.filter, "linear") != 0)
		return VK_FILTER_NEAREST;

	vkGetPhysicalDeviceFormatProperties(g_vk.gpu, src_format, &props);
	if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
		return VK_FILTER_LINEAR;

	return VK_FILTER_NEAREST;
}

static void video_vulkan_transition_swapchain(VkCommandBuffer cmd,
	VkImage image,
	VkImageLayout old_layout,
	VkImageLayout new_layout)
{
	VkImageMemoryBarrier barrier;
	VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = old_layout;
	barrier.newLayout = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;

	switch (old_layout) {
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;
		default:
			barrier.srcAccessMask = 0;
			break;
	}

	switch (new_layout) {
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;
		default:
			barrier.dstAccessMask = 0;
			break;
	}

	vkCmdPipelineBarrier(cmd,
		src_stage,
		dst_stage,
		0, 0, NULL, 0, NULL, 1, &barrier);
}

static void video_vulkan_transition_source(VkCommandBuffer cmd,
	VkImageLayout old_layout,
	VkImageLayout new_layout,
	uint32_t src_queue_family,
	uint32_t dst_queue_family);

static void video_vulkan_clear_swapchain_image(VkCommandBuffer cmd)
{
	VkClearColorValue clear_color;
	VkImageSubresourceRange clear_range;

	memset(&clear_color, 0, sizeof(clear_color));
	memset(&clear_range, 0, sizeof(clear_range));
	clear_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	clear_range.levelCount = 1;
	clear_range.layerCount = 1;
	vkCmdClearColorImage(cmd,
		g_vk.swapchain_images[g_vk.current_image_index],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		&clear_color,
		1,
		&clear_range);
}

static void video_vulkan_blit_active_image_to_swapchain(VkCommandBuffer cmd)
{
	VkImageBlit blit_region;
	VkImageLayout source_original_layout;
	uint32_t source_width;
	uint32_t source_height;
	aspect_viewport_t vp;
	bool need_source_acquire = false;
	bool need_source_release = false;
	VkFilter blit_filter;

	if (!g_vk.active_image_valid || g_vk.active_image.create_info.image == VK_NULL_HANDLE)
		return;

	source_width = g_vk.active_width ? g_vk.active_width :
		(g_vk.geom.base_width ? g_vk.geom.base_width : g_vk.swapchain_extent.width);
	source_height = g_vk.active_height ? g_vk.active_height :
		(g_vk.geom.base_height ? g_vk.geom.base_height : g_vk.swapchain_extent.height);
	source_original_layout = g_vk.active_image.image_layout;
	need_source_acquire =
		(source_original_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) ||
		(g_vk.src_queue_family != VK_QUEUE_FAMILY_IGNORED &&
		 g_vk.src_queue_family != g_vk.graphics_queue_family);
	need_source_release =
		(source_original_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) ||
		(g_vk.src_queue_family != VK_QUEUE_FAMILY_IGNORED &&
		 g_vk.src_queue_family != g_vk.graphics_queue_family);

	if (need_source_acquire) {
		video_vulkan_transition_source(cmd,
			source_original_layout,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			(g_vk.src_queue_family != VK_QUEUE_FAMILY_IGNORED)
				? g_vk.src_queue_family
				: VK_QUEUE_FAMILY_IGNORED,
			(g_vk.src_queue_family != VK_QUEUE_FAMILY_IGNORED)
				? g_vk.graphics_queue_family
				: VK_QUEUE_FAMILY_IGNORED);
	}

	vp = aspect_calc((int)g_vk.swapchain_extent.width,
		(int)g_vk.swapchain_extent.height,
		(int)source_width,
		(int)source_height,
		g_vk.aspect_ratio);
	memset(&blit_region, 0, sizeof(blit_region));
	blit_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit_region.srcSubresource.layerCount = 1;
	blit_region.srcOffsets[1].x = (int32_t)source_width;
	blit_region.srcOffsets[1].y = (int32_t)source_height;
	blit_region.srcOffsets[1].z = 1;
	blit_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit_region.dstSubresource.layerCount = 1;
	blit_region.dstOffsets[0].x = vp.x;
	blit_region.dstOffsets[0].y = vp.y;
	blit_region.dstOffsets[1].x = vp.x + vp.w;
	blit_region.dstOffsets[1].y = vp.y + vp.h;
	blit_region.dstOffsets[1].z = 1;

	blit_filter = video_vulkan_choose_filter(g_vk.active_image.create_info.format);
	vkCmdBlitImage(cmd,
		g_vk.active_image.create_info.image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		g_vk.swapchain_images[g_vk.current_image_index],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1,
		&blit_region,
		blit_filter);

	if (need_source_release) {
		video_vulkan_transition_source(cmd,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			source_original_layout,
			(g_vk.src_queue_family != VK_QUEUE_FAMILY_IGNORED)
				? g_vk.graphics_queue_family
				: VK_QUEUE_FAMILY_IGNORED,
			(g_vk.src_queue_family != VK_QUEUE_FAMILY_IGNORED)
				? g_vk.src_queue_family
				: VK_QUEUE_FAMILY_IGNORED);
	}
}

static void video_vulkan_transition_source(VkCommandBuffer cmd,
	VkImageLayout old_layout,
	VkImageLayout new_layout,
	uint32_t src_queue_family,
	uint32_t dst_queue_family)
{
	VkImageMemoryBarrier barrier;
	VkImageSubresourceRange range = video_vulkan_source_range();

	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = old_layout;
	barrier.newLayout = new_layout;
	barrier.srcQueueFamilyIndex = src_queue_family;
	barrier.dstQueueFamilyIndex = dst_queue_family;
	barrier.image = g_vk.active_image.create_info.image;
	barrier.subresourceRange = range;

	if (new_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
		barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		vkCmdPipelineBarrier(cmd,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, NULL, 0, NULL, 1, &barrier);
	} else {
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.dstAccessMask = 0;
		vkCmdPipelineBarrier(cmd,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			0, 0, NULL, 0, NULL, 1, &barrier);
	}
}

bool video_vulkan_supported(void)
{
	return glfwVulkanSupported() == GLFW_TRUE;
}

bool video_vulkan_set_negotiation_interface(
	const struct retro_hw_render_context_negotiation_interface *iface)
{
	const struct retro_hw_render_context_negotiation_interface_vulkan *vk_iface;

	if (!iface)
		return false;

	if (iface->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
		return false;

	vk_iface = (const struct retro_hw_render_context_negotiation_interface_vulkan *)iface;
	g_vk.negotiation = *vk_iface;
	g_vk.negotiation_set = true;
	log_printf("video", "registered Vulkan negotiation interface v%u",
		g_vk.negotiation.interface_version);
	return true;
}

bool video_vulkan_get_negotiation_interface_support(
	struct retro_hw_render_context_negotiation_interface *iface)
{
	if (!iface)
		return false;

	if (iface->interface_type == RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
		iface->interface_version = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION;
	else
		iface->interface_version = 0;

	return true;
}

const struct retro_hw_render_interface *video_vulkan_get_hw_render_interface(void)
{
	if (!g_vk.initialized)
		return NULL;

	return (const struct retro_hw_render_interface *)&g_vk.iface;
}

bool video_vulkan_init(GLFWwindow *window,
	const struct retro_game_geometry *geom,
	const struct retro_hw_render_callback *hw)
{
	if (!window || !geom || !hw)
		return false;

	g_vk.window = window;
	g_vk.hw = *hw;
	video_vulkan_set_geometry(geom);

	if (g_vk.initialized)
		return true;

	video_vulkan_queue_lock_init();
	video_vulkan_log_problematic_overlay_hooks();

	if (!video_vulkan_create_instance())
		return false;
	if (!video_vulkan_create_surface())
		return false;
	if (!video_vulkan_create_device())
		return false;
	if (!video_vulkan_create_swapchain())
		return false;

	video_vulkan_init_interface();
	video_vulkan_reset_submission_state();
	g_vk.initialized = true;

	log_printf("video", "Vulkan backend initialized queue_family=%u",
		g_vk.graphics_queue_family);
	return true;
}

void video_vulkan_set_geometry(const struct retro_game_geometry *geom)
{
	if (!geom)
		return;

	g_vk.geom = *geom;
	g_vk.aspect_ratio = video_vulkan_resolve_aspect_ratio(geom);
}

void video_vulkan_mark_swapchain_dirty(void)
{
	g_vk.swapchain_dirty = true;
}

void video_vulkan_set_fast_forward(bool enabled)
{
	if (g_vk.fast_forward == enabled)
		return;

	g_vk.fast_forward = enabled;
	log_printf("video", "Vulkan fast-forward=%d", enabled ? 1 : 0);

	/* Evita recriar a swapchain a cada press/release do fast-forward.
	 * Isso estava derrubando cores sensiveis no caminho Vulkan. */
}

void video_vulkan_begin_frame(void)
{
	VkResult res;
	uint32_t slot;
	uint32_t previous_slot;

	video_vulkan_reset_submission_state();
	g_vk.frame_acquired = false;

	if (!g_vk.initialized)
		return;

	if (!video_vulkan_ensure_swapchain())
		return;

	if (g_vk.swapchain == VK_NULL_HANDLE || !g_vk.swapchain_image_count)
		return;

	if (g_vk.sync_index_wait_ready)
		memset(g_vk.sync_index_wait_ready, 0, sizeof(bool) * g_vk.swapchain_image_count);

	slot = g_vk.next_frame_slot;
	g_vk.next_frame_slot = (slot + 1) % g_vk.swapchain_image_count;

	if (!video_vulkan_wait_for_fence_with_timeout("begin_frame_slot",
		g_vk.frame_fences[slot],
		g_vk.current_image_index,
		slot,
		1000000000ULL))
		return;
	vkResetFences(g_vk.device, 1, &g_vk.frame_fences[slot]);

	res = vkAcquireNextImageKHR(g_vk.device,
		g_vk.swapchain,
		UINT64_MAX,
		g_vk.image_available[slot],
		VK_NULL_HANDLE,
		&g_vk.current_image_index);

	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
		video_vulkan_mark_swapchain_dirty();
		video_vulkan_ensure_swapchain();
		return;
	}

	if (res != VK_SUCCESS) {
		log_printf("video", "vkAcquireNextImageKHR failed (%d)", (int)res);
		return;
	}

	previous_slot = g_vk.image_fence_slots[g_vk.current_image_index];
	if (previous_slot == UINT32_MAX || previous_slot == slot) {
		if (g_vk.sync_index_wait_ready)
			g_vk.sync_index_wait_ready[g_vk.current_image_index] = true;
	} else {
		if (!video_vulkan_wait_for_fence_with_timeout("begin_frame_previous_slot",
			g_vk.frame_fences[previous_slot],
			g_vk.current_image_index,
			previous_slot,
			1000000000ULL)) {
			g_vk.image_fence_slots[g_vk.current_image_index] = UINT32_MAX;
			return;
		}

		if (g_vk.sync_index_wait_ready)
			g_vk.sync_index_wait_ready[g_vk.current_image_index] = true;
	}

	g_vk.current_frame_slot = slot;
	g_vk.frame_acquired = true;
}

void video_vulkan_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
	(void)pitch;

	if (!g_vk.initialized)
		return;

	if (width)
		g_vk.pending_width = width;
	if (height)
		g_vk.pending_height = height;

	if (data == RETRO_HW_FRAME_BUFFER_VALID) {
		if (g_vk.pending_image_valid) {
			g_vk.active_image = g_vk.pending_image;
			g_vk.active_image_valid = true;
			if (g_vk.pending_width)
				g_vk.active_width = g_vk.pending_width;
			if (g_vk.pending_height)
				g_vk.active_height = g_vk.pending_height;
		}

		return;
	}

	if (!data)
		return;

	if (!g_vk.warned_software_upload) {
		log_printf("video", "software uploads are not supported on the Vulkan backend yet");
		g_vk.warned_software_upload = true;
	}
}

void video_vulkan_render(void)
{
	VkCommandBuffer cmd;
	VkCommandBufferBeginInfo begin_info;
	VkSubmitInfo submit_info;
	VkPresentInfoKHR present_info;
	VkSemaphore signal_semaphores[2];
	VkSemaphore *waits = NULL;
	VkPipelineStageFlags *wait_stages = NULL;
	VkCommandBuffer *commands = NULL;
	VkResult res;
	uint32_t wait_count;
	uint32_t command_count;
	uint32_t signal_count = 0;
	bool ignore_image_waits;

	if (!g_vk.initialized || !g_vk.frame_acquired)
		return;

	cmd = g_vk.command_buffers[g_vk.current_frame_slot];

	vkResetCommandBuffer(cmd, 0);

	memset(&begin_info, 0, sizeof(begin_info));
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
		log_printf("video", "vkBeginCommandBuffer failed");
		g_vk.frame_acquired = false;
		return;
	}

	video_vulkan_transition_swapchain(cmd,
		g_vk.swapchain_images[g_vk.current_image_index],
		g_vk.swapchain_image_initialized[g_vk.current_image_index]
			? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
			: VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	video_vulkan_clear_swapchain_image(cmd);
	video_vulkan_blit_active_image_to_swapchain(cmd);

	video_vulkan_transition_swapchain(cmd,
		g_vk.swapchain_images[g_vk.current_image_index],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	g_vk.swapchain_image_initialized[g_vk.current_image_index] = true;

	if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
		log_printf("video", "vkEndCommandBuffer failed");
		g_vk.frame_acquired = false;
		return;
	}

	/* Per libretro_vulkan.h, semaphores passed via set_image() must be
	 * ignored when the core also passes command buffers for frontend submission.
	 * Waiting on both can deadlock because the core expects those command buffers
	 * to be submitted by the frontend, not by the core itself. */
	ignore_image_waits = g_vk.core_command_count > 0;
	if (ignore_image_waits && g_vk.wait_count && !g_vk_logged_ignored_image_waits) {
		log_printf("video",
			"ignoring %u set_image semaphore(s) because core submitted %u command buffer(s)",
			g_vk.wait_count,
			g_vk.core_command_count);
		g_vk_logged_ignored_image_waits = true;
	}
	wait_count = 1 + (ignore_image_waits ? 0u : g_vk.wait_count);
	waits = (VkSemaphore *)malloc(sizeof(VkSemaphore) * wait_count);
	wait_stages = (VkPipelineStageFlags *)malloc(sizeof(VkPipelineStageFlags) * wait_count);
	if (!waits || !wait_stages)
		die("Out of memory while preparing Vulkan submit info.");

	waits[0] = g_vk.image_available[g_vk.current_frame_slot];
	wait_stages[0] = VK_PIPELINE_STAGE_TRANSFER_BIT;
	if (!ignore_image_waits && g_vk.wait_count) {
		memcpy(&waits[1], g_vk.wait_semaphores, sizeof(VkSemaphore) * g_vk.wait_count);
		memcpy(&wait_stages[1], g_vk.wait_stages, sizeof(VkPipelineStageFlags) * g_vk.wait_count);
	}

	command_count = 1 + g_vk.core_command_count;
	commands = (VkCommandBuffer *)malloc(sizeof(VkCommandBuffer) * command_count);
	if (!commands)
		die("Out of memory while preparing Vulkan command buffers.");

	if (g_vk.core_command_count)
		memcpy(commands, g_vk.core_commands,
			sizeof(VkCommandBuffer) * g_vk.core_command_count);
	commands[command_count - 1] = cmd;

	signal_semaphores[signal_count++] = g_vk.render_finished[g_vk.current_frame_slot];
	if (g_vk.signal_semaphore != VK_NULL_HANDLE)
		signal_semaphores[signal_count++] = g_vk.signal_semaphore;

	memset(&submit_info, 0, sizeof(submit_info));
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.waitSemaphoreCount = wait_count;
	submit_info.pWaitSemaphores = waits;
	submit_info.pWaitDstStageMask = wait_stages;
	submit_info.commandBufferCount = command_count;
	submit_info.pCommandBuffers = commands;
	submit_info.signalSemaphoreCount = signal_count;
	submit_info.pSignalSemaphores = signal_semaphores;

	video_vulkan_lock_queue(&g_vk);
	res = vkQueueSubmit(g_vk.graphics_queue, 1, &submit_info,
		g_vk.frame_fences[g_vk.current_frame_slot]);
	video_vulkan_unlock_queue(&g_vk);
	if (res != VK_SUCCESS) {
		log_printf("video", "vkQueueSubmit failed (%d)", (int)res);
		free(commands);
		free(wait_stages);
		free(waits);
		g_vk.frame_acquired = false;
		return;
	}

	memset(&present_info, 0, sizeof(present_info));
	present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present_info.waitSemaphoreCount = 1;
	present_info.pWaitSemaphores = &g_vk.render_finished[g_vk.current_frame_slot];
	present_info.swapchainCount = 1;
	present_info.pSwapchains = &g_vk.swapchain;
	present_info.pImageIndices = &g_vk.current_image_index;

	video_vulkan_lock_queue(&g_vk);
	res = vkQueuePresentKHR(g_vk.graphics_queue, &present_info);
	video_vulkan_unlock_queue(&g_vk);

	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
		video_vulkan_mark_swapchain_dirty();
	else if (res != VK_SUCCESS)
		log_printf("video", "vkQueuePresentKHR failed (%d)", (int)res);

	g_vk.image_fence_slots[g_vk.current_image_index] = g_vk.current_frame_slot;
	g_vk.frame_acquired = false;

	free(commands);
	free(wait_stages);
	free(waits);
}

/* ─── Menu overlay (software rendered, copied to swapchain) ─── */

static VkBuffer menu_staging_buf = VK_NULL_HANDLE;
static VkDeviceMemory menu_staging_mem = VK_NULL_HANDLE;
static void *menu_staging_ptr = NULL;
static uint32_t menu_staging_capacity = 0;
static bool menu_staging_host_coherent = false;

static uint32_t find_host_visible_memory_type(uint32_t type_bits)
{
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(g_vk.gpu, &props);

	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((type_bits & (1u << i)) &&
		    (props.memoryTypes[i].propertyFlags &
		     (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
		     (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			return i;
	}

	/* Fallback: any host-visible */
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((type_bits & (1u << i)) &&
		    (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
			return i;
	}
	return UINT32_MAX;
}

static bool ensure_menu_staging(uint32_t needed)
{
	VkBufferCreateInfo buf_info;
	VkMemoryRequirements mem_req;
	VkMemoryAllocateInfo alloc_info;
	VkPhysicalDeviceMemoryProperties props;
	uint32_t mem_type;

	if (menu_staging_capacity >= needed)
		return true;

	/* Destroy previous */
	if (menu_staging_buf != VK_NULL_HANDLE) {
		vkUnmapMemory(g_vk.device, menu_staging_mem);
		vkDestroyBuffer(g_vk.device, menu_staging_buf, NULL);
		vkFreeMemory(g_vk.device, menu_staging_mem, NULL);
		menu_staging_buf = VK_NULL_HANDLE;
		menu_staging_ptr = NULL;
		menu_staging_capacity = 0;
		menu_staging_host_coherent = false;
	}

	memset(&buf_info, 0, sizeof(buf_info));
	buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buf_info.size = needed;
	buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if (vkCreateBuffer(g_vk.device, &buf_info, NULL, &menu_staging_buf) != VK_SUCCESS)
		return false;

	vkGetBufferMemoryRequirements(g_vk.device, menu_staging_buf, &mem_req);
	mem_type = find_host_visible_memory_type(mem_req.memoryTypeBits);
	if (mem_type == UINT32_MAX) {
		vkDestroyBuffer(g_vk.device, menu_staging_buf, NULL);
		menu_staging_buf = VK_NULL_HANDLE;
		return false;
	}

	memset(&alloc_info, 0, sizeof(alloc_info));
	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.allocationSize = mem_req.size;
	alloc_info.memoryTypeIndex = mem_type;

	if (vkAllocateMemory(g_vk.device, &alloc_info, NULL, &menu_staging_mem) != VK_SUCCESS) {
		vkDestroyBuffer(g_vk.device, menu_staging_buf, NULL);
		menu_staging_buf = VK_NULL_HANDLE;
		return false;
	}

	vkBindBufferMemory(g_vk.device, menu_staging_buf, menu_staging_mem, 0);
	vkMapMemory(g_vk.device, menu_staging_mem, 0, needed, 0, &menu_staging_ptr);
	vkGetPhysicalDeviceMemoryProperties(g_vk.gpu, &props);
	menu_staging_host_coherent =
		(props.memoryTypes[mem_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
	menu_staging_capacity = needed;
	return true;
}

static void destroy_menu_staging(void)
{
	if (menu_staging_buf != VK_NULL_HANDLE) {
		vkUnmapMemory(g_vk.device, menu_staging_mem);
		vkDestroyBuffer(g_vk.device, menu_staging_buf, NULL);
		vkFreeMemory(g_vk.device, menu_staging_mem, NULL);
		menu_staging_buf = VK_NULL_HANDLE;
		menu_staging_ptr = NULL;
		menu_staging_capacity = 0;
		menu_staging_host_coherent = false;
	}
}

static void sync_menu_staging_for_cpu(void)
{
	VkMappedMemoryRange range;

	if (menu_staging_host_coherent || menu_staging_mem == VK_NULL_HANDLE)
		return;

	memset(&range, 0, sizeof(range));
	range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = menu_staging_mem;
	range.offset = 0;
	range.size = VK_WHOLE_SIZE;
	vkInvalidateMappedMemoryRanges(g_vk.device, 1, &range);
}

static void sync_menu_staging_for_gpu(void)
{
	VkMappedMemoryRange range;

	if (menu_staging_host_coherent || menu_staging_mem == VK_NULL_HANDLE)
		return;

	memset(&range, 0, sizeof(range));
	range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = menu_staging_mem;
	range.offset = 0;
	range.size = VK_WHOLE_SIZE;
	vkFlushMappedMemoryRanges(g_vk.device, 1, &range);
}

void video_vulkan_render_menu(void)
{
	VkCommandBuffer cmd;
	VkCommandBufferBeginInfo begin_info;
	VkSubmitInfo submit_info;
	VkPresentInfoKHR present_info;
	VkBufferImageCopy region;
	VkResult res;
	VkSemaphore wait_sem;
	VkPipelineStageFlags wait_stage;

	if (!g_vk.initialized)
		return;

	/* Acquire frame if not already */
	if (!g_vk.frame_acquired)
		video_vulkan_begin_frame();
	if (!g_vk.frame_acquired)
		return;

	uint32_t w = g_vk.swapchain_extent.width;
	uint32_t h = g_vk.swapchain_extent.height;
	uint32_t buf_size = w * h * 4;

	if (!ensure_menu_staging(buf_size))
		return;

	/* Pass 1: render the last game frame into the swapchain, then read it back
	 * to the CPU staging buffer so the menu can be blended on top instead of
	 * replacing the whole screen with black. */
	cmd = g_vk.command_buffers[g_vk.current_frame_slot];
	vkResetCommandBuffer(cmd, 0);

	memset(&begin_info, 0, sizeof(begin_info));
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
		log_printf("video", "menu pass 1 vkBeginCommandBuffer failed");
		g_vk.frame_acquired = false;
		return;
	}

	video_vulkan_transition_swapchain(cmd,
		g_vk.swapchain_images[g_vk.current_image_index],
		g_vk.swapchain_image_initialized[g_vk.current_image_index]
			? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
			: VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	video_vulkan_clear_swapchain_image(cmd);
	video_vulkan_blit_active_image_to_swapchain(cmd);
	video_vulkan_transition_swapchain(cmd,
		g_vk.swapchain_images[g_vk.current_image_index],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	memset(&region, 0, sizeof(region));
	region.bufferRowLength = w;
	region.bufferImageHeight = h;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	region.imageExtent.depth = 1;

	vkCmdCopyImageToBuffer(cmd,
		g_vk.swapchain_images[g_vk.current_image_index],
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		menu_staging_buf,
		1, &region);
	if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
		log_printf("video", "menu pass 1 vkEndCommandBuffer failed");
		g_vk.frame_acquired = false;
		return;
	}

	wait_sem = g_vk.image_available[g_vk.current_frame_slot];
	wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;

	memset(&submit_info, 0, sizeof(submit_info));
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.waitSemaphoreCount = 1;
	submit_info.pWaitSemaphores = &wait_sem;
	submit_info.pWaitDstStageMask = &wait_stage;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &cmd;

	video_vulkan_lock_queue(&g_vk);
	res = vkQueueSubmit(g_vk.graphics_queue, 1, &submit_info,
		g_vk.frame_fences[g_vk.current_frame_slot]);
	video_vulkan_unlock_queue(&g_vk);

	if (res != VK_SUCCESS) {
		log_printf("video", "menu vkQueueSubmit failed (%d)", (int)res);
		g_vk.frame_acquired = false;
		return;
	}

	vkWaitForFences(g_vk.device, 1, &g_vk.frame_fences[g_vk.current_frame_slot], VK_TRUE, UINT64_MAX);
	vkResetFences(g_vk.device, 1, &g_vk.frame_fences[g_vk.current_frame_slot]);
	sync_menu_staging_for_cpu();

	{
		uint32_t *pixels = (uint32_t *)menu_staging_ptr;
		font_set_sw_target(pixels, (int)w, (int)h);
		menu_set_sw_target(pixels, (int)w, (int)h);
		menu_render();
		font_clear_sw_target();
		menu_clear_sw_target();
	}
	sync_menu_staging_for_gpu();

	/* Pass 2: upload the composited frame back to the swapchain for present. */
	vkResetCommandBuffer(cmd, 0);
	memset(&begin_info, 0, sizeof(begin_info));
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
		log_printf("video", "menu pass 2 vkBeginCommandBuffer failed");
		g_vk.frame_acquired = false;
		return;
	}

	video_vulkan_transition_swapchain(cmd,
		g_vk.swapchain_images[g_vk.current_image_index],
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdCopyBufferToImage(cmd,
		menu_staging_buf,
		g_vk.swapchain_images[g_vk.current_image_index],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &region);
	video_vulkan_transition_swapchain(cmd,
		g_vk.swapchain_images[g_vk.current_image_index],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	g_vk.swapchain_image_initialized[g_vk.current_image_index] = true;

	if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
		log_printf("video", "menu pass 2 vkEndCommandBuffer failed");
		g_vk.frame_acquired = false;
		return;
	}

	memset(&submit_info, 0, sizeof(submit_info));
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &cmd;
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores = &g_vk.render_finished[g_vk.current_frame_slot];

	video_vulkan_lock_queue(&g_vk);
	res = vkQueueSubmit(g_vk.graphics_queue, 1, &submit_info,
		g_vk.frame_fences[g_vk.current_frame_slot]);
	video_vulkan_unlock_queue(&g_vk);

	if (res != VK_SUCCESS) {
		log_printf("video", "menu pass 2 vkQueueSubmit failed (%d)", (int)res);
		g_vk.frame_acquired = false;
		return;
	}

	memset(&present_info, 0, sizeof(present_info));
	present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present_info.waitSemaphoreCount = 1;
	present_info.pWaitSemaphores = &g_vk.render_finished[g_vk.current_frame_slot];
	present_info.swapchainCount = 1;
	present_info.pSwapchains = &g_vk.swapchain;
	present_info.pImageIndices = &g_vk.current_image_index;

	video_vulkan_lock_queue(&g_vk);
	res = vkQueuePresentKHR(g_vk.graphics_queue, &present_info);
	video_vulkan_unlock_queue(&g_vk);

	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
		video_vulkan_mark_swapchain_dirty();

	g_vk.image_fence_slots[g_vk.current_image_index] = g_vk.current_frame_slot;
	g_vk.frame_acquired = false;
}

void video_vulkan_deinit(void)
{
	bool destroy_device_via_core = false;

	if (!g_vk.initialized) {
		video_vulkan_queue_lock_deinit();
		memset(&g_vk, 0, sizeof(g_vk));
		return;
	}

	if (g_vk.device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(g_vk.device);

	destroy_menu_staging();

	video_vulkan_destroy_swapchain();

	if (g_vk.command_pool != VK_NULL_HANDLE)
		vkDestroyCommandPool(g_vk.device, g_vk.command_pool, NULL);

	destroy_device_via_core =
		g_vk.device_created_by_core &&
		g_vk.negotiation_set &&
		g_vk.negotiation.destroy_device != NULL;

	if (destroy_device_via_core)
		g_vk.negotiation.destroy_device();

	if (g_vk.surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);

	if (!destroy_device_via_core && g_vk.device != VK_NULL_HANDLE)
		vkDestroyDevice(g_vk.device, NULL);

	if (g_vk.instance != VK_NULL_HANDLE)
		vkDestroyInstance(g_vk.instance, NULL);

	free(g_vk.wait_semaphores);
	free(g_vk.wait_stages);
	free(g_vk.core_commands);

	video_vulkan_queue_lock_deinit();
	memset(&g_vk, 0, sizeof(g_vk));
}
