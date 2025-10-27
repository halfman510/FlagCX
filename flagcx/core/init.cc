/*************************************************************************
 * Copyright (c) 2015-2022, NVIDIA CORPORATION. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#include "adaptor.h"
#include "bootstrap.h"
#include "check.h"
#include "collectives.h"
#include "flagcx.h"
#include "group.h"
#include "net.h"
#include "topo.h"
#include "transport.h"
#include "type.h"
#include <string.h>

static bool initialized = false;
pthread_mutex_t initLock = PTHREAD_MUTEX_INITIALIZER;

struct flagcxCommInitRankAsyncJob {
  struct flagcxAsyncJob base;
  struct flagcxHeteroComm *comm;
  struct flagcxHeteroComm **newcomm;
  int cudaDev;
  // For flagcxCommInitRank
  int nranks, myrank;
  flagcxUniqueId commId;
  // for flagcxCommSplit
  struct flagcxHeteroComm *parent;
  int color, key;
};

flagcxResult_t flagcxHeteroGetVersion(int *version) {
  if (version == NULL)
    return flagcxInvalidArgument;
  *version = FLAGCX_VERSION(1, 0, 0);
  return flagcxSuccess;
}

static flagcxResult_t flagcxInit() {
  if (__atomic_load_n(&initialized, __ATOMIC_ACQUIRE))
    return flagcxSuccess;
  pthread_mutex_lock(&initLock);
  if (!initialized) {
    // FLAGCXCHECK(loadDeviceSymbol());
    FLAGCXCHECK(bootstrapNetInit());
    // FLAGCXCHECK(flagcxNetPluginInit());
    __atomic_store_n(&initialized, true, __ATOMIC_RELEASE);
  }
  pthread_mutex_unlock(&initLock);
  return flagcxSuccess;
}

flagcxResult_t flagcxHeteroGetUniqueId(flagcxUniqueId *out) {
  FLAGCXCHECK(flagcxInit());
  flagcxResult_t res =
      bootstrapGetUniqueId((struct flagcxBootstrapHandle *)out);
  return res;
}

static uint64_t hashUniqueId(flagcxUniqueId const &id) {
  char const *bytes = (char const *)&id;
  uint64_t h = 0xdeadbeef;
  for (int i = 0; i < (int)sizeof(flagcxUniqueId); i++) {
    h ^= h >> 32;
    h *= 0x8db3db47fa2994ad;
    h += bytes[i];
  }
  return h;
}

static flagcxResult_t fillPeerInfo(flagcxHeteroComm_t comm,
                                   struct flagcxPeerInfo *info,
                                   uint64_t commHash) {
  info->rank = comm->rank;
  info->cudaDev = comm->cudaDev;
  info->hostHash = getHostHash() + commHash;
  info->pidHash = getPidHash() + commHash;
  info->busId = comm->busId;
  info->comm = comm;

  return flagcxSuccess;
}

//异构通信初始化和拓扑检测
//每个rank初始化传输层
static flagcxResult_t initTransportsRank(flagcxHeteroComm_t comm,
                                         flagcxHeteroComm_t parent) {
  INFO(FLAGCX_INIT, "inside initTransportsRank");
  flagcxResult_t ret = flagcxSuccess;
  int rank = comm->rank;
  int nranks = comm->nRanks;
  int nNodes = 1;

  //人口普查，让每个进程都了解其它所有进程的基础信息
  // fill peer info
  FLAGCXCHECKGOTO(flagcxCalloc(&comm->peerInfo, nranks), ret, fail);//flagcxCalloc分配能容纳nranks个进程信息的内存
  INFO(FLAGCX_INIT, "start fillPeerInfo");
  FLAGCXCHECKGOTO(fillPeerInfo(comm, comm->peerInfo + rank, comm->commHash),
                  ret, fail);//fillPeerInfo每个进程只填充自己的信息，comm->peerInfo + rank是自己的rank位置
  // Question: where did we initialize comm->bootstrap?
  INFO(FLAGCX_INIT, "start bootstrapAllGather for peerInfo");
  FLAGCXCHECKGOTO(bootstrapAllGather(comm->bootstrap, (void *)comm->peerInfo,
                                     sizeof(struct flagcxPeerInfo)),
                  ret, fail);//AllGather操作使得所有进程把自己填充的小块信息进行交换，完成后comm->peerInfo数组在每个进程上都被完整填充，实现了信息对称
  FLAGCXCHECKGOTO(bootstrapBarrier(comm->bootstrap, rank, nranks, 0), ret,
                  fail);//bootstrapBarrier同步，确保所有进程都完成了信息交换

  // check for duplicate GPUs
  //健壮性检查，检查刚刚的全局peerInfo列表，是否有两个进程在同一个机器上使用同一个物理GPU
  INFO(FLAGCX_INIT, "start check for duplicate GPUs");
  for (int i = 0; i < nranks; i++) {
    if (comm->peerInfo[i].hostHash != comm->peerInfo[rank].hostHash)
      nNodes++;
    if ((i != rank) &&
        (comm->peerInfo[i].hostHash == comm->peerInfo[rank].hostHash) &&
        (comm->peerInfo[i].busId == comm->peerInfo[rank].busId)) {
      WARN("Duplicate GPU detected : rank %d and rank %d both on CUDA device "
           "%lx",
           rank, i, comm->peerInfo[rank].busId);//同一台机器是hostHash，同一个物理GPU是busId
      ret = flagcxInvalidUsage;
      goto fail;
    }
  }

  INFO(FLAGCX_INIT, "start flagcxTopoGetServerTopo");
  FLAGCXCHECKGOTO(flagcxTopoGetServerTopo(comm, &comm->topoServer), ret, fail);//获取服务器内部拓扑
  FLAGCXCHECKGOTO(flagcxTopoComputePaths(comm->topoServer, comm), ret, fail);//计算内部路径
  if (comm->rank == 0) {
    FLAGCXCHECK(flagcxTopoPrint(comm->topoServer));
  }
  INFO(FLAGCX_INIT, "start getting local net from gpu");
  FLAGCXCHECKGOTO(flagcxGetLocalNetFromGpu(comm->cudaDev, &comm->netDev, comm),
                  ret, fail);//为GPU匹配最佳网卡，找出与当前GPU物理连接最近的网络接口

  INFO(FLAGCX_INIT, "start getting topoServer from other servers");
  FLAGCXCHECKGOTO(
      flagcxGetInterServerTopo(comm, &comm->interServerTopo, comm->topoServer),
      ret, fail);//获取服务器间拓扑

  return ret;
fail:
  return flagcxInternalError;
}

//异构模式下为即将到来的复杂通信搭建的过程
static flagcxResult_t flagcxCommInitRankFunc(struct flagcxAsyncJob *job_) {//参数是结构体，可能是被后台线程池调用，用于执行异步初始化任务，避免阻塞主线程的操作
  struct flagcxCommInitRankAsyncJob *job =
      (struct flagcxCommInitRankAsyncJob *)job_;
  flagcxHeteroComm_t comm = job->comm;
  flagcxResult_t res = flagcxSuccess;
  const char *env = flagcxGetEnv("FLAGCX_ENABLE_TOPO_DETECT");

  //全新的顶层通信器，初始化bootstrap引导模块
  if (!job->parent) {//判断说明通信器之间应该有层级关系，如果是个全新的顶层通信器，就进入代码块初始化；如果是子通信器就跳过，可能复用父通信器的状态
    // New version of calling bootstrapInit
    struct bootstrapState *state;
    FLAGCXCHECK(flagcxCalloc(&state, 1));
    state->rank = comm->rank;
    state->nranks = comm->nRanks;
    state->abortFlag = comm->abortFlag;
    comm->bootstrap = state;
    state->magic = ((struct flagcxBootstrapHandle *)&job->commId)->magic;
    comm->magic = ((struct flagcxBootstrapHandle *)&job->commId)->magic;
    FLAGCXCHECKGOTO(
        bootstrapInit((struct flagcxBootstrapHandle *)&job->commId, state), res,
        fail);//传入参数Unique Id，把当前进程加入基础通信网络，为后续传输信息做准备
  }

  //给代理服务和异步执行分配和初始化所需的数据结构
  if (!job->parent) {
    // Setting up proxy network
    int nranks = comm->nRanks;
    for (int i = 0; i < MAXCHANNELS; i++) {
      FLAGCXCHECK(flagcxCalloc(&comm->channels[i].peers, nranks));//comm->channels为通信信道分配内存
      for (int r = 0; r < nranks; r++)
        FLAGCXCHECK(flagcxCalloc(&comm->channels[i].peers[r], nranks));//comm->channels是一个三维数组，大概是管理点对点连接
    }
    FLAGCXCHECK(flagcxCalloc(&comm->connectSend, nranks));
    FLAGCXCHECK(flagcxCalloc(&comm->connectRecv, nranks));
    FLAGCXCHECK(flagcxCalloc(&comm->proxyState, 1));
    FLAGCXCHECK(flagcxCalloc(&comm->tasks.peers, nranks));
    FLAGCXCHECK(flagcxCalloc(&comm->tasks.p2pOrder, 2 * nranks));
    // Setup mutex/cond to work inter-process
    pthread_mutexattr_t mutexAttr;//创建互斥锁
    pthread_mutexattr_init(&mutexAttr);
    pthread_mutexattr_setpshared(&mutexAttr, PTHREAD_PROCESS_SHARED);//PTHREAD_PROCESS_SHARED说明创建的互斥锁可以跨进程使用
    pthread_mutex_init(&comm->proxyState->mutex, &mutexAttr);//comm->proxyState初始化状态代理机，这里的代理可能是同一物理服务器上多个进程共享一个内存来进行通信，跨进程锁保护共享内存不会读写冲突
    pthread_condattr_t condAttr;//创建条件变量
    pthread_condattr_init(&condAttr);
    pthread_condattr_setpshared(&condAttr, PTHREAD_PROCESS_SHARED);//条件变量也可以跨进程使用
    pthread_cond_init(&comm->proxyState->cond, &condAttr);

    for (int i = 0; i < MAXCHANNELS; i++) {
      FLAGCXCHECK(
          flagcxCalloc(&comm->proxyState->proxyOps[i].consPeers, nranks));//初始化每个通道的代理操作队列
      comm->proxyState->proxyOps[i].consNextChannel =
          reinterpret_cast<struct flagcxProxyOps *>(0x1);//consNextChannel下一个消费者通道
      comm->proxyState->proxyOps[i].prodNextChannel =
          reinterpret_cast<struct flagcxProxyOps *>(0x1);//prodNextChannel下一个生产者通道
      pthread_mutex_init(&comm->proxyState->proxyOps[i].mutex, 0);
      for (int peer = 0; peer < nranks; peer++) {
        comm->proxyState->proxyOps[i].consPeers[peer].nextPeer =
            reinterpret_cast<struct flagcxProxyOps::consPeer *>(0x1);//构建多生产者多消费者环
      }
    }

    comm->groupNext = reinterpret_cast<struct flagcxHeteroComm *>(0x1);
    comm->preconnectNext = reinterpret_cast<struct flagcxHeteroComm *>(0x1);
    comm->proxyState->nRanks = comm->nRanks;

    bool runtimeProxy = false;
    const char *runtimeEnv = flagcxGetEnv("FLAGCX_RUNTIME_PROXY");//检查环境变量，用户可以在运行时决定是否启动代理服务
    if (runtimeEnv) {
      runtimeProxy = (std::stoi(runtimeEnv) == 1) ? true : false;
    }
    INFO(FLAGCX_INIT, "Flagcx RuntimeProxy flag set to %d", runtimeProxy);
    if (!runtimeProxy) {
      FLAGCXCHECK(flagcxProxyInit(comm));//如果为false，调用这个函数正式启动代理服务（可能创建后台线程）
    }
  }
  //网络适配器初始化与拓扑检测
  FLAGCXCHECK(flagcxNetInit(comm));//初始化网络适配器
  INFO(FLAGCX_INIT, "Using network %s", comm->netAdaptor->name);
  if (env && strcmp(env, "TRUE") == 0) {
    INFO(FLAGCX_INIT, "getting busId for cudaDev %d", comm->cudaDev);
    FLAGCXCHECK(getBusId(comm->cudaDev, &comm->busId));//获取设备的PCI bus ID
    INFO(FLAGCX_INIT, "getting commHash for rank %d", comm->rank);
    comm->commHash = getHash(job->commId.internal, FLAGCX_UNIQUE_ID_BYTES);//计算通信器哈希值
    INFO(FLAGCX_INIT, "commHash for rank %d is %lu", comm->rank,
         comm->commHash);
    // TODO: put net init into a separate function

    INFO(FLAGCX_INIT, "start initTransportsRank");
    FLAGCXCHECKGOTO(initTransportsRank(comm, NULL), res, fail);//调用函数，每个rank初始化传输层
  } else {
    flagcxGetLocalNetFromGpu(comm->cudaDev, &comm->netDev, comm);
  }

exit:
  return res;
fail:
  comm->initState = res;
  goto exit;
}

static flagcxResult_t flagcxCommInitRankDev(flagcxHeteroComm_t *newcomm,
                                            int nranks, flagcxUniqueId commId,
                                            int myrank, int cudaDev,
                                            flagcxConfig_t *config) {
  flagcxResult_t res = flagcxSuccess;
  flagcxHeteroComm_t comm = NULL;
  struct flagcxCommInitRankAsyncJob *job = NULL;
  const char *env = flagcxGetEnv("FLAGCX_COMM_ID");

  if (env && myrank == 0) {
    INFO(FLAGCX_ENV, "FLAGCX_COMM_ID set by environment to %s", env);
    FLAGCXCHECKGOTO(
        bootstrapCreateRoot((struct flagcxBootstrapHandle *)&commId, true), res,
        fail);
  }

  if (nranks < 1 || myrank < 0 || myrank >= nranks) {
    WARN("Invalid rank requested : %d/%d", myrank, nranks);
    res = flagcxInvalidArgument;
    goto fail;
  }

  FLAGCXCHECKGOTO(flagcxCalloc(&comm, 1), res, fail);
  comm->startMagic = comm->endMagic =
      FLAGCX_MAGIC; // Used to detect comm corruption.
  FLAGCXCHECKGOTO(flagcxCalloc((uint32_t **)&comm->abortFlagRefCount, 1), res,
                  fail);
  *comm->abortFlagRefCount = 1;
  /* start with flagcxInternalError and will be changed to flagcxSuccess if init
   * succeeds. */
  comm->initState = flagcxInternalError;
  comm->nRanks = nranks;
  comm->rank = myrank;
  comm->cudaDev = cudaDev;
  *newcomm = comm;

  FLAGCXCHECKGOTO(flagcxCalloc(&job, 1), res, fail);
  job->comm = comm;
  job->nranks = nranks;
  job->commId = commId; // C++ struct assignment
  job->myrank = myrank;
  job->cudaDev = cudaDev;
  FLAGCXCHECKGOTO(flagcxCommInitRankFunc(&job->base), res, fail);
  free(job);
