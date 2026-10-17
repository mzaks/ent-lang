// What the driver costs: N empty kernels and one synchronize, and whether
// the CPU writing a managed page the GPU does not touch costs anything.
//   cc -O2 -I/opt/cuda/include probe_launch.c -lcuda && ./a.out
#include <cuda.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}
#define C(x) do{CUresult r=(x); if(r){const char*s;cuGetErrorName(r,&s);printf("%s: %s\n",#x,s);exit(1);}}while(0)
int main(void){
  CUcontext ctx; CUdevice d; CUmodule m; CUfunction e; CUstream s;
  C(cuInit(0)); C(cuDeviceGet(&d,0)); C(cuDevicePrimaryCtxRetain(&ctx,d)); C(cuCtxSetCurrent(ctx));
  const char *p2 = ".version 8.0\n.target sm_89\n.address_size 64\n.visible .entry empty(.param .u64 p) { ret; }\n";
  C(cuModuleLoadData(&m,p2)); C(cuModuleGetFunction(&e,m,"empty"));
  C(cuStreamCreate(&s,CU_STREAM_NON_BLOCKING));
  CUdeviceptr mem; C(cuMemAllocManaged(&mem, 1<<26, CU_MEM_ATTACH_GLOBAL));
  void *args[]={&mem};
  for (int launches = 0; launches <= 4; ++launches) {
    double best=1e18;
    for (int rep=0; rep<2000; ++rep){
      double t0=now();
      for (int k=0;k<launches;++k) C(cuLaunchKernel(e,40,1,1,256,1,1,0,s,args,0));
      C(cuStreamSynchronize(s));
      double dt=now()-t0; if(dt<best)best=dt;
    }
    printf("%d launches + sync: best %.1f us\n", launches, best/1000);
  }
  // host write to managed memory between frames
  for (int touch=0; touch<2; ++touch){
    double best=1e18;
    for(int rep=0;rep<2000;++rep){
      double t0=now();
      if (touch) ((volatile char*)mem)[1<<25] += 1;
      for (int k=0;k<3;++k) C(cuLaunchKernel(e,40,1,1,256,1,1,0,s,args,0));
      C(cuStreamSynchronize(s));
      double dt=now()-t0; if(dt<best)best=dt;
    }
    printf("3 launches, host writes managed=%d: %.1f us\n", touch, best/1000);
  }
  // spin-wait sync instead of default
  return 0;
}
