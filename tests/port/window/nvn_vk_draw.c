/* tests/port/window: stand-in for runtime/nvn_vk_draw.c. The Makefile compiles a COPY of runtime/nvn_vk.c in the build
 * folder next to this file, so its `#include "nvn_vk_draw.c"` takes this one: the window/instance/device/surface/swapchain/
 * present code of nvn_vk.c runs as it is, without the drawing part (vkcore's files: they need the whole renderer). */
static void vkd_device_info(VkDeviceCreateInfo *di, const VkExtensionProperties *dext, uint32_t ndext) { (void)di; (void)dext; (void)ndext; }
static void vkd_init(void) {}
static void vkd_shutdown(void) {}
static int wd_iconic(void) { int focused, minimised; display_window_state(&focused, &minimised); return minimised; }    /* = runtime/nvn_vk_wd.c (Linux) */
int test_wd_iconic(void) { return wd_iconic(); }
