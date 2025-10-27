/*************************************************************************
 * Copyright (c) 2016-2022, NVIDIA CORPORATION. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#include "bootstrap.h"
#include "alloc.h"
#include "check.h"
#include "comm.h"
#include "debug.h"
#include "param.h"
#include "utils.h"
#include <sys/types.h>
#include <unistd.h>
#include <vector>

struct bootstrapRootArgs {
  struct flagcxSocket *listenSock;
  uint64_t magic;
};

/* Init functions */
static char bootstrapNetIfName[MAX_IF_NAME_SIZE + 1];
union flagcxSocketAddress bootstrapNetIfAddr;
static int bootstrapNetInitDone = 0;
pthread_mutex_t bootstrapNetLock = PTHREAD_MUTEX_INITIALIZER;

//所有进程中选择一个用于bootstrap初始协调的网络接口NIC，并准备好地址信息
//解决初次握手问题
flagcxResult_t bootstrapNetInit() {
  if (bootstrapNetInitDone == 0) {//无锁的快速检查，如果初始化已经完成，函数直接返回，开销极小
    pthread_mutex_lock(&bootstrapNetLock);//如果可能需要初始化，就获取互斥锁
    if (bootstrapNetInitDone == 0) {//第二次检查，获取锁之后再次检查标记位，为了防止A等待锁的时候，B已经完成初始化；如果没有第二次检查，线程A获取锁之后会重复初始化导致错误。
      const char *env = flagcxGetEnv("FLAGCX_COMM_ID");//这个环境变量是根进程rank0的地址IP+端口
      if (env) {//指定了集结点的情况，FLAGCX_COMM_ID不为空
        union flagcxSocketAddress remoteAddr;
        if (flagcxSocketGetAddrFromString(&remoteAddr, env) != flagcxSuccess) {//flagcxSocketGetAddrFromString把字符串形式的地址解析成标准的SocketAddress结构体
          WARN("Invalid FLAGCX_COMM_ID, please use format: <ipv4>:<port> or "
               "[<ipv6>]:<port> or <hostname>:<port>");
          pthread_mutex_unlock(&bootstrapNetLock);
          return flagcxInvalidArgument;
        }
        //flagcxFindInterfaceMatchSubnet找到和目标地址在同一个子网的网络接口，把本地本地接口的地址信息存入bootstrapNetIfAddr
        //在多网卡的服务器上，选择和目标地址在同一个子网的接口，可以确保选择了正确的用于高性能计算的高速网卡，而不是慢的管理网卡
        if (flagcxFindInterfaceMatchSubnet(bootstrapNetIfName,
                                           &bootstrapNetIfAddr, &remoteAddr,
                                           MAX_IF_NAME_SIZE, 1) <= 0) {
          WARN("NET/Socket : No usable listening interface found");
          pthread_mutex_unlock(&bootstrapNetLock);
          return flagcxSystemError;
        }
      } else {//没有指定集结点的情况，FLAGCX_COMM_ID为空（这种情况一般只在rank0进程自己身上发生）
        int nIfs = flagcxFindInterfaces(bootstrapNetIfName, &bootstrapNetIfAddr,
                                        MAX_IF_NAME_SIZE, 1);//flagcxFindInterfaces遍历本机所有的网络接口，根据预设规则，选择一个看起来最合适的接口作为默认的引导网络接口
        if (nIfs <= 0) {//如果找不到一个可用的网络接口，说明这台机器网络环境有问题，函数报错
          WARN("Bootstrap : no socket interface found");
          pthread_mutex_unlock(&bootstrapNetLock);
          return flagcxInternalError;
        }
      }
      //把最终选定的网络接口名bootstrapNetIfName和地址bootstrapNetIfAddr格式化成一个可读的字符串
      char line[SOCKET_NAME_MAXLEN + MAX_IF_NAME_SIZE + 2];
      sprintf(line, " %s:", bootstrapNetIfName);
      flagcxSocketToString(&bootstrapNetIfAddr, line + strlen(line));
      INFO(FLAGCX_NET, "Bootstrap : Using%s", line);//打印出来信息，方便调试
      bootstrapNetInitDone = 1;//所有工作完成后，标志位置为1，初始化已成功完成
    }
    pthread_mutex_unlock(&bootstrapNetLock);
  }
  return flagcxSuccess;
}

/* Socket Interface Selection type */
enum bootstrapInterface_t { findSubnetIf = -1, dontCareIf = -2 };

// Additional sync functions
static flagcxResult_t bootstrapNetSend(struct flagcxSocket *sock, void *data,
                                       int size) {
  FLAGCXCHECK(flagcxSocketSend(sock, &size, sizeof(int)));
  FLAGCXCHECK(flagcxSocketSend(sock, data, size));
  return flagcxSuccess;
}
static flagcxResult_t bootstrapNetRecv(struct flagcxSocket *sock, void *data,
                                       int size) {
  int recvSize;
  FLAGCXCHECK(flagcxSocketRecv(sock, &recvSize, sizeof(int)));
  if (recvSize > size) {
    WARN("Message truncated : received %d bytes instead of %d", recvSize, size);
    return flagcxInternalError;
  }
  FLAGCXCHECK(flagcxSocketRecv(sock, data, std::min(recvSize, size)));
  return flagcxSuccess;
}
static flagcxResult_t bootstrapNetSendRecv(struct flagcxSocket *sendSock,
                                           void *sendData, int sendSize,
                                           struct flagcxSocket *recvSock,
                                           void *recvData, int recvSize) {
  int senderRecvSize;
  FLAGCXCHECK(flagcxSocketSendRecv(sendSock, &sendSize, sizeof(int), recvSock,
                                   &senderRecvSize, sizeof(int)));
  if (senderRecvSize > recvSize) {
    WARN("Message truncated : received %d bytes instead of %d", senderRecvSize,
         recvSize);
    return flagcxInternalError;
  }
  FLAGCXCHECK(flagcxSocketSendRecv(sendSock, sendData, sendSize, recvSock,
                                   recvData, recvSize));
  return flagcxSuccess;
}

struct extInfo {
  int rank;
  int nranks;
  union flagcxSocketAddress extAddressListenRoot;
  union flagcxSocketAddress extAddressListen;
};

#include <sys/resource.h>

static flagcxResult_t setFilesLimit() {
  struct rlimit filesLimit;
  SYSCHECK(getrlimit(RLIMIT_NOFILE, &filesLimit), "getrlimit");
  filesLimit.rlim_cur = filesLimit.rlim_max;
  SYSCHECK(setrlimit(RLIMIT_NOFILE, &filesLimit), "setrlimit");
  return flagcxSuccess;
}