exit:
  return flagcxGroupErrCheck(res);
fail:
  if (comm) {
    if (comm->abortFlagRefCount)
      free(comm->abortFlagRefCount);
    free(comm);
  }
  if (newcomm)
    *newcomm = NULL;
  goto exit;
}

flagcxResult_t flagcxHeteroCommInitRank(flagcxHeteroComm_t *newcomm, int nranks,
                                        flagcxUniqueId commId, int myrank) {
  FLAGCXCHECK(flagcxInit());
  int cudaDev = 0;
  flagcxConfig_t config;
  // flagcxGetDevice(&cudaDev);
  deviceAdaptor->getDevice(&cudaDev);
  FLAGCXCHECK(
      flagcxCommInitRankDev(newcomm, nranks, commId, myrank, cudaDev, &config));
  return flagcxSuccess;
}

flagcxResult_t flagcxHeteroCommCount(const flagcxHeteroComm_t comm,
                                     int *count) {
  *count = comm->nRanks;
  return flagcxSuccess;
}

flagcxResult_t flagcxHeteroCommUserRank(const flagcxHeteroComm_t comm,
                                        int *rank) {
  *rank = comm->rank;
  return flagcxSuccess;
}

flagcxResult_t flagcxHeteroCommDestroy(flagcxHeteroComm_t comm) {
  flagcxProxyDestroy(comm);
  for (int i = 0; i < MAXCHANNELS; i++) {
    for (int r = 0; r < comm->nRanks; r++) {
      free(comm->channels[i].peers[r]);
    }
    free(comm->channels[i].peers);
  }
  for (int i = 0; i < MAXCHANNELS; i++) {
    free(comm->proxyState->proxyOps[i].consPeers);
  }

  free(comm->connectSend);
  free(comm->connectRecv);
  free(comm->proxyState);
  free(comm->tasks.peers);
  free(comm->tasks.p2pOrder);
  free(comm->abortFlagRefCount);
  if (comm->topoServer) {
    flagcxTopoFree(comm->topoServer);
  }
  if (comm->interServerTopo) {
    flagcxInterServerTopoFree(comm->interServerTopo);
  }
  free(comm->peerInfo);
  free(comm);

  return flagcxSuccess;
}
