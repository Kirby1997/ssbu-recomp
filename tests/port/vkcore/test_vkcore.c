/* vkcore tests on the Frame: (1) store mmap reader vs a synthetic archive, (2) cache file atomic replace, (3) path / dir / find
 * helpers of nvn_vk_posix.h, (4) headless Vulkan: the memory-type selection of nvn_vk_draw.c (vk_mem_alloc, the readback
 * choice at the cb cache) picks valid types for staging / device-local / readback on this driver and the memory works. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include "plat.h"
#include "nvn_vk_posix.h"
#include <vulkan/vulkan.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

/* ---- 1. store reader ---- */
#define ARC_SIZE (300ull << 20)
static void test_store(const char *dir) {
    char path[512], real[512];
    snprintf(path, sizeof path, "%s/data.arc", dir);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "create archive");
    CHECK(ftruncate(fd, (off_t)ARC_SIZE) == 0, "truncate");
    /* a frame magic, a BNSH header and a "random" run at positions that straddle the 64 MB slices and the page edges */
    uint64_t offs[] = { 0, 8189, 4096, (64ull << 20) - 8, (64ull << 20), (64ull << 20) + 0x40000, (128ull << 20) - 3, 200ull * 1000 * 1000 + 17, ARC_SIZE - 4 };
    int n = (int)(sizeof offs / sizeof offs[0]);
    static const uint8_t magic[4] = {0x28, 0xB5, 0x2F, 0xFD};
    for (int i = 0; i < n; i++) CHECK(pwrite(fd, magic, 4, (off_t)offs[i]) == 4, "pwrite");
    uint64_t s = 88172645463325252ull;
    uint8_t buf[4096];
    for (int i = 0; i < 4096; i++) buf[i] = (uint8_t)rnd(&s);
    buf[5] = 'B'; buf[6] = 'N'; buf[7] = 'S'; buf[8] = 'H';      /* a stray "BNSH" (the reader looks at 'B' too) */
    CHECK(pwrite(fd, buf, sizeof buf, 100ll << 20) == (ssize_t)sizeof buf, "pwrite rnd");
    /* the same scan as nvn_vk_store.c: slices of 64 MB + 256 KB room, a position counts when its 4 bytes are the magic */
    const uint8_t *base = vkp_map_file(fd, ARC_SIZE);
    CHECK(base != NULL, "vkp_map_file");
    if (!base) { close(fd); return; }
    CHECK((uint64_t)(uintptr_t)base + ARC_SIZE <= (1ull << 47), "mapping above 2^47: %p", (const void *)base);
    printf("  archive mapped at %p (%llu MB)\n", (const void *)base, (unsigned long long)(ARC_SIZE >> 20));
    int found = 0;
    uint64_t SL = 64ull << 20, ROOM = 0x40000;
    for (uint64_t lo = 0; lo < ARC_SIZE; lo += SL) {
        uint64_t hi = lo + SL < ARC_SIZE ? lo + SL : ARC_SIZE, vlen = hi + ROOM < ARC_SIZE ? hi + ROOM - lo : ARC_SIZE - lo;
        const uint8_t *v = base + lo;
        uint64_t end = hi - lo, lim = vlen >= 4 ? vlen - 3 : 0;
        if (end > lim) end = lim;
        for (uint64_t p = 0; p < end; p++) if (v[p] == 0x28 && !memcmp(v + p, magic, 4)) { found++; }
    }
    CHECK(found == n, "scan found %d of %d frames", found, n);
    /* every offset reads the same through the mapping and pread */
    int bad = 0;
    for (int i = 0; i < n; i++) { uint8_t t[4]; if (pread(fd, t, 4, (off_t)offs[i]) != 4 || memcmp(t, base + offs[i], 4)) bad++; }
    for (int i = 0; i < 4096; i++) if (base[(100ull << 20) + (uint64_t)i] != buf[i]) { bad++; break; }
    CHECK(!bad, "mapping differs from pread");
    /* the far view arithmetic: fo page aligned, p within */
    for (int i = 0; i < n; i++) { uint64_t fo = offs[i] & ~0xffffull, flen = ARC_SIZE - fo < (1ull << 30) ? ARC_SIZE - fo : (1ull << 30); CHECK(fo + flen <= ARC_SIZE && (fo & 0xfff) == 0 && !memcmp(base + fo + (offs[i] - fo), magic, 4), "far view %llu", (unsigned long long)offs[i]); }
    CHECK(munmap((void *)base, ARC_SIZE) == 0, "munmap");
    close(fd);
    (void)real;
    printf("store mmap: %d frames found in a %llu MB synthetic archive, all offsets equal to pread\n", found, (unsigned long long)(ARC_SIZE >> 20));
}

