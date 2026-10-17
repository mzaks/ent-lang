// bench/gpu (G0): the runtime calls MLIR's GPU lowering emits (mgpu*), over the CUDA
// driver API, and the world's arena in memory the GPU can reach.
// ENT_GPU_MEM=managed (default): unified memory, migrates to the GPU and
// stays there while only the GPU touches it. ENT_GPU_MEM=host: pinned host
// memory the GPU reads over PCIe on every access (no residency at all).

#include <cuda.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CUcontext context;

static void check(CUresult r, const char *what) {
  if (r != CUDA_SUCCESS) {
    const char *name = "?";
    cuGetErrorName(r, &name);
    fprintf(stderr, "%s: %s\n", what, name);
    exit(3);
  }
}

static void init(void) {
  if (context)
    return;
  check(cuInit(0), "cuInit");
  CUdevice device;
  check(cuDeviceGet(&device, 0), "cuDeviceGet");
  check(cuDevicePrimaryCtxRetain(&context, device), "cuDevicePrimaryCtxRetain");
  check(cuCtxSetCurrent(context), "cuCtxSetCurrent");
}

// Replaces posix_memalign in the generated ent_world_create.
int ent_gpu_memalign(void **out, size_t alignment, size_t bytes) {
  (void)alignment; // both kinds are at least page aligned
  init();
  const char *kind = getenv("ENT_GPU_MEM");
  if (kind && !strcmp(kind, "host")) {
    check(cuMemHostAlloc(out, bytes, CU_MEMHOSTALLOC_DEVICEMAP), "cuMemHostAlloc");
  } else {
    CUdeviceptr p;
    check(cuMemAllocManaged(&p, bytes, CU_MEM_ATTACH_GLOBAL), "cuMemAllocManaged");
    *out = (void *)(uintptr_t)p;
  }
  return 0;
}

void *mgpuModuleLoadJIT(void *ptx, int optLevel) {
  init();
  CUmodule module;
  CUjit_option options[] = {CU_JIT_OPTIMIZATION_LEVEL};
  void *values[] = {(void *)(intptr_t)optLevel};
  check(cuModuleLoadDataEx(&module, ptx, 1, options, values), "cuModuleLoadDataEx");
  return module;
}

void mgpuModuleUnload(void *module) { cuModuleUnload((CUmodule)module); }

void *mgpuModuleGetFunction(void *module, const char *name) {
  CUfunction f;
  check(cuModuleGetFunction(&f, (CUmodule)module, name), "cuModuleGetFunction");
  return f;
}

void *mgpuStreamCreate(void) {
  init();
  CUstream s;
  check(cuStreamCreate(&s, CU_STREAM_NON_BLOCKING), "cuStreamCreate");
  return s;
}

void mgpuStreamSynchronize(void *s) {
  check(cuStreamSynchronize((CUstream)s), "cuStreamSynchronize");
}

void mgpuStreamDestroy(void *s) { cuStreamDestroy((CUstream)s); }

void mgpuLaunchKernel(void *f, intptr_t gx, intptr_t gy, intptr_t gz,
                      intptr_t bx, intptr_t by, intptr_t bz, int32_t smem,
                      void *stream, void **params, void **extra,
                      size_t paramsCount) {
  (void)paramsCount;
  check(cuLaunchKernel((CUfunction)f, gx, gy, gz, bx, by, bz, smem,
                       (CUstream)stream, params, extra),
        "cuLaunchKernel");
}