//协调员线程coordinator thread，是只在rank0上运行的后台线程
//体现了收集-处理-分发的协调模式
static void *bootstrapRoot(void *rargs) {
  struct bootstrapRootArgs *args = (struct bootstrapRootArgs *)rargs;
  struct flagcxSocket *listenSock = args->listenSock;
  uint64_t magic = args->magic;
  flagcxResult_t res = flagcxSuccess;
  int nranks = 0, c = 0;
  struct extInfo info;
  union flagcxSocketAddress *rankAddresses = NULL;
  union flagcxSocketAddress *rankAddressesRoot =
      NULL; // for initial rank <-> root information exchange
  union flagcxSocketAddress *zero = NULL;
  FLAGCXCHECKGOTO(flagcxCalloc(&zero, 1), res, out);
  setFilesLimit();

  TRACE(FLAGCX_INIT, "BEGIN");
  /* Receive addresses from all ranks */
  //监听循环一直运行，直到接收到所有nranks个进程的连接请求
  do {
    struct flagcxSocket sock;
    FLAGCXCHECKGOTO(flagcxSocketInit(&sock), res, out);
    FLAGCXCHECKGOTO(flagcxSocketAccept(&sock, listenSock), res, out);//阻塞点，线程在这里睡眠，等待新进程连接到监听套接字
    FLAGCXCHECKGOTO(bootstrapNetRecv(&sock, &info, sizeof(info)), res, out);//新连接被接收后，从这个连接接收对方的extInfo结构体
    FLAGCXCHECKGOTO(flagcxSocketClose(&sock), res, out);

    //首次连接处理，第一个进程rank0连接的时候，才知道这次任务的总进程数是多少
    if (c == 0) {
      nranks = info.nranks;
      FLAGCXCHECKGOTO(flagcxCalloc(&rankAddresses, nranks), res, out);//公开信箱地址
      FLAGCXCHECKGOTO(flagcxCalloc(&rankAddressesRoot, nranks), res, out);//私有信箱地址，接收rank0指令
    }

    //错误检查，检查后续连接的nranks是否与第一个一致
    if (nranks != info.nranks) {
      WARN("Bootstrap Root : mismatch in rank count from procs %d : %d", nranks,
           info.nranks);
      goto out;
    }

    //检查某个rank是否重复报道
    if (memcmp(zero, &rankAddressesRoot[info.rank],
               sizeof(union flagcxSocketAddress)) != 0) {
      WARN("Bootstrap Root : rank %d of %d ranks has already checked in",
           info.rank, nranks);
      goto out;
    }

    INFO(FLAGCX_INIT, "Bootstrap Root : rank %d of %d ranks checked in",
         info.rank, nranks);

    // Save the connection handle for that rank
    memcpy(rankAddressesRoot + info.rank, &info.extAddressListenRoot,
           sizeof(union flagcxSocketAddress));
    memcpy(rankAddresses + info.rank, &info.extAddressListen,
           sizeof(union flagcxSocketAddress));

    ++c;//c是接收计数器，+1，继续accept下一个连接，直到c=nranks
    TRACE(FLAGCX_INIT, "Received connect from rank %d total %d/%d", info.rank,
          c, nranks);
  } while (c < nranks);
  TRACE(FLAGCX_INIT, "COLLECTED ALL %d HANDLES", nranks);

  // Send the connect handle for the next rank in the AllGather ring
  //分发环邻居指令，收集完所有信息后，bootstrapRoot线程主动向每一个线程下发建立环连接的指令
  for (int r = 0; r < nranks; ++r) {//遍历所有rank
    int next = (r + 1) % nranks;
    struct flagcxSocket sock;
    FLAGCXCHECKGOTO(flagcxSocketInit(&sock, rankAddressesRoot + r, magic,
                                     flagcxSocketTypeBootstrap),
                    res, out);//建立临时套接字，连到r的私有信箱
    FLAGCXCHECKGOTO(flagcxSocketConnect(&sock), res, out);
    FLAGCXCHECKGOTO(bootstrapNetSend(&sock, rankAddresses + next,
                                     sizeof(union flagcxSocketAddress)),
                    res, out);//把rank next的公开信箱发送给rank r
    FLAGCXCHECKGOTO(flagcxSocketClose(&sock), res, out);
  }
  INFO(FLAGCX_INIT, "SENT OUT ALL %d HANDLES", nranks);

  //完成所有协调任务后，或中间碰到错误时，跳转到out标签处
out:
  if (listenSock != NULL) {
    flagcxSocketClose(listenSock);
    free(listenSock);
  }
  if (rankAddresses)
    free(rankAddresses);
  if (rankAddressesRoot)
    free(rankAddressesRoot);
  if (zero)
    free(zero);
  free(rargs);

  TRACE(FLAGCX_INIT, "DONE");
  return NULL;
}

flagcxResult_t bootstrapCreateRoot(struct flagcxBootstrapHandle *handle,
                                   bool idFromEnv) {
  struct flagcxSocket *listenSock;
  struct bootstrapRootArgs *args;
  pthread_t thread;

  FLAGCXCHECK(flagcxCalloc(&listenSock, 1));
  FLAGCXCHECK(flagcxSocketInit(listenSock, &handle->addr, handle->magic,
                               flagcxSocketTypeBootstrap, NULL, 0));
  FLAGCXCHECK(flagcxSocketListen(listenSock));
  FLAGCXCHECK(flagcxSocketGetAddr(listenSock, &handle->addr));

  FLAGCXCHECK(flagcxCalloc(&args, 1));
  args->listenSock = listenSock;
  args->magic = handle->magic;
  NEQCHECK(pthread_create(&thread, NULL, bootstrapRoot, (void *)args), 0);
  flagcxSetThreadName(thread, "FLAGCX bootstrapRoot");
  NEQCHECK(pthread_detach(thread), 0); // will not be pthread_join()'d
  return flagcxSuccess;
}

flagcxResult_t bootstrapGetUniqueId(struct flagcxBootstrapHandle *handle) {
  memset(handle, 0, sizeof(flagcxBootstrapHandle));
  FLAGCXCHECK(getRandomData(&handle->magic, sizeof(handle->magic)));

  const char *env = flagcxGetEnv("FLAGCX_COMM_ID");
  if (env) {
    INFO(FLAGCX_ENV, "FLAGCX_COMM_ID set by environment to %s", env);
    if (flagcxSocketGetAddrFromString(&handle->addr, env) != flagcxSuccess) {
      WARN("Invalid FLAGCX_COMM_ID, please use format: <ipv4>:<port> or "
           "[<ipv6>]:<port> or <hostname>:<port>");
      return flagcxInvalidArgument;
    }
  } else {
    memcpy(&handle->addr, &bootstrapNetIfAddr,
           sizeof(union flagcxSocketAddress));
    FLAGCXCHECK(bootstrapCreateRoot(handle, false));
  }

  return flagcxSuccess;
}

struct unexConn {
  int peer;
  int tag;
  struct flagcxSocket sock;
  struct unexConn *next;
};