/* ---- 2. atomic replace ---- */
static volatile int stop;
static const char *g_cache;
static int g_torn, g_reads;
static void *reader(void *a) {
    (void)a;
    while (!stop) {
        FILE *f = fopen(g_cache, "rb");
        if (!f) continue;
        char b[8192]; size_t n = fread(b, 1, sizeof b, f); fclose(f);
        g_reads++;
        /* every version is 8192 bytes of one repeated byte: a torn file has a short length or two values */
        if (n != sizeof b) { g_torn++; continue; }
        for (size_t i = 1; i < n; i++) if (b[i] != b[0]) { g_torn++; break; }
    }
    return NULL;
}
static void test_replace(const char *dir) {
    char cache[512], tmp[512];
    snprintf(cache, sizeof cache, "%s/pipelines_test.bin", dir); snprintf(tmp, sizeof tmp, "%s.tmp", cache);
    g_cache = cache;
    { FILE *f = fopen(cache, "wb"); char b[8192]; memset(b, 'A', sizeof b); fwrite(b, 1, sizeof b, f); fclose(f); }
    pthread_t th; pthread_create(&th, NULL, reader, NULL);
    int ok = 1;
    for (int i = 0; i < 300; i++) {
        FILE *f = fopen(tmp, "wb"); char b[8192]; memset(b, 'B' + i % 20, sizeof b);
        if (!f || fwrite(b, 1, sizeof b, f) != sizeof b) { ok = 0; break; }
        fclose(f);
        if (!MoveFileExA(tmp, cache, MOVEFILE_REPLACE_EXISTING)) { ok = 0; break; }
    }
    stop = 1; pthread_join(th, NULL);
    CHECK(ok, "replace failed");
    CHECK(access(tmp, F_OK) != 0, "tmp file left behind");
    CHECK(g_torn == 0, "%d torn reads of %d", g_torn, g_reads);
    CHECK(!MoveFileExA("/nonexistent/x", cache, MOVEFILE_REPLACE_EXISTING), "move of a missing file succeeded");
    printf("cache replace: 300 replaces under a concurrent reader (%d reads), torn reads %d, tmp left %d\n", g_reads, g_torn, access(tmp, F_OK) == 0);
}

/* ---- 3. helpers ---- */
static void test_helpers(const char *dir) {
    char p[600], b[600];
    snprintf(p, sizeof p, "%s\\a\\b\\c", dir);
    CHECK(CreateDirectoryA(p, NULL), "mkdir -p");
    snprintf(b, sizeof b, "%s/a/b/c", dir);
    CHECK(GetFileAttributesA(b) == 0x10u, "dir attribute");
    CHECK(GetFileAttributesA("/nonexistent") == INVALID_FILE_ATTRIBUTES, "missing attribute");
    for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s/a/b/c/%d%s", dir, i, i == 2 ? ".txt" : ".bnsh"); FILE *f = fopen(p, "wb"); fwrite("12345", 1, 5 + (size_t)i, f); fclose(f); }
    WIN32_FIND_DATAA fd; int cnt = 0; uint32_t sz = 0;
    snprintf(p, sizeof p, "%s/a/b/c/*.bnsh", dir);
    HANDLE h = FindFirstFileA(p, &fd);
    CHECK(h != INVALID_HANDLE_VALUE, "FindFirstFileA");
    if (h != INVALID_HANDLE_VALUE) { do { cnt++; sz += fd.nFileSizeLow; } while (FindNextFileA(h, &fd)); FindClose(h); }
    CHECK(cnt == 2 && sz == 11, "find: %d files, %u bytes", cnt, sz);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    snprintf(p, sizeof p, "%s/a/b/c/2.txt", dir);
    CHECK(GetFileAttributesExA(p, GetFileExInfoStandard, &fa) && fa.nFileSizeLow == 7 && fa.ftLastWriteTime.dwHighDateTime > 0x01d00000u, "GetFileAttributesEx");
    CHECK(DeleteFileA(p) && GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES, "DeleteFileA");
    char r[512];
    const char *q = vkp_path("..\\cache\\vk\\x.bin", r, sizeof r);
    CHECK(strstr(q, "/cache/vk/x.bin") && q[0] == '/' && !strchr(q, '\\') && strstr(q, "/../cache") == NULL ? 1 : strstr(q, "../cache/vk/x.bin") != NULL, "path: %s", q);
    CHECK(!strcmp(vkp_path("/abs/p", r, sizeof r), "/abs/p"), "absolute unchanged");
    printf("helpers: mkdir -p, find (%d files), attributes, delete, exe-relative path -> %s\n", cnt, q);
}

