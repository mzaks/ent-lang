// A million floats the GPU adds to and the CPU then reads, every frame:
// managed memory moved by faults, by prefetches both ways, advised read
// mostly, against the GPU's own memory copied to pinned host memory.
//   cc -O2 -march=native -I/opt/cuda/include probe_readback.c -lcuda && ./a.out
#include <cuda.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static const char *ptx =
".version 8.0\n.target sm_89\n.address_size 64\n"
".visible .entry bump(.param .u64 p, .param .u32 n) {\n"
" .reg .u64 a, o; .reg .u32 i, b, t, m; .reg .f32 f; .reg .pred q;\n"
" ld.param.u64 a, [p]; ld.param.u32 m, [n];\n"
" mov.u32 b, %ctaid.x; mov.u32 t, %tid.x; mad.lo.u32 i, b, 256, t;\n"
" setp.ge.u32 q, i, m; @q ret;\n"
" mul.wide.u32 o, i, 4; add.u64 a, a, o; ld.global.f32 f, [a]; add.f32 f, f, 0f3F800000; st.global.f32 [a], f; ret; }\n";
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}
#define C(x) do{CUresult r=(x); if(r){const char*s;cuGetErrorName(r,&s);printf("%s: %s\n",#x,s);exit(1);}}while(0)
int main(void){
  CUcontext ctx; CUdevice d; CUmodule m; CUfunction k; CUstream s;
  C(cuInit(0)); C(cuDeviceGet(&d,0)); C(cuDevicePrimaryCtxRetain(&ctx,d)); C(cuCtxSetCurrent(ctx));
  C(cuModuleLoadData(&m,ptx)); C(cuModuleGetFunction(&k,m,"bump"));
  C(cuStreamCreate(&s,CU_STREAM_NON_BLOCKING));
  unsigned n = 1000000; size_t bytes = n*4ul;
  CUmemLocation gpu={CU_MEM_LOCATION_TYPE_DEVICE,d}, cpu={CU_MEM_LOCATION_TYPE_HOST,0};
  for (int mode=0; mode<5; ++mode){
    CUdeviceptr mem; C(cuMemAllocManaged(&mem, 1<<24, CU_MEM_ATTACH_GLOBAL));
    float *h=(float*)mem; for(unsigned i=0;i<n;++i)h[i]=0;
    if(mode>=3) C(cuMemAdvise_v2(mem,bytes,CU_MEM_ADVISE_SET_READ_MOSTLY,gpu));
    void *args[]={&mem,&n}; double sum=0, t0=0; int frames=200;
    for(int f=0; f<frames+10; ++f){
      if(f==10)t0=now();
      if(mode==1) C(cuMemPrefetchAsync_v2(mem,bytes,gpu,0,s));
      C(cuLaunchKernel(k,(n+255)/256,1,1,256,1,1,0,s,args,0));
      if(mode==1||mode==4) C(cuMemPrefetchAsync_v2(mem,bytes,cpu,0,s));
      C(cuStreamSynchronize(s));
      if(mode==2){ /* explicit copy into host buffer */ }
      for(unsigned i=0;i<n;++i)sum+=h[i];
    }
    printf("%s: %.3f ms/frame (sum %.0f)\n", mode==0?"faults":mode==1?"prefetch both ways":mode==2?"faults again":mode==3?"read-mostly, faults":"read-mostly, prefetch to cpu", (now()-t0)/frames/1e6, sum);
    cuMemFree(mem);
  }
  // explicit device buffer + copy back
  CUdeviceptr dev; C(cuMemAlloc(&dev, bytes)); float *host; C(cuMemAllocHost((void**)&host, bytes));
  C(cuMemsetD32(dev,0,n)); void *args[]={&dev,&n}; double sum=0,t0=0; int frames=200;
  for(int f=0; f<frames+10; ++f){ if(f==10)t0=now();
    C(cuLaunchKernel(k,(n+255)/256,1,1,256,1,1,0,s,args,0));
    C(cuMemcpyDtoHAsync(host,dev,bytes,s)); C(cuStreamSynchronize(s));
    for(unsigned i=0;i<n;++i)sum+=host[i]; }
  printf("device + copy to pinned: %.3f ms/frame (sum %.0f)\n",(now()-t0)/frames/1e6,sum);
  return 0;
}