//服务器-客户端握手+环建立
flagcxResult_t bootstrapInit(struct flagcxBootstrapHandle *handle,
                             void *commState) {
  struct bootstrapState *state = (struct bootstrapState *)commState;
  int rank = state->rank;
  int nranks = state->nranks;
  flagcxSocketAddress nextAddr;
  struct flagcxSocket sock, listenSockRoot;
  struct extInfo info = {0};

  TRACE(FLAGCX_INIT, "rank %d nranks %d", rank, nranks);

  info.rank = rank;//把rank、nranks以及下面两个套接字的地址都放入，打包进一个extInfo结构体
  info.nranks = nranks;
  // Create socket for other ranks to contact me
  //每个进程启动后，创建两个监听套接字，第一个套接字公开广播给其它进程，用于建立环
  FLAGCXCHECK(flagcxSocketInit(&state->listenSock, &bootstrapNetIfAddr,
                               state->magic, flagcxSocketTypeBootstrap,
                               state->abortFlag));
  FLAGCXCHECK(flagcxSocketListen(&state->listenSock));
  FLAGCXCHECK(flagcxSocketGetAddr(&state->listenSock, &info.extAddressListen));

  // Create socket for root to contact me
  //第二个套接字专门接收rank0根进程的信息
  FLAGCXCHECK(flagcxSocketInit(&listenSockRoot, &bootstrapNetIfAddr,
                               state->magic, flagcxSocketTypeBootstrap,
                               state->abortFlag));
  FLAGCXCHECK(flagcxSocketListen(&listenSockRoot));
  FLAGCXCHECK(flagcxSocketGetAddr(&listenSockRoot, &info.extAddressListenRoot));

  // stagger connection times to avoid an overload of the root
  //错峰连接根进程rank0
  if (nranks > 128) {
    long msec = rank;
    struct timespec tv;
    tv.tv_sec = msec / 1000;
    tv.tv_nsec = 1000000 * (msec % 1000);//每个进程根据自己的进程号等待一段时间，号越大等待时间越长，这样请求分散开了
    TRACE(FLAGCX_INIT, "rank %d delaying connection to root by %ld msec", rank,
          msec);
    (void)nanosleep(&tv, NULL);
  }

  // send info on my listening socket to root
  //每个进程创建一个临时套接字，连接handle->addr指定的根进程地址，把自己的info结构体发过去，发送后关闭这个连接
  FLAGCXCHECK(flagcxSocketInit(&sock, &handle->addr, state->magic,
                               flagcxSocketTypeBootstrap, state->abortFlag));
  FLAGCXCHECK(flagcxSocketConnect(&sock));
  FLAGCXCHECK(bootstrapNetSend(&sock, &info, sizeof(info)));
  FLAGCXCHECK(flagcxSocketClose(&sock));

  // get info on my "next" rank in the bootstrap ring from root
  //在第二个私有信箱连接根进程的套接字上等待，根进程收集完信息之后，会逐一连接每个进程，告诉它下一步怎么做
  FLAGCXCHECK(flagcxSocketInit(&sock));
  FLAGCXCHECK(flagcxSocketAccept(&sock, &listenSockRoot));
  FLAGCXCHECK(
      bootstrapNetRecv(&sock, &nextAddr, sizeof(union flagcxSocketAddress)));//从根进程那边接收下一个邻居的地址，创建套接字主动连接它
  FLAGCXCHECK(flagcxSocketClose(&sock));
  FLAGCXCHECK(flagcxSocketClose(&listenSockRoot));

  FLAGCXCHECK(flagcxSocketInit(&state->ringSendSocket, &nextAddr, state->magic,
                               flagcxSocketTypeBootstrap, state->abortFlag));
  FLAGCXCHECK(flagcxSocketConnect(&state->ringSendSocket));
  // Accept the connect request from the previous rank in the AllGather ring
  //在第一个公开信箱中等待前一个邻居来联系自己
  FLAGCXCHECK(flagcxSocketInit(&state->ringRecvSocket));
  FLAGCXCHECK(flagcxSocketAccept(&state->ringRecvSocket, &state->listenSock));
  //上面的过程是建立逻辑环

  // AllGather all listen handlers
  //最终的信息同步，确保所有进程都有完整的通信地址簿
  FLAGCXCHECK(flagcxCalloc(&state->peerCommAddresses, nranks));
  FLAGCXCHECK(
      flagcxSocketGetAddr(&state->listenSock, state->peerCommAddresses + rank));
  FLAGCXCHECK(bootstrapAllGather(state, state->peerCommAddresses,
                                 sizeof(union flagcxSocketAddress)));//每个进程把自己的第一个公开信箱地址放入peerCommAddresses数组的对应位置，然后调用bootstrapAllGather广播给所有进程

  INFO(FLAGCX_INIT, "rank %d nranks %d - DONE", rank, nranks);

  return flagcxSuccess;
}

// Bootstrap send/receive functions
//
// We do not keep connections opened with all ranks at all times, and we have no
// guarantee that connections to our unique listen socket will arrive in the
// same order as we need them. Therefore, when establishing a connection, the
// sender sends a (peer, tag) tuple to allow the receiver to identify the flow,
// and keep it in an unexpected queue if needed.

flagcxResult_t bootstrapConnect(void *commState, int peer, int tag,
                                struct flagcxSocket *sock) {
  flagcxResult_t ret = flagcxSuccess;
  struct bootstrapState *state = (struct bootstrapState *)commState;

  FLAGCXCHECKGOTO(flagcxSocketInit(sock, state->peerCommAddresses + peer,
                                   state->magic, flagcxSocketTypeBootstrap),
                  ret, fail);
  FLAGCXCHECKGOTO(flagcxSocketConnect(sock), ret, fail);
  FLAGCXCHECKGOTO(bootstrapNetSend(sock, &state->rank, sizeof(int)), ret, fail);
  FLAGCXCHECKGOTO(bootstrapNetSend(sock, &tag, sizeof(int)), ret, fail);
  return flagcxSuccess;
fail:
  FLAGCXCHECK(flagcxSocketClose(sock));
  return ret;
}

flagcxResult_t bootstrapSend(void *commState, int peer, int tag, void *data,
                             int size) {
  flagcxResult_t ret = flagcxSuccess;
  struct flagcxSocket sock;

  TRACE(FLAGCX_BOOTSTRAP, "Sending to peer=%d tag=%d size=%d", peer, tag, size);
  FLAGCXCHECK(bootstrapConnect(commState, peer, tag, &sock));
  FLAGCXCHECKGOTO(bootstrapNetSend(&sock, data, size), ret, exit);

  TRACE(FLAGCX_BOOTSTRAP, "Sent to peer=%d tag=%d size=%d", peer, tag, size);

exit:
  FLAGCXCHECK(flagcxSocketClose(&sock));
  return ret;
}

