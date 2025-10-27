/*************************************************************************
 * Copyright (c) 2025 by MetaX Integrated Circuits (Shanghai) Co., Ltd. All
 *Rights Reserved. Copyright (c) 2025 by DU. All Rights Reserved.
 ************************************************************************/

#include "adaptor.h"
#include "core.h"
#include "net.h"
#include <string.h>
/*************************************************************************
 条件编译：在C++源码被真正的编译器g++编译成机器码之前，经过预处理器，如果#后面的宏被定义了，就保留这部分代码，交给编译器
 ************************************************************************/
#ifdef USE_NVIDIA_ADAPTOR
#ifdef USE_BOOTSTRAP_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&bootstrapAdaptor,
                                                      &ncclAdaptor};
#elif USE_GLOO_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&glooAdaptor,
                                                      &ncclAdaptor};
#elif USE_MPI_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&mpiAdaptor,
                                                      &ncclAdaptor};
#endif
struct flagcxDeviceAdaptor *deviceAdaptor = &cudaAdaptor;
#elif USE_ASCEND_ADAPTOR
#ifdef USE_BOOTSTRAP_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&bootstrapAdaptor,
                                                      &hcclAdaptor};
#elif USE_GLOO_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&glooAdaptor,
                                                      &hcclAdaptor};
#elif USE_MPI_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&mpiAdaptor,
                                                      &hcclAdaptor};
#endif
struct flagcxDeviceAdaptor *deviceAdaptor = &cannAdaptor;
#elif USE_ILUVATAR_COREX_ADAPTOR
#ifdef USE_BOOTSTRAP_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&bootstrapAdaptor,
                                                      &ixncclAdaptor};
#elif USE_GLOO_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&glooAdaptor,
                                                      &ixncclAdaptor};
#elif USE_MPI_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&mpiAdaptor,
                                                      &ixncclAdaptor};
#endif
struct flagcxDeviceAdaptor *deviceAdaptor = &ixcudaAdaptor;
#elif USE_CAMBRICON_ADAPTOR
#ifdef USE_BOOTSTRAP_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&bootstrapAdaptor,
                                                      &cnclAdaptor};
#elif USE_GLOO_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&glooAdaptor,
                                                      &cnclAdaptor};
#elif USE_MPI_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&mpiAdaptor,
                                                      &cnclAdaptor};
#endif
struct flagcxDeviceAdaptor *deviceAdaptor = &mluAdaptor;
#elif USE_METAX_ADAPTOR
#ifdef USE_BOOTSTRAP_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&bootstrapAdaptor,
                                                      &mcclAdaptor};
#elif USE_GLOO_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&glooAdaptor,
                                                      &mcclAdaptor};
#elif USE_MPI_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&mpiAdaptor,
                                                      &mcclAdaptor};
#endif
struct flagcxDeviceAdaptor *deviceAdaptor = &macaAdaptor;

#elif USE_MUSA_ADAPTOR
#ifdef USE_BOOTSTRAP_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&bootstrapAdaptor,
                                                      &musa_mcclAdaptor};
#elif USE_GLOO_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&glooAdaptor,
                                                      &musa_mcclAdaptor};
#elif USE_MPI_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&mpiAdaptor,
                                                      &musa_mcclAdaptor};
#endif
struct flagcxDeviceAdaptor *deviceAdaptor = &musaAdaptor;

#elif USE_KUNLUNXIN_ADAPTOR
#ifdef USE_BOOTSTRAP_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&bootstrapAdaptor,
                                                      &xcclAdaptor};
#elif USE_GLOO_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&glooAdaptor,
                                                      &xcclAdaptor};
#elif USE_MPI_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&mpiAdaptor,
                                                      &xcclAdaptor};
#endif
struct flagcxDeviceAdaptor *deviceAdaptor = &kunlunAdaptor;
#elif USE_DU_ADAPTOR
#ifdef USE_BOOTSTRAP_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&bootstrapAdaptor,
                                                      &duncclAdaptor};
#elif USE_GLOO_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&glooAdaptor,
                                                      &duncclAdaptor};
#elif USE_MPI_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&mpiAdaptor,
                                                      &duncclAdaptor};
#endif
struct flagcxDeviceAdaptor *deviceAdaptor = &ducudaAdaptor;
#elif USE_AMD_ADAPTOR
#ifdef USE_BOOTSTRAP_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&bootstrapAdaptor,
                                                      &rcclAdaptor};
#elif USE_GLOO_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&glooAdaptor,
                                                      &rcclAdaptor};
#elif USE_MPI_ADAPTOR
struct flagcxCCLAdaptor *cclAdaptors[NCCLADAPTORS] = {&mpiAdaptor,
                                                      &rcclAdaptor};
#endif
struct flagcxDeviceAdaptor *deviceAdaptor = &hipAdaptor;
#endif

// External adaptor declarations
extern struct flagcxNetAdaptor flagcxNetSocket;
extern struct flagcxNetAdaptor flagcxNetIb;

#ifdef USE_UCX
extern struct flagcxNetAdaptor flagcxNetUcx;
#endif

// Unified network adaptor entry point
struct flagcxNetAdaptor *getUnifiedNetAdaptor(int netType) {
  switch (netType) {
    case IBRC:
#ifdef USE_UCX
      // When UCX is enabled, use UCX instead of IBRC
      return &flagcxNetUcx;
#else
      return &flagcxNetIb;
#endif
    case SOCKET:
      return &flagcxNetSocket;
    default:
      return NULL;
  }
}
