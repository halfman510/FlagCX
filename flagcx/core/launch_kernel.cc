#include "launch_kernel.h"
#include "group.h"
#include <stdio.h>

flagcxLaunchFunc_t deviceAsyncLoad = NULL;
flagcxLaunchFunc_t deviceAsyncStore = NULL;

FLAGCX_PARAM(FuncMaxSpinCount, "FUNC_MAX_SPIN_COUNT", INT64_MAX);
static int64_t funcMaxSpinCount = flagcxParamFuncMaxSpinCount();

//符号函数加载
flagcxResult_t loadKernelSymbol(const char *path, const char *name,
                                flagcxLaunchFunc_t *fn) {//path是.so文件的路径，fn是函数指针的地址
  //dlopen用来加载共享库的核心函数，RTLD_LAZY是懒加载模式，只有当某个函数第一次被实际调用时，才去解析它的地址，
  // 而不是dlopen的时候就解析所有函数，这样可以加快速度，成功会返回handl句柄
  void *handle = flagcxOpenLib(
      path, RTLD_LAZY, [](const char *p, int err, const char *msg) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
      });

  //不为空时，dlsym找到动态符号对应的地址，返回void类型指针，该指针指向函数在内存中的实际地址，返回的void类型无法直接
  // 调用，先通过类型转换cast转换成期望的函数指针类型flagcxLaunchFunc_t，把转换后类型正确的函数指针赋值给调用者传入的
  // fn指针。
  if (!handle)
    return flagcxSystemError;

  void *sym = dlsym(handle, name);
  if (!sym) {
    fprintf(stderr, "dlsym failed: %s\n", dlerror());
    return flagcxSystemError;
  }

  *fn = (flagcxLaunchFunc_t)sym;
  return flagcxSuccess;
}

void cpuAsyncStore(void *args) {
  bool *volatile value = (bool *)args;
  __atomic_store_n(value, 1, __ATOMIC_RELAXED);
}

void cpuAsyncLoad(void *args) {
  bool *volatile value = (bool *)args;
  while (!__atomic_load_n(value, __ATOMIC_RELAXED)) {
  }
}

void cpuAsyncLoadWithMaxSpinCount(void *args) {
  bool *volatile value = (bool *)args;
  int64_t spinCount = 0;
  while (!__atomic_load_n(value, __ATOMIC_RELAXED) &&
         spinCount < funcMaxSpinCount) {
    spinCount++;
  }
}