flagcxResult_t unexpectedEnqueue(struct bootstrapState *state, int peer,
                                 int tag, struct flagcxSocket *sock) {
  // New unex
  struct unexConn *unex;
  FLAGCXCHECK(flagcxCalloc(&unex, 1));
  unex->peer = peer;
  unex->tag = tag;
  memcpy(&unex->sock, sock, sizeof(struct flagcxSocket));

  // Enqueue
  struct unexConn *list = state->unexpectedConnections;
  if (list == NULL) {
    state->unexpectedConnections = unex;
    return flagcxSuccess;
  }
  while (list->next)
    list = list->next;
  list->next = unex;
  return flagcxSuccess;
}

flagcxResult_t unexpectedDequeue(struct bootstrapState *state, int peer,
                                 int tag, struct flagcxSocket *sock,
                                 int *found) {
  struct unexConn *elem = state->unexpectedConnections;
  struct unexConn *prev = NULL;
  *found = 0;
  while (elem) {
    if (elem->peer == peer && elem->tag == tag) {
      if (prev == NULL) {
        state->unexpectedConnections = elem->next;
      } else {
        prev->next = elem->next;
      }
      memcpy(sock, &elem->sock, sizeof(struct flagcxSocket));
      free(elem);
      *found = 1;
      return flagcxSuccess;
    }
    prev = elem;
    elem = elem->next;
  }
  return flagcxSuccess;
}

static void unexpectedFree(struct bootstrapState *state) {
  struct unexConn *elem = state->unexpectedConnections;
  struct unexConn *prev = NULL;

  while (elem) {
    prev = elem;
    elem = elem->next;
    free(prev);
  }
  return;
}

// We can't know who we'll receive from, so we need to receive everything at
// once
flagcxResult_t bootstrapAccept(void *commState, int peer, int tag,
                               struct flagcxSocket *sock) {
  flagcxResult_t ret = flagcxSuccess;
  struct bootstrapState *state = (struct bootstrapState *)commState;
  int newPeer, newTag;

  // Search unexpected connections first
  int found;
  FLAGCXCHECK(unexpectedDequeue(state, peer, tag, sock, &found));
  if (found)
    return flagcxSuccess;

  // Then look for new connections
  while (1) {
    FLAGCXCHECKGOTO(flagcxSocketInit(sock), ret, fail);
    FLAGCXCHECKGOTO(flagcxSocketAccept(sock, &state->listenSock), ret, fail);
    FLAGCXCHECKGOTO(bootstrapNetRecv(sock, &newPeer, sizeof(int)), ret, fail);
    FLAGCXCHECKGOTO(bootstrapNetRecv(sock, &newTag, sizeof(int)), ret, fail);
    if (newPeer == peer && newTag == tag)
      return flagcxSuccess;
    FLAGCXCHECKGOTO(unexpectedEnqueue(state, newPeer, newTag, sock), ret, fail);
  }
  return flagcxSuccess;
fail:
  FLAGCXCHECK(flagcxSocketClose(sock));
  return ret;
}

// We can't know who we'll receive from, so we need to receive everything at
// once
flagcxResult_t bootstrapRecv(void *commState, int peer, int tag, void *data,
                             int size) {
  flagcxResult_t ret;
  struct flagcxSocket sock;
  FLAGCXCHECK(bootstrapAccept(commState, peer, tag, &sock));
  TRACE(FLAGCX_BOOTSTRAP, "Receiving tag=%d peer=%d size=%d", tag, peer, size);
  FLAGCXCHECKGOTO(bootstrapNetRecv(&sock, ((char *)data), size), ret, exit);
exit:
  FLAGCXCHECK(flagcxSocketClose(&sock));
  return ret;
}

// Collective algorithms, based on bootstrapSend/Recv, and sometimes
// bootstrapConnect/Accept

flagcxResult_t bootstrapRingAllGather(struct flagcxSocket *prevSocket,
                                      struct flagcxSocket *nextSocket, int rank,
                                      int nranks, char *data, int size) {
  /* Simple ring based AllGather
   * At each step i receive data from (rank-i-1) from prev
   * and send previous step's data from (rank-i) to next
   */
  for (int i = 0; i < nranks - 1; i++) {
    size_t rslice = (rank - i - 1 + nranks) % nranks;
    size_t sslice = (rank - i + nranks) % nranks;

    // Send slice to the right, recv slice from the left
    FLAGCXCHECK(bootstrapNetSendRecv(nextSocket, data + sslice * size, size,
                                     prevSocket, data + rslice * size, size));
  }
  return flagcxSuccess;
}

// Another Version of RingAllGather
// The data bytes gather from multiple ranks are uneven.
flagcxResult_t bootstrapRingAllGatherV2(struct flagcxSocket *prevSocket,
                                        struct flagcxSocket *nextSocket,
                                        int rank, int nranks, char *data,
                                        size_t *offset, size_t *length) {
  /* Simple ring based AllGather
   * At each step i receive data from (rank-i-1) from prev
   * and send previous step's data from (rank-i) to next
   */
  uint64_t timers[TIMERS_COLL_COUNT] = {0};
  timers[TIMER_COLL_TOTAL] = clockNano();

  for (int i = 0; i < nranks - 1; i++) {
    size_t rslice = (rank - i - 1 + nranks) % nranks;
    size_t sslice = (rank - i + nranks) % nranks;

    // Send slice to the right, recv slice from the left
    FLAGCXCHECK(bootstrapNetSendRecv(
        nextSocket, (void *)(data + offset[sslice]), length[sslice], prevSocket,
        (void *)(data + offset[rslice]), length[rslice]));
  }
  timers[TIMER_COLL_TOTAL] = clockNano() - timers[TIMER_COLL_TOTAL];
  INFO(FLAGCX_COLL, "COLL timings - %s: rank %d nranks %d total %.2fms.",
       "BootstrapRingAllGatherV2", rank, nranks,
       timers[TIMER_COLL_TOTAL] / 1e6);
  return flagcxSuccess;
}
flagcxResult_t bootstrapAllGather(void *commState, void *allData, int size) {
  struct bootstrapState *state = (struct bootstrapState *)commState;
  int rank = state->rank;
  int nranks = state->nranks;

  TRACE(FLAGCX_INIT, "rank %d nranks %d size %d", rank, nranks, size);

  FLAGCXCHECK(bootstrapRingAllGather(&state->ringRecvSocket,
                                     &state->ringSendSocket, rank, nranks,
                                     (char *)allData, size));

  TRACE(FLAGCX_INIT, "rank %d nranks %d size %d - DONE", rank, nranks, size);
  return flagcxSuccess;
}

