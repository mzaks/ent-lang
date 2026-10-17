// What a program built for a GPU (`ent build --gpu`, docs/gpu.md) runs
// on, over NVIDIA's driver (libcuda): the functions MLIR's lowering of
// kernel launches calls (mgpu*), the world's memory, and the CPU's waits.
//
// The world is in managed memory: one address on the CPU and the GPU, its
// pages moved by the driver to whichever touches them. Kernels are given
// one after another to one stream and not waited for: the compiler calls
// ent_gpu_wait where the CPU needs what they touch (and when a schedule
// ends), so the launch's own synchronize does nothing.
//
// ENT_GPU_MEM=host, where the program runs, puts the world in pinned host
// memory instead, which the GPU reads over PCIe every time (to measure
// what keeping it on the GPU is worth).

#include <cuda.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CUcontext context;
static CUstream stream;
// Whether kernels were given to the stream since the last wait.
static bool pending;

static void check(CUresult result, const char *what) {
  if (result == CUDA_SUCCESS)
    return;
  const char *name = "?";
  cuGetErrorName(result, &name);
  fprintf(stderr, "ent: the GPU: %s: %s\n", what, name);
  exit(3);
}

static void start(void) {
  if (context)
    return;
  check(cuInit(0), "cuInit");
  CUdevice device;
  check(cuDeviceGet(&device, 0), "cuDeviceGet");
  check(cuDevicePrimaryCtxRetain(&context, device), "cuDevicePrimaryCtxRetain");
  check(cuCtxSetCurrent(context), "cuCtxSetCurrent");
  check(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING), "cuStreamCreate");
}

void ent_gpu_wait(void) {
  if (!pending)
    return;
  // (An error of a kernel shows here, where the CPU first waits for it.)
  check(cuStreamSynchronize(stream), "a kernel");
  pending = false;
}

// The world and what else is big: in memory the GPU reaches. What is
// small (a sort's scratch) is the CPU's alone.
enum { kBig = 1 << 20, kMostBig = 64 };
static void *big[kMostBig];

static void *allocate(size_t alignment, size_t bytes) {
  if (bytes >= kBig) {
    start();
    void *memory = NULL;
    const char *kind = getenv("ENT_GPU_MEM");
    if (kind && !strcmp(kind, "host")) {
      check(cuMemHostAlloc(&memory, bytes, CU_MEMHOSTALLOC_DEVICEMAP),
            "cuMemHostAlloc");
    } else {
      CUdeviceptr managed;
      check(cuMemAllocManaged(&managed, bytes, CU_MEM_ATTACH_GLOBAL),
            "cuMemAllocManaged");
      memory = (void *)(uintptr_t)managed;
      // (Experiment, step 4: ENT_GPU_ADVISE=device keeps the pages on the
      // GPU, the CPU reaching them where they are.)
      const char *advise = getenv("ENT_GPU_ADVISE");
      if (advise && !strcmp(advise, "device")) {
        CUdevice device;
        check(cuCtxGetDevice(&device), "cuCtxGetDevice");
        CUmemLocation gpu = {CU_MEM_LOCATION_TYPE_DEVICE, device};
        CUmemLocation cpu = {CU_MEM_LOCATION_TYPE_HOST, 0};
        check(cuMemAdvise(managed, bytes, CU_MEM_ADVISE_SET_PREFERRED_LOCATION,
                          gpu), "cuMemAdvise");
        check(cuMemAdvise(managed, bytes, CU_MEM_ADVISE_SET_ACCESSED_BY, cpu),
              "cuMemAdvise");
      }
    }
    for (int i = 0; i < kMostBig; ++i)
      if (!big[i]) {
        big[i] = memory;
        return memory;
      }
    fprintf(stderr, "ent: the GPU: more than %d big blocks\n", kMostBig);
    exit(3);
  }
  void *memory = NULL;
  if (posix_memalign(&memory, alignment < sizeof(void *) ? sizeof(void *)
                                                         : alignment,
                     bytes))
    return NULL;
  return memory;
}

static void release(void *memory) {
  if (!memory)
    return;
  for (int i = 0; i < kMostBig; ++i)
    if (big[i] == memory) {
      ent_gpu_wait();
      big[i] = NULL;
      const char *kind = getenv("ENT_GPU_MEM");
      if (kind && !strcmp(kind, "host"))
        cuMemFreeHost(memory);
      else
        cuMemFree((CUdeviceptr)(uintptr_t)memory);
      return;
    }
  free(memory);
}

// The world of a C host (ent_world_create with ENT_GPU defined).
int ent_gpu_memalign(void **out, size_t alignment, size_t bytes) {
  *out = allocate(alignment, bytes);
  return *out ? 0 : 1;
}
void ent_gpu_free(void *memory) { release(memory); }

// What the program's own memory comes from (finalize-memref-to-llvm with
// use-generic-functions): `main`'s world among it.
void *_mlir_memref_to_llvm_alloc(size_t bytes) { return allocate(16, bytes); }
void *_mlir_memref_to_llvm_aligned_alloc(size_t alignment, size_t bytes) {
  return allocate(alignment, bytes);
}
void _mlir_memref_to_llvm_free(void *memory) { release(memory); }

// MLIR's launches. The kernels are PTX, compiled by the driver when the
// program loads them.
void *mgpuModuleLoadJIT(void *ptx, int level) {
  start();
  CUmodule module;
  CUjit_option options[] = {CU_JIT_OPTIMIZATION_LEVEL};
  void *values[] = {(void *)(intptr_t)level};
  check(cuModuleLoadDataEx(&module, ptx, 1, options, values), "loading kernels");
  return module;
}

void *mgpuModuleLoad(void *data, size_t size) {
  (void)size;
  start();
  CUmodule module;
  check(cuModuleLoadData(&module, data), "loading kernels");
  return module;
}

void mgpuModuleUnload(void *module) { cuModuleUnload((CUmodule)module); }

void *mgpuModuleGetFunction(void *module, const char *name) {
  CUfunction function;
  check(cuModuleGetFunction(&function, (CUmodule)module, name), name);
  return function;
}

// The world's stream, for every launch.
void *mgpuStreamCreate(void) {
  start();
  return stream;
}
void mgpuStreamSynchronize(void *given) { (void)given; }
void mgpuStreamDestroy(void *given) { (void)given; }

void mgpuLaunchKernel(void *function, intptr_t gridX, intptr_t gridY,
                      intptr_t gridZ, intptr_t blockX, intptr_t blockY,
                      intptr_t blockZ, int32_t sharedBytes, void *given,
                      void **params, void **extra, size_t paramsCount) {
  (void)given;
  (void)paramsCount;
  // (No blocks: a loop over no entities.)
  if (gridX == 0 || gridY == 0 || gridZ == 0)
    return;
  check(cuLaunchKernel((CUfunction)function, gridX, gridY, gridZ, blockX,
                       blockY, blockZ, sharedBytes, stream, params, extra),
        "cuLaunchKernel");
  pending = true;
}