/* ---- 4. headless Vulkan ---- */
static int pick(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkFlags want) {   /* vk_mem_alloc */
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++) if ((bits >> i & 1) && (mp->memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}
static void test_vulkan(void) {
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("vulkan: libvulkan.so.1 not found, skipped\n"); return; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ic = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ic.pApplicationInfo = &ai;
    VkInstance inst;
    CHECK(ci(&ic, NULL, &inst) == VK_SUCCESS, "vkCreateInstance");
#define IP(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
    IP(vkEnumeratePhysicalDevices); IP(vkGetPhysicalDeviceProperties); IP(vkGetPhysicalDeviceMemoryProperties); IP(vkGetPhysicalDeviceQueueFamilyProperties);
    IP(vkCreateDevice); IP(vkGetDeviceProcAddr); IP(vkDestroyInstance); IP(vkGetPhysicalDeviceFeatures); IP(vkGetPhysicalDeviceFormatProperties);
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, NULL);
    CHECK(n > 0, "no physical device");
    if (!n) return;
    VkPhysicalDevice pd[8]; if (n > 8) n = 8; vkEnumeratePhysicalDevices(inst, &n, pd);
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pd[0], &pr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd[0], &mp);
    printf("  device %s (api %u.%u.%u), %u heaps, %u memory types:", pr.deviceName, VK_VERSION_MAJOR(pr.apiVersion), VK_VERSION_MINOR(pr.apiVersion), VK_VERSION_PATCH(pr.apiVersion), mp.memoryHeapCount, mp.memoryTypeCount);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) printf(" [%u]=0x%x", i, (unsigned)mp.memoryTypes[i].propertyFlags);
    printf("\n");
    VkPhysicalDeviceFeatures ft; vkGetPhysicalDeviceFeatures(pd[0], &ft);
    printf("  textureCompressionBC %d, ASTC_LDR %d\n", (int)ft.textureCompressionBC, (int)ft.textureCompressionASTC_LDR);
    CHECK(ft.textureCompressionBC, "BC not native");
    CHECK(ft.textureCompressionASTC_LDR, "ASTC LDR not native");
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(pd[0], VK_FORMAT_BC7_UNORM_BLOCK, &fp); CHECK(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "BC7 sampled");
    vkGetPhysicalDeviceFormatProperties(pd[0], VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &fp); CHECK(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "ASTC 4x4 sampled");
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd[0], &qn, NULL);
    VkQueueFamilyProperties qf[16]; if (qn > 16) qn = 16; vkGetPhysicalDeviceQueueFamilyProperties(pd[0], &qn, qf);
    uint32_t q = 0; while (q < qn && !(qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) q++;
    float pri = 1; VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex = q; qi.queueCount = 1; qi.pQueuePriorities = &pri;
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    VkDevice dev; CHECK(vkCreateDevice(pd[0], &di, NULL, &dev) == VK_SUCCESS, "vkCreateDevice");
#define DP(n) PFN_##n n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    DP(vkCreateBuffer); DP(vkGetBufferMemoryRequirements); DP(vkAllocateMemory); DP(vkMapMemory); DP(vkBindBufferMemory); DP(vkCreateImage); DP(vkGetImageMemoryRequirements);
    DP(vkBindImageMemory); DP(vkFreeMemory); DP(vkUnmapMemory); DP(vkDestroyBuffer); DP(vkDestroyImage); DP(vkDestroyDevice);
    /* staging (stream ring, vbuf_new): HOST_VISIBLE|HOST_COHERENT. readback (cb cache): HOST_VISIBLE|COHERENT|CACHED when a type has all three, else without CACHED.
     * device-local images: DEVICE_LOCAL only. Each is allocated, mapped where host-visible, written and read back. */
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 4u << 20; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer b; VkMemoryRequirements rq;
    struct { const char *name; VkFlags usage, want; } cases[3] = {
        { "staging  (HV|HC)", VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT },
        { "readback (HV|HC|HCACHED)", VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT },
        { "buffer device-local", VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT } };
    for (int c = 0; c < 3; c++) {
        bi.usage = cases[c].usage;
        CHECK(vkCreateBuffer(dev, &bi, NULL, &b) == VK_SUCCESS, "vkCreateBuffer");
        vkGetBufferMemoryRequirements(dev, b, &rq);
        int t = pick(&mp, rq.memoryTypeBits, cases[c].want);
        if (t < 0 && c == 1) t = pick(&mp, rq.memoryTypeBits, cases[c].want & ~(VkFlags)VK_MEMORY_PROPERTY_HOST_CACHED_BIT);      /* the fallback in nvn_vk_draw.c */
        CHECK(t >= 0, "%s: no memory type (bits 0x%x)", cases[c].name, (unsigned)rq.memoryTypeBits);
        if (t < 0) { vkDestroyBuffer(dev, b, NULL); continue; }
        VkMemoryAllocateInfo mi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mi.allocationSize = 64u << 20; mi.memoryTypeIndex = (uint32_t)t;
        VkDeviceMemory mem; VkResult r = vkAllocateMemory(dev, &mi, NULL, &mem);
        CHECK(r == VK_SUCCESS, "%s: vkAllocateMemory 64 MB type %d: %d", cases[c].name, t, (int)r);
        if (r == VK_SUCCESS) {
            CHECK(vkBindBufferMemory(dev, b, mem, 0) == VK_SUCCESS, "bind");
            if (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
                volatile uint8_t *m = NULL;
                CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&m) == VK_SUCCESS && m, "map");
                if (m) { for (int i = 0; i < 4096; i++) m[i] = (uint8_t)(i * 7); int ok = 1; for (int i = 0; i < 4096; i++) if (m[i] != (uint8_t)(i * 7)) ok = 0; CHECK(ok, "host write/read"); vkUnmapMemory(dev, mem); }
            }
            vkFreeMemory(dev, mem, NULL);
        }
        printf("  %-26s -> type %d (flags 0x%x) memoryTypeBits 0x%x size %llu align %llu\n", cases[c].name, t, (unsigned)mp.memoryTypes[t].propertyFlags, (unsigned)rq.memoryTypeBits, (unsigned long long)rq.size, (unsigned long long)rq.alignment);
        vkDestroyBuffer(dev, b, NULL);
    }
    /* device-local optimal images: RGBA8 render target, BC7 and ASTC textures, D24S8 depth */
    struct { const char *n; VkFormat f; VkImageUsageFlags u; } im[] = {
        { "RGBA8 colour target", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT },
        { "BC7 texture", VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT },
        { "ASTC 6x6 texture", VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT },
        { "D24S8 depth", VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT } };
    for (int i = 0; i < 4; i++) {
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = im[i].f; ii.extent = (VkExtent3D){512, 512, 1};
        ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = im[i].u; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage img; VkResult r = vkCreateImage(dev, &ii, NULL, &img);
        CHECK(r == VK_SUCCESS, "%s: vkCreateImage %d", im[i].n, (int)r);
        if (r != VK_SUCCESS) continue;
        vkGetImageMemoryRequirements(dev, img, &rq);
        int t = pick(&mp, rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CHECK(t >= 0, "%s: no device-local type (bits 0x%x)", im[i].n, (unsigned)rq.memoryTypeBits);
        if (t >= 0) {
            VkMemoryAllocateInfo mi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mi.allocationSize = rq.size > (16u << 20) ? rq.size : (64u << 20); mi.memoryTypeIndex = (uint32_t)t;
            VkDeviceMemory mem; r = vkAllocateMemory(dev, &mi, NULL, &mem);
            CHECK(r == VK_SUCCESS, "%s: alloc %d", im[i].n, (int)r);
            if (r == VK_SUCCESS) { CHECK(vkBindImageMemory(dev, img, mem, 0) == VK_SUCCESS, "bind image"); vkFreeMemory(dev, mem, NULL); }
            printf("  image %-20s -> type %d (flags 0x%x) bits 0x%x size %llu\n", im[i].n, t, (unsigned)mp.memoryTypes[t].propertyFlags, (unsigned)rq.memoryTypeBits, (unsigned long long)rq.size);
        }
        vkDestroyImage(dev, img, NULL);
    }
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
    printf("vulkan: memory-type selection OK on %s\n", pr.deviceName);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/tmp/vkcore_test";
    mkdir(dir, 0755);
    test_store(dir);
    test_replace(dir);
    test_helpers(dir);
    test_vulkan();
    printf(fails ? "RESULT: %d FAILURES\n" : "RESULT: all vkcore tests passed\n", fails);
    return fails != 0;
}