flagcxResult_t AllGatherBootstrap(void *commState, const void *sendbuff,
                                  void *recvbuff, size_t sendcount,
                                  flagcxDataType_t datatype) {
  struct bootstrapState *state = (struct bootstrapState *)commState;
  int rank = state->rank;
  // if not in-place
  if (sendbuff !=
      (void *)((char *)recvbuff +
               rank * getFlagcxDataTypeSize(datatype) * sendcount)) {
    memcpy((void *)((char *)recvbuff +
                    rank * getFlagcxDataTypeSize(datatype) * sendcount),
           sendbuff, getFlagcxDataTypeSize(datatype) * sendcount);
  }
  return bootstrapAllGather(commState, recvbuff,
                            getFlagcxDataTypeSize(datatype) * sendcount);
}
/*
 * Reduce-Scatter
 *
 * Reduces data in sendbuff using op operation and leaves reduced result
 * scattered over the devices so that recvbuff on rank i will contain the i-th
 * block of the result.
 *
 * Block size among all ranks are not necessary equal.
 * The i-th block begins at offset[i] and has the length of length[i].
 * The recvbuff of rank i should has the length of at least length[i].
 *
 * In-place operations will happen if recvbuff == sendbuff + offset[rank].
 */
flagcxResult_t bootstrapRingReduceScatter(
    struct flagcxSocket *prevSocket, struct flagcxSocket *nextSocket, int rank,
    int nranks, const char *sendbuff, char *recvbuff, size_t *offset,
    size_t *length, flagcxDataType_t datatype, flagcxRedOp_t op) {
  uint64_t timers[TIMERS_COLL_COUNT] = {0};
  timers[TIMER_COLL_TOTAL] = clockNano();

  // Allocate two temporary buffer with size length[0] to ensure that it can
  // fill any chunk size.
  timers[TIMER_COLL_ALLOC] = clockNano();
  // found the largest chunk
  size_t subSize = 0;
  for (int i = 0; i < nranks; ++i) {
    subSize = std::max(length[i], subSize);
  }
  char *data_for_send = nullptr;
  FLAGCXCHECK(flagcxCalloc(&data_for_send, subSize));
  char *data_for_recv = nullptr;
  FLAGCXCHECK(flagcxCalloc(&data_for_recv, subSize));
  timers[TIMER_COLL_ALLOC] = clockNano() - timers[TIMER_COLL_ALLOC];

  uint64_t start = 0;
  uint64_t end = 0;
  // for iteration 0 -> n-1
  for (int iter = 0; iter < nranks - 1; ++iter) {
    // for each iteration ${iter}
    // 1. rank ${rank} should send data of chunk $(send_chunk_no) to next rank
    // 2. rank ${rank} should recv data of chunk $(recv_chunk_no) from prev rank
    int send_chunk_no = (rank + 2 * nranks - iter - 1) % nranks;
    int recv_chunk_no = (rank + 2 * nranks - iter - 2) % nranks;
    bool needSend = length[send_chunk_no] != 0;
    bool needRecv = length[recv_chunk_no] != 0;

    INFO(FLAGCX_COLL,
         "rank %d nranks %d; iter=%d; send_chunk_no=%d; send_chunk_size=%lu; "
         "needSend=%d; "
         "recv_chunk_no=%d; recv_chunk_size=%lu; needRecv=%d",
         rank, nranks, iter, send_chunk_no, length[send_chunk_no], needSend,
         recv_chunk_no, length[recv_chunk_no], needRecv);
    if (!needSend && !needRecv) {
      continue;
    }

    // step 1: prepare send data if needed
    if (needSend) {
      start = clockNano();
      if (iter == 0) {
        // initial iteration
        memcpy(data_for_send, sendbuff + offset[send_chunk_no],
               length[send_chunk_no]);
      } else {
        std::swap(data_for_send, data_for_recv);
      }
      end = clockNano();
      timers[TIMER_COLL_MEM] += end - start;
    }

    // step 2: exchange data using Send/Recv
    start = clockNano();
    if (needSend && needRecv) {
      FLAGCXCHECK(bootstrapNetSendRecv(
          nextSocket, (void *)data_for_send, length[send_chunk_no], prevSocket,
          (void *)data_for_recv, length[recv_chunk_no]));
    } else if (needSend) {
      FLAGCXCHECK(bootstrapNetSend(nextSocket, (void *)data_for_send,
                                   length[send_chunk_no]));
    } else if (needRecv) {
      FLAGCXCHECK(bootstrapNetRecv(prevSocket, (void *)data_for_recv,
                                   length[recv_chunk_no]));
    }
    end = clockNano();
    timers[TIMER_COLL_COMM] += end - start;

    // step3 : local reduction for data_for_send & chunk ${recv_chunk_no} if
    // recv something
    //         save result in data_for_recv
    if (!needRecv) {
      continue;
    }
    start = clockNano();
    switch (op) {
      case flagcxSum:
        GENERATE_ALL_TYPES(datatype, sum, data_for_recv,
                           sendbuff + offset[recv_chunk_no], data_for_recv,
                           length[recv_chunk_no] /
                               getFlagcxDataTypeSize(datatype));
        break;
      case flagcxMax:
        GENERATE_ALL_TYPES(datatype, max, data_for_recv,
                           sendbuff + offset[recv_chunk_no], data_for_recv,
                           length[recv_chunk_no] /
                               getFlagcxDataTypeSize(datatype));
        break;
      case flagcxMin:
        GENERATE_ALL_TYPES(datatype, min, data_for_recv,
                           sendbuff + offset[recv_chunk_no], data_for_recv,
                           length[recv_chunk_no] /
                               getFlagcxDataTypeSize(datatype));
        break;
      default:
        WARN("Unsupported reduction operation %d", op);
        return flagcxInvalidArgument;
    }
    end = clockNano();
    timers[TIMER_COLL_CALC] += end - start;
  }

  // copy the final reduction to recvbuff
  memcpy(recvbuff, data_for_recv, length[rank]);
  free(data_for_send);
  free(data_for_recv);

  timers[TIMER_COLL_TOTAL] = clockNano() - timers[TIMER_COLL_TOTAL];
  INFO(FLAGCX_COLL,
       "COLL timings - %s: rank %d nranks %d total %.2fms (calc %.2fms, "
       "mem_alloc %.2fms, memory %.2fms, comm %.2fms)",
       "BootstrapRingReduceScatter", rank, nranks,
       timers[TIMER_COLL_TOTAL] / 1e6, timers[TIMER_COLL_CALC] / 1e6,
       timers[TIMER_COLL_ALLOC] / 1e6, timers[TIMER_COLL_MEM] / 1e6,
       timers[TIMER_COLL_COMM] / 1e6);
  return flagcxSuccess;
}

const size_t MIN_CHUNK_SIZE = 1024 * 1024 * 4; // 4MB

size_t roundUp(size_t value, size_t multiple) {
  size_t remainder = value % multiple;
  if (remainder == 0) {
    return value;
  }
  return value + multiple - remainder;
}

flagcxResult_t bootstrapRingAllReduce(struct flagcxSocket *prevSocket,
                                      struct flagcxSocket *nextSocket, int rank,
                                      int nranks, const char *sendbuff,
                                      char *recvbuff, size_t count,
                                      flagcxDataType_t datatype,
                                      flagcxRedOp_t op) {

  //prevSocket和nextSocket是前后两个套接字，count输入元素的数量，datatype类型，op规约操作
  // The ring algorithm works as follows.
  //
  // The given input is split into a number of chunks equal to the
  // number of processes. Once the reducescatter has finished, every
  // process hosts one chunk of reduced output, in sequential order
  // (rank 0 has chunk 0, rank 1 has chunk 1, etc.). As the input may
  // not be divisible by the number of processes, the chunk on the
  // final ranks may have partial output or may be empty.
  //

  //自适应块大小
  //把AllReduce的整个数组大小，逻辑上划分成nranks个大小差不多的数据块chunk
  size_t size = count * getFlagcxDataTypeSize(datatype);
  size_t ChunkBytes = std::max((size + nranks - 1) / nranks, MIN_CHUNK_SIZE);//计算每个数据块的大概字节数，不能小于预设的最小值
  //预设最小值是为了防止块内容太小，协议IP头的开销占比大，效率低
  // Ensure that min chunk size is a multiple of the element size.
  ChunkBytes = roundUp(ChunkBytes, getFlagcxDataTypeSize(datatype));//roundUp函数向上调整，确保块大小是数据类型大小的整数倍
  INFO(FLAGCX_COLL, "rank %d nranks %d; size=%lu; typesize=%lu; ChunkBytes=%lu",
       rank, nranks, size, getFlagcxDataTypeSize(datatype), ChunkBytes);
       //分块是为了在环的不同链路上同时进行计算和传输，最大化的利用网络带宽

  // step 1: split the data and prepare offset and length array
  std::vector<size_t> offset(nranks, 0);//存储第i个数据块的起始位置偏移量
  std::vector<size_t> length(nranks, 0);//存储第i个数据块的长度
  for (size_t i = 0; i < nranks; ++i) {
    if (ChunkBytes * i >= size) {//处理空块
      offset[i] = size;
      length[i] = 0;
      continue;
    }
    offset[i] = ChunkBytes * i;
    length[i] =
        ChunkBytes * (i + 1) >= size ? size - ChunkBytes * i : ChunkBytes;//由于整数除法+roundUp，所有块的长度加起来可能稍微>总数据大小size
  }//前面的块<size，长度是标准的ChunkBytes；最后一个非空块可能大于size，长度裁剪成size - ChunkBytes * i，从当前的起始位置到数组末尾的所有剩余数据。

  // step 2: reduce scatter
  FLAGCXCHECK(bootstrapRingReduceScatter(
      prevSocket, nextSocket, rank, nranks, sendbuff, recvbuff + offset[rank],
      offset.data(), length.data(), datatype, op));

  // step 3: all gather
  FLAGCXCHECK(bootstrapRingAllGatherV2(prevSocket, nextSocket, rank, nranks,
                                       recvbuff, offset.data(), length.data()));
  return flagcxSuccess;
}

flagcxResult_t
bootstrapRingReduce(void *commState, struct flagcxSocket *prevSocket,
                    struct flagcxSocket *nextSocket, int rank, int nranks,
                    const char *sendbuff, char *recvbuff, size_t count,
                    flagcxDataType_t datatype, flagcxRedOp_t op, int root) {

  // The ring algorithm works as follows.
  //
  // The given input is split into a number of chunks equal to the
  // number of processes. Once the reducescatter has finished, every
  // process hosts one chunk of reduced output, in sequential order
  // (rank 0 has chunk 0, rank 1 has chunk 1, etc.). As the input may
  // not be divisible by the number of processes, the chunk on the
  // final ranks may have partial output or may be empty.
  //

  size_t size = count * getFlagcxDataTypeSize(datatype);
  size_t ChunkBytes = std::max((size + nranks - 1) / nranks, MIN_CHUNK_SIZE);
  // Ensure that min chunk size is a multiple of the element size.
  ChunkBytes = roundUp(ChunkBytes, getFlagcxDataTypeSize(datatype));
  INFO(FLAGCX_COLL, "rank %d nranks %d; size=%lu; typesize=%lu; ChunkBytes=%lu",
       rank, nranks, size, getFlagcxDataTypeSize(datatype), ChunkBytes);

  // step 1: split the data and prepare offset and length array
  std::vector<size_t> offset(nranks, 0);
  std::vector<size_t> length(nranks, 0);
  for (size_t i = 0; i < nranks; ++i) {
    if (ChunkBytes * i >= size) {
      offset[i] = size;
      length[i] = 0;
      continue;
    }
    offset[i] = ChunkBytes * i;
    length[i] =
        ChunkBytes * (i + 1) >= size ? size - ChunkBytes * i : ChunkBytes;
  }

  // step 2: reduce scatter
  FLAGCXCHECK(bootstrapRingReduceScatter(
      prevSocket, nextSocket, rank, nranks, sendbuff, recvbuff + offset[rank],
      offset.data(), length.data(), datatype, op));

  // step 3: gather
  const int bootstrapTag = -9993;
  if (rank == root) {
    for (int i = 0; i < nranks; i++) {
      if (i == rank)
        continue;
      FLAGCXCHECK(bootstrapRecv(commState, i, bootstrapTag,
                                recvbuff + offset[i], length[i]));
    }
  } else {
    FLAGCXCHECK(bootstrapSend(commState, root, bootstrapTag,
                              recvbuff + offset[rank], length[rank]));
  }

  return flagcxSuccess;
}

flagcxResult_t AllReduceBootstrap(void *commState, const void *sendbuff,
                                  void *recvbuff, size_t count,
                                  flagcxDataType_t datatype, flagcxRedOp_t op) {
  struct bootstrapState *state = (struct bootstrapState *)commState;
  int rank = state->rank;
  int nranks = state->nranks;
  if (nranks == 1) {
    if (sendbuff != recvbuff) {
      memcpy(recvbuff, sendbuff, count * getFlagcxDataTypeSize(datatype));
    }
    return flagcxSuccess;
  }
  FLAGCXCHECK(bootstrapRingAllReduce(
      &state->ringRecvSocket, &state->ringSendSocket, rank, nranks,
      (char *)sendbuff, (char *)recvbuff, count, datatype, op));

  return flagcxSuccess;
}

flagcxResult_t ReduceBootstrap(void *commState, const void *sendbuff,
                               void *recvbuff, size_t count,
                               flagcxDataType_t datatype, flagcxRedOp_t op,
                               int root) {
  struct bootstrapState *state = (struct bootstrapState *)commState;
  int rank = state->rank;
  int nranks = state->nranks;

  if (nranks == 1) {
    if (sendbuff != recvbuff) {
      memcpy(recvbuff, sendbuff, count * getFlagcxDataTypeSize(datatype));
    }
    return flagcxSuccess;
  }
  FLAGCXCHECK(bootstrapRingReduce(
      commState, &state->ringRecvSocket, &state->ringSendSocket, rank, nranks,
      (char *)sendbuff, (char *)recvbuff, count, datatype, op, root));
  return flagcxSuccess;
}

flagcxResult_t ReduceScatterBootstrap(void *commState, const void *sendbuff,
                                      void *recvbuff, size_t recvcount,
                                      flagcxDataType_t datatype,
                                      flagcxRedOp_t op) {
  struct bootstrapState *state = (struct bootstrapState *)commState;
  int rank = state->rank;
  int nranks = state->nranks;
  if (nranks == 1) {
    if (sendbuff != recvbuff) {
      memcpy(recvbuff, sendbuff, recvcount * getFlagcxDataTypeSize(datatype));
    }
    return flagcxSuccess;
  }
  // prepare offset, length vector
  std::vector<size_t> offset(nranks, 0);
  std::vector<size_t> length(nranks, 0);
  for (size_t i = 0; i < nranks; ++i) {
    offset[i] = i * recvcount * getFlagcxDataTypeSize(datatype);
    length[i] = recvcount * getFlagcxDataTypeSize(datatype);
  }
  FLAGCXCHECK(bootstrapRingReduceScatter(
      &state->ringRecvSocket, &state->ringSendSocket, rank, nranks,
      (char *)sendbuff, (char *)recvbuff, offset.data(), length.data(),
      datatype, op));
  return flagcxSuccess;
}

flagcxResult_t AlltoAllBootstrap(void *commState, const void *sendbuff,
                                 void *recvbuff, size_t count,
                                 flagcxDataType_t datatype) {
  struct bootstrapState *state = (struct bootstrapState *)commState;
  int rank = state->rank;
  int nranks = state->nranks;
  size_t size = count * getFlagcxDataTypeSize(datatype);

  bool inPlace = (sendbuff == recvbuff);
  char *tmpBuff = nullptr;
  if (inPlace) {
    FLAGCXCHECK(flagcxCalloc(&tmpBuff, size));
  }

  for (int i = 0; i < nranks; ++i) {
    if (i == rank) {
      if (!inPlace) {
        memcpy((void *)((char *)recvbuff + size * i),
               (void *)((char *)sendbuff + size * i), size);
      }
    }
    const int bootstrapTag = -9991;
    if (rank > i) {
      FLAGCXCHECK(bootstrapSend(commState, i, bootstrapTag,
                                (void *)((char *)sendbuff + size * i), size));
      if (inPlace) {
        FLAGCXCHECK(
            bootstrapRecv(commState, i, bootstrapTag, (void *)tmpBuff, size));
        memcpy((void *)((char *)recvbuff + size * i), (void *)tmpBuff, size);
      } else {
        FLAGCXCHECK(bootstrapRecv(commState, i, bootstrapTag,
                                  (void *)((char *)recvbuff + size * i), size));
      }
    } else if (rank < i) {
      if (inPlace) {
        FLAGCXCHECK(
            bootstrapRecv(commState, i, bootstrapTag, (void *)tmpBuff, size));
      } else {
        FLAGCXCHECK(bootstrapRecv(commState, i, bootstrapTag,
                                  (void *)((char *)recvbuff + size * i), size));
      }
      FLAGCXCHECK(bootstrapSend(commState, i, bootstrapTag,
                                (void *)((char *)sendbuff + size * i), size));
      if (inPlace) {
        memcpy((void *)((char *)recvbuff + size * i), (void *)tmpBuff, size);
      }
    }
  }
  free(tmpBuff);
  return flagcxSuccess;
}

flagcxResult_t BroadcastBootstrap(void *commState, const void *sendbuff,
                                  void *recvbuff, size_t sendcount,
                                  flagcxDataType_t datatype, int root) {
  struct bootstrapState *state = (struct bootstrapState *)commState;
  int rank = state->rank;
  int nranks = state->nranks;
  const int bootstrapTag = -9992;
  if (nranks == 1) {
    if (sendbuff != recvbuff) {
      memcpy(recvbuff, sendbuff, getFlagcxDataTypeSize(datatype) * sendcount);
    }
    return flagcxSuccess;
  }
  if (rank == root) {
    if (sendbuff != recvbuff) {
      memcpy(recvbuff, sendbuff, getFlagcxDataTypeSize(datatype) * sendcount);
    }
    // root sends data to all other ranks
    for (int i = 0; i < nranks; ++i) {
      if (i != root) {
        FLAGCXCHECK(bootstrapSend(commState, i, bootstrapTag,
                                  (void *)(sendbuff),
                                  sendcount * getFlagcxDataTypeSize(datatype)));
      }
    }
  } else {
    // all other ranks receive data from root
    FLAGCXCHECK(bootstrapRecv(commState, root, bootstrapTag, recvbuff,
                              sendcount * getFlagcxDataTypeSize(datatype)));
  }
  return flagcxSuccess;
}

flagcxResult_t ScatterBootstrap(void *commState, const void *sendbuff,
                                void *recvbuff, size_t count,
                                flagcxDataType_t datatype, int root) {
  struct bootstrapState *state = (struct bootstrapState *)commState;
  int rank = state->rank;
  int nranks = state->nranks;
  const int bootstrapTag = -9993;
  if (nranks == 1) {
    if (sendbuff != recvbuff) {
      memcpy(recvbuff, sendbuff, getFlagcxDataTypeSize(datatype) * count);
    }
    return flagcxSuccess;
  }

  if (rank == root) {
    // For root process, only copy its own portion of data
    size_t rootOffset = root * count * getFlagcxDataTypeSize(datatype);
    if ((char *)sendbuff + rootOffset != recvbuff) {
      memcpy(recvbuff, (const char *)sendbuff + rootOffset,
             getFlagcxDataTypeSize(datatype) * count);
    }
    // root sends data to all other ranks
    for (int i = 0; i < nranks; ++i) {
      if (i != root) {
        size_t offset = i * count * getFlagcxDataTypeSize(datatype);
        FLAGCXCHECK(bootstrapSend(commState, i, bootstrapTag,
                                  (char *)sendbuff + offset,
                                  count * getFlagcxDataTypeSize(datatype)));
      }
    }
  } else {
    // all other ranks receive data from root
    FLAGCXCHECK(bootstrapRecv(commState, root, bootstrapTag, recvbuff,
                              count * getFlagcxDataTypeSize(datatype)));
  }
  return flagcxSuccess;
}

flagcxResult_t GatherBootstrap(void *commState, const void *sendbuff,
                               void *recvbuff, size_t count,
                               flagcxDataType_t datatype, int root) {
  struct bootstrapState *state = (struct bootstrapState *)commState;
  int rank = state->rank;
  int nranks = state->nranks;
  const int bootstrapTag = -9994;

  if (nranks == 1) {
    if (sendbuff != recvbuff) {
      memcpy(recvbuff, sendbuff, getFlagcxDataTypeSize(datatype) * count);
    }
    return flagcxSuccess;
  }

  if (rank == root) {
    // Handle root's own data
    size_t rootOffset = root * count * getFlagcxDataTypeSize(datatype);
    if (sendbuff != (char *)recvbuff + rootOffset) {
      memcpy((char *)recvbuff + rootOffset, sendbuff,
             getFlagcxDataTypeSize(datatype) * count);
    }

    // Receive data from other ranks
    for (int i = 0; i < nranks; ++i) {
      if (i != root) {
        int offset = i * count * getFlagcxDataTypeSize(datatype);
        FLAGCXCHECK(bootstrapRecv(commState, i, bootstrapTag,
                                  (char *)recvbuff + offset,
                                  count * getFlagcxDataTypeSize(datatype)));
      }
    }
  } else {
    // Non-root ranks send data to root
    FLAGCXCHECK(bootstrapSend(commState, root, bootstrapTag, (void *)sendbuff,
                              count * getFlagcxDataTypeSize(datatype)));
  }
  return flagcxSuccess;
}

flagcxResult_t bootstrapIntraNodeBarrier(void *commState, int *ranks, int rank,
                                         int nranks, int tag) {
  if (nranks == 1)
    return flagcxSuccess;
  TRACE(FLAGCX_INIT, "rank %d nranks %d tag %x - ENTER", rank, nranks, tag);

  /* Simple [intra] process barrier
   *
   * Based on the dissemination algorithm by Debra Hensgen, Raphael Finkel, and
   * Udi Manbet, "Two Algorithms for Barrier Synchronization," International
   * Journal of Parallel Programming, 17(1):1-17, 1988"
   */
  int data[1];
  for (int mask = 1; mask < nranks; mask <<= 1) {
    int src = (rank - mask + nranks) % nranks;
    int dst = (rank + mask) % nranks;
    FLAGCXCHECK(bootstrapSend(commState, ranks ? ranks[dst] : dst, tag, data,
                              sizeof(data)));
    FLAGCXCHECK(bootstrapRecv(commState, ranks ? ranks[src] : src, tag, data,
                              sizeof(data)));
  }

  TRACE(FLAGCX_INIT, "rank %d nranks %d tag %x - DONE", rank, nranks, tag);
  return flagcxSuccess;
}

flagcxResult_t bootstrapBarrier(void *commState, int rank, int nranks,
                                int tag) {
  return bootstrapIntraNodeBarrier(commState, NULL, rank, nranks, tag);
}

// [IntraNode] in-place Broadcast
flagcxResult_t bootstrapIntraNodeBroadcast(void *commState, int *ranks,
                                           int rank, int nranks, int root,
                                           void *bcastData, int size) {
  if (nranks == 1)
    return flagcxSuccess;
  TRACE(FLAGCX_INIT, "rank %d nranks %d root %d size %d - ENTER", rank, nranks,
        root, size);

  if (rank == root) {
    for (int i = 0; i < nranks; i++) {
      if (i != root)
        FLAGCXCHECK(bootstrapSend(commState, ranks ? ranks[i] : i,
                                  /*tag=*/ranks ? ranks[i] : i, bcastData,
                                  size));
    }
  } else {
    FLAGCXCHECK(bootstrapRecv(commState, ranks ? ranks[root] : root,
                              /*tag=*/ranks ? ranks[rank] : rank, bcastData,
                              size));
  }

  TRACE(FLAGCX_INIT, "rank %d nranks %d root %d size %d - DONE", rank, nranks,
        root, size);
  return flagcxSuccess;
}

flagcxResult_t bootstrapBroadcast(void *commState, int rank, int nranks,
                                  int root, void *bcastData, int size) {
  return bootstrapIntraNodeBroadcast(commState, NULL, rank, nranks, root,
                                     bcastData, size);
}

flagcxResult_t bootstrapClose(void *commState) {
  struct bootstrapState *state = (struct bootstrapState *)commState;
  if (state->unexpectedConnections != NULL) {
    unexpectedFree(state);
    if (__atomic_load_n(state->abortFlag, __ATOMIC_RELAXED) == 0) {
      WARN("Unexpected connections are not empty");
      return flagcxInternalError;
    }
  }

  FLAGCXCHECK(flagcxSocketClose(&state->listenSock));
  FLAGCXCHECK(flagcxSocketClose(&state->ringSendSocket));
  FLAGCXCHECK(flagcxSocketClose(&state->ringRecvSocket));

  free(state->peerCommAddresses);
  free(state);

  return flagcxSuccess;
}

flagcxResult_t bootstrapAbort(void *commState) {
  struct bootstrapState *state = (struct bootstrapState *)commState;
  if (commState == NULL)
    return flagcxSuccess;
  FLAGCXCHECK(flagcxSocketClose(&state->listenSock));
  FLAGCXCHECK(flagcxSocketClose(&state->ringSendSocket));
  FLAGCXCHECK(flagcxSocketClose(&state->ringRecvSocket));
  free(state->peerCommAddresses);
  free(state->peerProxyAddresses);
  free(state);
  return flagcxSuccess;
}
// AlltoALlv require sendbuff and recvbuff not overlap
flagcxResult_t AlltoAllvBootstrap(void *commState, const void *sendbuff,
                                  size_t *sendcounts, size_t *sdispls,
                                  void *recvbuff, size_t *recvcounts,
                                  size_t *rdispls, flagcxDataType_t datatype) {
  struct bootstrapState *state = (struct bootstrapState *)commState;
  int rank = state->rank;
  int nranks = state->nranks;
  size_t typeSize = getFlagcxDataTypeSize(datatype);

  for (int i = 0; i < nranks; ++i) {
    if (i == rank) {
      memcpy((void *)((char *)recvbuff + rdispls[i] * typeSize),
             (void *)((char *)sendbuff + sdispls[i] * typeSize),
             sendcounts[i] * typeSize);
    }
    const int bootstrapTag = -9995; // Suggest making this unique if possible
    if (rank > i) {
      // Send to rank i
      FLAGCXCHECK(
          bootstrapSend(commState, i, bootstrapTag,
                        (void *)((char *)sendbuff + sdispls[i] * typeSize),
                        sendcounts[i] * typeSize));
      // Recv from rank i
      FLAGCXCHECK(
          bootstrapRecv(commState, i, bootstrapTag,
                        (void *)((char *)recvbuff + rdispls[i] * typeSize),
                        recvcounts[i] * typeSize));
    } else if (rank < i) {
      // Receive from rank i
      FLAGCXCHECK(
          bootstrapRecv(commState, i, bootstrapTag,
                        (void *)((char *)recvbuff + rdispls[i] * typeSize),
                        recvcounts[i] * typeSize));
      // Send to rank i
      FLAGCXCHECK(
          bootstrapSend(commState, i, bootstrapTag,
                        (void *)((char *)sendbuff + sdispls[i] * typeSize),
                        sendcounts[i] * typeSize));
    }
  }
  return flagcxSuccess;
}
