/*************************************************************************
 * Copyright (c) 2016-2022, NVIDIA CORPORATION. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#include "proxy.h"
#include "adaptor.h"
#include "collectives.h"
#include "comm.h"
#include "info.h"
#include "net.h"
#include "socket.h"
#include "transport.h"
#define ENABLE_TIMER 0
#include "timer.h"

#include <assert.h>
#include <string>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
using namespace std;

enum { proxyRecv = 0, proxySend = 1 };
extern union flagcxSocketAddress bootstrapNetIfAddr;

static bool proxyMatchOpType(int type) {
  switch (type) {
    case flagcxProxyMsgInit:
    case flagcxProxyMsgSharedInit:
    case flagcxProxyMsgSetup:
    case flagcxProxyMsgConnect:
    case flagcxProxyMsgGetFd:
    case flagcxProxyMsgRegister:
    case flagcxProxyMsgDeregister:
    case flagcxProxyMsgRegMr:
    case flagcxProxyMsgDeregMr:
    case flagcxProxyMsgSendRecv:
      return true;
    default:
      return false;
  }
}

FLAGCX_TEMPLETELIST_DEFINE(ProdProgChannel, struct flagcxProxyOps,
                           prodPrevChannel, prodNextChannel);
FLAGCX_TEMPLETELIST_DEFINE(ConsProgChannel, struct flagcxProxyOps,
                           consPrevChannel, consNextChannel);
FLAGCX_TEMPLETELIST_DEFINE(ProgPeer, struct flagcxProxyOps::consPeer, prevPeer,
                           nextPeer);

flagcxResult_t
flagcxProxyProgressChannelJoin(struct flagcxProxyState *proxyState,
                               struct flagcxProxyState *) {

  return flagcxSuccess;
}

static flagcxResult_t asyncProxyOpEnqueue(flagcxProxyAsyncOp **opHead,
                                          flagcxProxyAsyncOp *newOp) {
  flagcxProxyAsyncOp *list = *opHead;
  if (list == NULL)
    *opHead = newOp;
  else {
    while (list->next)
      list = list->next;
    list->next = newOp;
    newOp->prev = list;
  }
  return flagcxSuccess;
}

static flagcxResult_t asyncProxyOpDequeue(flagcxProxyAsyncOp **opHead,
                                          flagcxProxyAsyncOp *op) {
  if (*opHead == op)
    *opHead = op->next;
  if (op->next)
    op->next->prev = op->prev;
  if (op->prev)
    op->prev->next = op->next;
  if (op->reqSize)
    free(op->reqBuff);
  if (op->respSize)
    free(op->respBuff);
  free(op);
  return flagcxSuccess;
}

static flagcxResult_t SaveProxy(struct flagcxHeteroComm *comm,
                                struct flagcxChannel *channel, int type,
                                int peer, struct flagcxProxyOp *op,
                                int connIndex, bool *justInquire) {
  if (peer < 0)
    return flagcxSuccess;

  if (justInquire)
    *justInquire = true;
  else {
    struct flagcxProxyOps *proxyOps;
    struct flagcxIntruQueue<struct flagcxProxyOp, &flagcxProxyOp::next> *queue;

    proxyOps = &comm->proxyState->proxyOps[op->channelId];
    queue = type == proxySend ? &proxyOps->prodPeers.sendQueue
                              : &proxyOps->prodPeers.recvQueue;

    pthread_mutex_lock(&comm->proxyState->mutex);
    flagcxProdProgChannelListEnList(&comm->proxyState->prodProgChannelHead,
                                    proxyOps);
    flagcxIntruQueueEnqueue(queue, op);
    pthread_cond_signal(&comm->proxyState->cond);
    pthread_mutex_unlock(&comm->proxyState->mutex);
  }
  return flagcxSuccess;
}

flagcxResult_t flagcxProxySaveOp(struct flagcxHeteroComm *comm,
                                 struct flagcxProxyOp *op, bool *justInquire) {
  struct flagcxChannel *channel = &comm->channels[op->channelId];
  if (justInquire)
    *justInquire = false;
  switch (op->pattern) {
    case flagcxPatternSend:
    case flagcxPatternRecv: {
      if (op->root == comm->rank)
        return flagcxSuccess;
      FLAGCXCHECK(
          SaveProxy(comm, channel,
                    op->pattern == flagcxPatternSend ? proxySend : proxyRecv,
                    op->root, op, 0, justInquire));
    } break;
  }
  return flagcxSuccess;
}

// Only for double check purpose, we can check if the progress queue is empty
// It is safe to not call this function in the progress thread.
static void flagcxProgressQueEmptyCheck(struct flagcxProxyState *proxyState) {
  bool error = 0;
  if (!flagcxProdProgChannelListEmpty(proxyState->prodProgChannelHead) ||
      !flagcxConsProgChannelListEmpty(proxyState->consProgChannelHead)) {
    error = 1;
  }
  for (int i = 0; i < MAXCHANNELS; i++) {
    if (!flagcxProgPeerListEmpty(proxyState->proxyOps[i].consProgPeerHead))
      error = 1;
    for (int r = 0; r < proxyState->nRanks; r++) {
      if (!flagcxIntruQueueEmpty(
              &proxyState->proxyOps[i].consPeers[r].sendQueue) ||
          !flagcxIntruQueueEmpty(
              &proxyState->proxyOps[i].consPeers[r].recvQueue))
        error = 1;
    }
    if (!flagcxIntruQueueEmpty(&proxyState->proxyOps[i].prodPeers.sendQueue) ||
        !flagcxIntruQueueEmpty(&proxyState->proxyOps[i].prodPeers.recvQueue))
      error = 1;
  }
  if (error)
    INFO(FLAGCX_INIT, "progress queue is not empty");
}

// process all the ProxyOps in the consumer queue
// idle is set to 1 if no operations are pending
// if idle is set to 0, it means there are pending operations
// For simplicity, if these are any pending operations in queue, we set idle to
// 0
static flagcxResult_t progressOps(struct flagcxProxyState *proxyState,
                                  int *idle) {
  *idle = 1;
  if (!flagcxConsProgChannelListEmpty(proxyState->consProgChannelHead)) {
    struct flagcxProxyOps *proxyOps = proxyState->consProgChannelHead;
    do {
      struct flagcxProxyOps *next = proxyOps->consNextChannel;

      if (!flagcxProgPeerListEmpty(proxyOps->consProgPeerHead)) {
        struct flagcxProxyOps::consPeer *peer = proxyOps->consProgPeerHead;
        do {
          struct flagcxProxyOps::consPeer *next = peer->nextPeer;
          struct flagcxIntruQueue<struct flagcxProxyOp, &flagcxProxyOp::next>
              *queue;
          queue = &peer->sendQueue;
          if (!flagcxIntruQueueEmpty(queue)) {
            *idle &= 0;
            struct flagcxProxyOp *op = flagcxIntruQueueHead(queue);
            struct sendNetResources *resources =
                (sendNetResources *)op->connection->transportResources;
            flagcxProxySend(resources, op->recvbuff, op->nbytes, &op->args);
            if (op->args.done == 1 && op->args.eventRecorded) {
              // The P2P object should not be destroyed until the associated
              // event has completed
              if (deviceAdaptor->eventQuery(op->event) == flagcxSuccess) {
                flagcxIntruQueueDelete(queue, op);
                FLAGCXCHECK(deviceAdaptor->eventDestroy(op->event));
                free(op);
              }
            }
          }
          queue = &peer->recvQueue;
          if (!flagcxIntruQueueEmpty(queue)) {
            *idle &= 0;
            struct flagcxProxyOp *op = flagcxIntruQueueHead(queue);
            struct recvNetResources *resources =
                (recvNetResources *)op->connection->transportResources;
            flagcxProxyRecv(resources, op->recvbuff, op->nbytes, &op->args);
            if (op->args.done == 1 && op->args.eventRecorded) {
              // The P2P object should not be destroyed until the associated
              // event has completed
              if (deviceAdaptor->eventQuery(op->event) == flagcxSuccess) {
                flagcxIntruQueueDelete(queue, op);
                FLAGCXCHECK(deviceAdaptor->eventDestroy(op->event));
                free(op);
              }
            }
          }
          if (flagcxIntruQueueEmpty(&peer->sendQueue) &&
              flagcxIntruQueueEmpty(&peer->recvQueue)) {
            flagcxProgPeerListDelete(&proxyOps->consProgPeerHead, peer);
          }
          peer = next;
        } while (peer != NULL);
      }
      if (flagcxProgPeerListEmpty(proxyOps->consProgPeerHead)) {
        flagcxConsProgChannelListDelete(&proxyState->consProgChannelHead,
                                        proxyOps);
      }
      proxyOps = next;
    } while (proxyOps != NULL);
  }
  return flagcxSuccess;
}

// get proxy operations from the producer queue
// and move them to the consumer queue
// added means the number of operations fetched from producer queue and added to
// the consumer queue.
static flagcxResult_t
flagcxProxyGetPostedOps(struct flagcxProxyState *proxyState, int *added) {
  struct flagcxProxyProgressState *state = &proxyState->progressState;
  // No need to block waiting for the lock to be available. Exit, continue
  // progress, and come back later.
  if (pthread_mutex_trylock(&proxyState->mutex) != 0) {
    *added = 0;
    return flagcxSuccess;
  }

  // If we have ops to progress, no need to block waiting for something to
  // arrive
  if (flagcxConsProgChannelListEmpty(proxyState->consProgChannelHead)) {
    while (flagcxProdProgChannelListEmpty(proxyState->prodProgChannelHead) &&
           state->stop == 0) {
      pthread_cond_wait(&proxyState->cond, &proxyState->mutex);
    }
    if (state->stop != 0) {
      pthread_mutex_unlock(&proxyState->mutex);
      *added = 0;
      return flagcxSuccess;
    }
  }

  // Put anything available right now in the producer queue into the consumer
  // queue.
  while (!flagcxProdProgChannelListEmpty(proxyState->prodProgChannelHead)) {
    struct flagcxProxyOps *proxyOps =
        flagcxProdProgChannelListDeList(&proxyState->prodProgChannelHead);

    flagcxConsProgChannelListEnList(&proxyState->consProgChannelHead, proxyOps);
    struct flagcxIntruQueue<struct flagcxProxyOp, &flagcxProxyOp::next> *queue;
    queue = &proxyOps->prodPeers.sendQueue;
    while (!flagcxIntruQueueEmpty(queue)) {
      struct flagcxProxyOp *op = flagcxIntruQueueDequeue(queue);
      flagcxProgPeerListEnList(&proxyOps->consProgPeerHead,
                               &proxyOps->consPeers[op->root]);
      flagcxIntruQueueEnqueue(&proxyOps->consPeers[op->root].sendQueue, op);
      (*added)++;
    }
    queue = &proxyOps->prodPeers.recvQueue;
    while (!flagcxIntruQueueEmpty(queue)) {
      struct flagcxProxyOp *op = flagcxIntruQueueDequeue(queue);
      flagcxProgPeerListEnList(&proxyOps->consProgPeerHead,
                               &proxyOps->consPeers[op->root]);
      flagcxIntruQueueEnqueue(&proxyOps->consPeers[op->root].recvQueue, op);
      (*added)++;
    }
  }
  pthread_mutex_unlock(&proxyState->mutex);
  return flagcxSuccess;
}

FLAGCX_PARAM(ProgressAppendOpFreq, "PROGRESS_APPENDOP_FREQ", 8);

//执行者工人代码
inline void *flagcxProxyProgress(void *proxyState_) {
  struct flagcxProxyState *proxyState = (flagcxProxyState *)proxyState_;
  // flag indicating if there is any in-operating operation
  int idle = 1;
  /* Too frequent call of ncclProxyGetPostedOps() will result in perf regression
   * for small message communication. proxyOpAppendCounter is a counter that
   * helps us decide if we need to append proxy ops. After each progress,
   * proxyOpAppendCounter will increase by 1 and compare with environment
   * variable ncclParamProgressAppendOpFreq(). If they are equal, we will append
   * proxy ops. This will decrease the frequency of calling
   * ncclProxyGetPostedOps() and reduce the perf impact. */
  int proxyOpAppendCounter = 0;
  deviceAdaptor->setDevice(proxyState->cudaDev);
  struct flagcxProxyProgressState *state = &proxyState->progressState;

  //循环持续运行，直到收到停止信号state->stop == 1并且所有任务都已经完成idle == 1
  while (state->stop == 0 || idle == 0) {
    idle = 1;//假设本轮无事可做
    // consume the operations in the consumer queue
    progressOps(proxyState, &idle);//遍历当前消费者队列的所有异步操作

    //(++proxyOpAppendCounter == flagcxParamProgressAppendOpFreq())优化
    //引入计数器proxyOpAppendCounter和可配置的频率flagcxParamProgressAppendOpFreq，progress线程不是每次循环都去检查新任务，而是每隔N次循环才去检查一次
    //降低了访问频率，提高性能
    if (idle || (++proxyOpAppendCounter == flagcxParamProgressAppendOpFreq())) {//如果没有任务就去获取新任务
      int added = 0;
      proxyOpAppendCounter = 0;
      if (state->stop == 0) {
        // move all the operations from the producer queue to the consumer queue
        flagcxProxyGetPostedOps(proxyState, &added);//加锁，然后把生产者队列中所有的待办任务，一次性移动到消费者队列，给下一轮progressOps处理
      }
      if (added == 0) {
        sched_yield(); // No request progressed. Let others run.
        //完成当前任务，也没有从生产者队列获得新任务，调用sched_yield系统调用主动放弃当前的CPU时间片。
      }
    }
  }

  flagcxProgressQueEmptyCheck(proxyState);//最后做一次检查，退出
  return NULL;
}

static flagcxResult_t expectedProxyResponseStore(struct flagcxProxyState *state,
                                                 void *opId, void *respBuff,
                                                 int respSize,
                                                 flagcxResult_t res) {
  struct flagcxExpectedProxyResponse *elem = state->expectedResponses;
  while (elem) {
    if (elem->opId == opId) {
      if (respSize != elem->respSize) {
        WARN("Mismatched response size for opId=%p", opId);
        return flagcxInternalError;
      }

      if (elem->done) {
        WARN("Storing response for already completed opId=%p", opId);
        return flagcxInternalError;
      }

      memcpy(elem->respBuff, respBuff, respSize);
      free(respBuff);
      elem->done = true;
      elem->res = res;
      return flagcxSuccess;
    }
    elem = elem->next;
  }

  WARN("Proxy response for opId=%p doesn't match any expected response", opId);
  return flagcxInternalError;
}

static flagcxResult_t
expectedProxyResponseEnqueue(struct flagcxProxyState *state, void *opId,
                             int respSize) {
  struct flagcxExpectedProxyResponse *ex;
  FLAGCXCHECK(flagcxCalloc(&ex, 1));
  ex->opId = opId;

  // Pre-alloc response buffer
  ex->respBuff = malloc(respSize);
  ex->respSize = respSize;
  ex->res = flagcxInternalError;
  ex->done = false;

  // Enqueue
  struct flagcxExpectedProxyResponse *list = state->expectedResponses;
  if (list == NULL) {
    state->expectedResponses = ex;
    return flagcxSuccess;
  }
  while (list->next)
    list = list->next;
  list->next = ex;
  return flagcxSuccess;
}

static flagcxResult_t
expectedProxyResponseDequeue(struct flagcxProxyState *state, void *opId,
                             void *respBuff, int *found) {
  struct flagcxExpectedProxyResponse *elem = state->expectedResponses;
  struct flagcxExpectedProxyResponse *prev = NULL;
  *found = 0;
  while (elem) {
    if ((elem->opId == opId) && elem->done) {
      if (prev == NULL) {
        state->expectedResponses = elem->next;
      } else {
        prev->next = elem->next;
      }
      memcpy(respBuff, elem->respBuff, elem->respSize);
      flagcxResult_t res = elem->res;
      free(elem->respBuff);
      free(elem);
      *found = 1;
      return res;
    }
    prev = elem;
    elem = elem->next;
  }
  return flagcxSuccess;
}

static flagcxResult_t
expectedProxyResponseRemove(struct flagcxProxyState *state, void *opId) {
  struct flagcxExpectedProxyResponse *elem = state->expectedResponses;
  struct flagcxExpectedProxyResponse *prev = NULL;
  while (elem) {
    if (elem->opId == opId) {
      if (prev == NULL) {
        state->expectedResponses = elem->next;
      } else {
        prev->next = elem->next;
      }
      free(elem->respBuff);
      free(elem);
      return flagcxSuccess;
    }
    prev = elem;
    elem = elem->next;
  }
  WARN("Couldn't find opId=%p", opId);
  return flagcxInternalError;
}

flagcxResult_t flagcxPollProxyResponse(struct flagcxHeteroComm *comm,
                                       struct flagcxProxyConnector *proxyConn,
                                       void *respBuff, void *opId) {
  struct flagcxProxyState *sharedProxyState = comm->proxyState;
  // Check response queue
  int found = 0;
  flagcxResult_t res =
      expectedProxyResponseDequeue(sharedProxyState, opId, respBuff, &found);

  if (found == 0) {
    // Attempt to read in a new response header from the proxy thread
    struct flagcxSocket *sock = &sharedProxyState->peerSock;
    flagcxProxyRpcResponseHeader resp = {0};
    int offset = 0;
    if (flagcxSuccess != flagcxSocketProgress(FLAGCX_SOCKET_RECV, sock, &resp,
                                              sizeof(resp), &offset)) {
      WARN("Socket recv failed while polling for opId=%p", opId);
      return flagcxInternalError;
    }

    if (offset == 0) {
      return flagcxInProgress;
      // If we've returned a partial response, block to receive the rest of it
    } else if (offset < sizeof(resp)) {
      while (offset < sizeof(resp))
        FLAGCXCHECK(flagcxSocketProgress(FLAGCX_SOCKET_RECV, sock, &resp,
                                         sizeof(resp), &offset));
    }

    INFO(FLAGCX_PROXY, "flagcxPollProxyResponse Received new opId=%p",
         resp.opId);

    // If there's a respSize to recv
    if (resp.respSize > 0) {
      if (resp.opId != opId) {
        // Unexpected response, need to buffer the socket data
        respBuff = malloc(resp.respSize);
      }
      assert(respBuff != NULL);
      FLAGCXCHECK(flagcxSocketRecv(sock, respBuff, resp.respSize));
    }

    if (resp.opId == opId) {
      INFO(FLAGCX_PROXY, "resp.opId=%p matches expected opId=%p", resp.opId,
           opId);
      FLAGCXCHECK(expectedProxyResponseRemove(sharedProxyState, resp.opId));
      return resp.res;
    } else {
      INFO(FLAGCX_PROXY, "Queuing opId=%p respBuff=%p respSize=%d", resp.opId,
           respBuff, resp.respSize);
      // Store the result and mark response as completed
      FLAGCXCHECK(expectedProxyResponseStore(
          sharedProxyState, resp.opId, respBuff, resp.respSize, resp.res));
      return flagcxInProgress;
    }
  } else {
    INFO(FLAGCX_PROXY, "flagcxPollProxyResponse Dequeued cached opId=%p", opId);
  }
  return res;
}

static flagcxResult_t proxyProgressAsync(flagcxProxyAsyncOp **opHead,
                                         flagcxProxyAsyncOp *op,
                                         int *asyncOpCount) {
  int done = 0;
  const char *dmaBufEnable = flagcxGetEnv("FLAGCX_DMABUF_ENABLE");
  bool dmaEnabled = false; // disabled by default
  if (dmaBufEnable != NULL) {
    if (strcmp(dmaBufEnable, "1") == 0) {
      dmaEnabled = true;
    }
  }
  if (op->type == flagcxProxyMsgConnect) {
    if (op->connection->send) {
      struct sendNetResources *resources =
          (struct sendNetResources *)op->connection->transportResources;
      if (!resources->netSendComm) {
        FLAGCXCHECK(resources->netAdaptor->connect(
            resources->netDev, (void *)op->reqBuff, &resources->netSendComm));
      } else {
        bool dmaBufferSupport = false;
        if (deviceAdaptor->dmaSupport != NULL) {
          deviceAdaptor->dmaSupport(&dmaBufferSupport);
        }
        if (dmaBufferSupport && dmaEnabled) {
          INFO(FLAGCX_PROXY, "Registering memory region with DMA-BUF support");
          int dmabuf_fd;
          FLAGCXCHECK(deviceAdaptor->getHandleForAddressRange(
              (void *)&dmabuf_fd, resources->buffers[0],
              resources->buffSizes[0], 0));
          FLAGCXCHECK(resources->netAdaptor->regMrDmaBuf(
              resources->netSendComm, resources->buffers[0],
              resources->buffSizes[0], 2, 0ULL, dmabuf_fd,
              &resources->mhandles[0]));
        } else {
          if (resources->netAdaptor == getUnifiedNetAdaptor(IBRC)) {
            FLAGCXCHECK(resources->netAdaptor->regMr(
                resources->netSendComm, resources->buffers[0],
                resources->buffSizes[0], 2, &resources->mhandles[0]));
          } else if (resources->netAdaptor == getUnifiedNetAdaptor(SOCKET)) {
            FLAGCXCHECK(resources->netAdaptor->regMr(
                resources->netSendComm, resources->buffers[0],
                resources->buffSizes[0], 1, &resources->mhandles[0]));
          }
        }
        done = 1;
      }
    } else {
      struct recvNetResources *resources =
          (struct recvNetResources *)op->connection->transportResources;
      if (!resources->netRecvComm) {
        FLAGCXCHECK(resources->netAdaptor->accept(resources->netListenComm,
                                                  &resources->netRecvComm));
      } else {
        bool dmaBufferSupport = false;
        if (deviceAdaptor->dmaSupport != NULL) {
          deviceAdaptor->dmaSupport(&dmaBufferSupport);
        }
        if (dmaBufferSupport && dmaEnabled) {
          INFO(FLAGCX_PROXY, "Registering memory region with DMA-BUF support");
          int dmabuf_fd;
          FLAGCXCHECK(deviceAdaptor->getHandleForAddressRange(
              (void *)&dmabuf_fd, resources->buffers[0],
              resources->buffSizes[0], 0));
          FLAGCXCHECK(resources->netAdaptor->regMrDmaBuf(
              resources->netRecvComm, resources->buffers[0],
              resources->buffSizes[0], 2, 0ULL, dmabuf_fd,
              &resources->mhandles[0]));
        } else {
          if (resources->netAdaptor == getUnifiedNetAdaptor(IBRC)) {
            FLAGCXCHECK(resources->netAdaptor->regMr(
                resources->netRecvComm, resources->buffers[0],
                resources->buffSizes[0], 2, &resources->mhandles[0]));
          } else if (resources->netAdaptor == getUnifiedNetAdaptor(SOCKET)) {
            FLAGCXCHECK(resources->netAdaptor->regMr(
                resources->netRecvComm, resources->buffers[0],
                resources->buffSizes[0], 1, &resources->mhandles[0]));
          }
        }
        done = 1;
      }
    }
  } else
    return flagcxInternalError;

  if (done) {
    INFO(FLAGCX_PROXY,
         "proxyProgressAsync opId=%p op.type=%d op.reqBuff=%p op.respSize=%d "
         "done",
         op->opId, op->type, op->reqBuff, op->respSize);
    if (op->type == flagcxProxyMsgConnect)
      __atomic_store_n(&op->connection->state, connConnected, __ATOMIC_RELEASE);

    /* if setup or connect is done, we should not return any error at this point
     * since flagcxSocketSend might already send the respBuff to the requester.
     * If we still choose to abort and close the connection, it can cause
     * segfault if the requester is using the respBuff. */

    flagcxProxyRpcResponseHeader resp = {op->opId, flagcxSuccess, op->respSize};

    // Send the opId for referencing async operation
    FLAGCXCHECK(flagcxSocketSend(op->connection->sock, &resp, sizeof(resp)));
    if (op->respSize) {
      // Send the response
      FLAGCXCHECK(
          flagcxSocketSend(op->connection->sock, op->respBuff, op->respSize));
    }

    asyncProxyOpDequeue(opHead, op);
    (*asyncOpCount)--;
    return flagcxSuccess;
  }

  return flagcxInProgress;
}

flagcxResult_t flagcxProxyCallAsync(struct flagcxHeteroComm *comm,
                                    struct flagcxProxyConnector *proxyConn,
                                    int type, void *reqBuff, int reqSize,
                                    int respSize, void *opId) {
  struct flagcxSocket *sock;
  flagcxResult_t ret = flagcxSuccess;
  struct flagcxProxyState *sharedProxyState = comm->proxyState;

  sock = &sharedProxyState->peerSock;
  if (sock == NULL)
    return flagcxInternalError;

  FLAGCXCHECKGOTO(flagcxSocketSend(sock, &type, sizeof(int)), ret, error);
  FLAGCXCHECKGOTO(
      flagcxSocketSend(sock, &proxyConn->connection, sizeof(void *)), ret,
      error);
  FLAGCXCHECKGOTO(flagcxSocketSend(sock, &reqSize, sizeof(int)), ret, error);
  FLAGCXCHECKGOTO(flagcxSocketSend(sock, &respSize, sizeof(int)), ret, error);
  if (reqSize)
    FLAGCXCHECKGOTO(flagcxSocketSend(sock, reqBuff, reqSize), ret, error);

  // Send opId to proxy
  FLAGCXCHECKGOTO(flagcxSocketSend(sock, &opId, sizeof(opId)), ret, error);

  FLAGCXCHECK(expectedProxyResponseEnqueue(sharedProxyState, opId, respSize));
  return flagcxSuccess;
error:
  return ret;
}

static flagcxResult_t proxyServiceInitOp(int type, struct flagcxSocket *sock,
                                         struct flagcxProxyAsyncOp **opHead,
                                         flagcxHeteroComm_t comm,
                                         int *asyncOpCount) {
  struct flagcxProxyAsyncOp *asyncOp;
  FLAGCXCHECK(flagcxCalloc(&asyncOp, 1));

  asyncOp->type = type;
  FLAGCXCHECK(flagcxSocketRecv(sock, &asyncOp->connection, sizeof(void *)));

  FLAGCXCHECK(flagcxSocketRecv(sock, &asyncOp->reqSize, sizeof(int)));
  FLAGCXCHECK(flagcxSocketRecv(sock, &asyncOp->respSize, sizeof(int)));
  if (asyncOp->reqSize) {
    FLAGCXCHECK(flagcxCalloc(&asyncOp->reqBuff, asyncOp->reqSize));
    FLAGCXCHECK(flagcxSocketRecv(sock, asyncOp->reqBuff, asyncOp->reqSize));
  }

  // Store opId for completion response
  FLAGCXCHECK(flagcxSocketRecv(sock, &asyncOp->opId, sizeof(asyncOp->opId)));

  asyncOp->connection->sock = sock;
  if (asyncOp->respSize)
    FLAGCXCHECK(flagcxCalloc(&asyncOp->respBuff, asyncOp->respSize));

  FLAGCXCHECK(asyncProxyOpEnqueue(opHead, asyncOp));
  (*asyncOpCount)++;
  FLAGCXCHECK(proxyProgressAsync(opHead, asyncOp, asyncOpCount));
  return flagcxSuccess;
}

flagcxResult_t flagcxProxyCallBlocking(struct flagcxHeteroComm *comm,
                                       struct flagcxProxyConnector *proxyConn,
                                       int type, void *reqBuff, int reqSize,
                                       void *respBuff, int respSize) {
  // Alloc some memory to act as a handle
  flagcxResult_t res = flagcxSuccess;
  void *opId = malloc(1);

  FLAGCXCHECKGOTO(flagcxProxyCallAsync(comm, proxyConn, type, reqBuff, reqSize,
                                       respSize, opId),
                  res, fail);

  do {
    res = flagcxPollProxyResponse(comm, proxyConn, respBuff, opId);
  } while (res == flagcxInProgress);

exit:
  free(opId);
  return res;
fail:
  goto exit;
}

flagcxResult_t flagcxProxyInit(struct flagcxHeteroComm *comm) {
  INFO(FLAGCX_INIT, "rank=%d flagcxProxyInit called.", comm->rank);
  FLAGCXCHECK(flagcxSocketInit(&comm->proxyState->listenSock,
                               &bootstrapNetIfAddr, comm->magic,
                               flagcxSocketTypeProxy, NULL, 1));
  FLAGCXCHECK(flagcxSocketListen(&comm->proxyState->listenSock));

  flagcxSocket *proxySock = &comm->proxyState->peerSock;
  FLAGCXCHECK(flagcxSocketInit(proxySock, &comm->proxyState->listenSock.addr,
                               comm->magic, flagcxSocketTypeProxy));//创建第二个套接字，去连接刚刚的监听套接字（第二个参数）
  FLAGCXCHECK(flagcxSocketConnect(proxySock));

  //发送自己rank消息，让监听端知道这个连接是哪个rank的代理
  char proxyMsg[10];
  memcpy(proxyMsg, (string("Proxy: ") + to_string(comm->rank)).c_str(), 10);
  flagcxSocketSend(proxySock, proxyMsg, 10);
  //用pthread_create创建了两个独立的、持续运行的后台线程，持续监听并接收来自其它或自己的通信任务请求，收到请求后放入基于共享内存的环形缓冲区中
  comm->proxyState->cudaDev = comm->cudaDev;
  pthread_create(&comm->proxyState->thread, NULL, flagcxProxyService,
                 (void *)comm);//flagcxProxyService接收和分派任务
  pthread_create(&comm->proxyState->progressState.thread, NULL,
                 flagcxProxyProgress, comm->proxyState);//执行具体的任务
  comm->proxyState->initialized = 1;
  return flagcxSuccess;
}


//service线程，调度者，这个后台线程一旦启动，就进入循环，直到收到停止信号，持续接收新任务，推进旧任务
void *flagcxProxyService(void *args) {
  int stop = 0;
  int closeConn = 0;
  int asyncOpCount = 0;
  struct flagcxHeteroComm *comm = (struct flagcxHeteroComm *)args;
  struct flagcxProxyAsyncOp *opHead = NULL;
  struct flagcxProxyAsyncOp *list = NULL;
  struct flagcxSocket sock;
  flagcxResult_t res = flagcxSuccess;

  //初始化与连接建立，线程启动后的准备工作
  // Set device context
  FLAGCXCHECKGOTO(deviceAdaptor->setDevice(comm->cudaDev), res, out);//确保setDevice设备上下文绑定正确

  // One peer only
  FLAGCXCHECKGOTO(flagcxSocketInit(&sock), res, out);
  FLAGCXCHECKGOTO(flagcxSocketAccept(&sock, &comm->proxyState->listenSock), res,
                  out);//线程阻塞在这里，等待一个连接请求，对应flagcxProxyInit函数中的自连接flagcxSocketConnect
  char proxyMsg[10];
  flagcxSocketRecv(&sock, proxyMsg, 10);//接收代理的rank消息，完成握手
  INFO(FLAGCX_PROXY,
       "[Service thread] Receive proxy message : \033[31m%s\033[0m", proxyMsg);
  struct pollfd pollfds[1];//事件循环的核心机制，让操作系统内核监控sock.fd这个套接字，有数据可读POLLIN的时候通知
  pollfds[0].fd = sock.fd;
  pollfds[0].events = POLLIN;

  //核心事件循环，循环一直运行，直到收到停止信号stop=1并且所有异步操作都已处理完毕（opHead为空）
  while (!stop || (stop && opHead)) {
    int ret;
    do {
      ret = poll(pollfds, 1, asyncOpCount ? 0 : 500);//poll是系统调用，让线程睡眠，直到监控套接字上有事件发生或超时
      //如果当前有正在进行的异步操作asyncOpCount>0，timeout=0不阻塞立马推进旧人物；如果当前没有任务asyncOpCount==0，timeout设为500，线程睡眠半秒，等待新任务到来
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
      WARN("[Proxy Service] Poll failed: %s", strerror(errno));
      closeConn = 1;
      break;
    }
    if (closeConn) {
      break;
    }

    // Progress all ops
    list = opHead;//遍历当前正在进行的异步操作链表opHead
    while (list) {
      struct flagcxProxyAsyncOp *opNext = list->next;
      res = proxyProgressAsync(&opHead, list, &asyncOpCount);//对每个操作调用proxyProgressAsync函数推进它的状态
      if (res == flagcxSuccess || res == flagcxInProgress) {//如果一个操作已经完成（res == flagcxSuccess）就被链表移除，如果在进行中res == flagcxInProgress，继续保留在链表，等待下一次循环再推进
        list = opNext;
      } else {
        WARN("[Service thread] Error encountered progressing operation with "
             "res=%d, closing connection",
             res);
        closeConn = 1;
        break;
      }
    }
    if (closeConn) {
      break;
    }

    // Check for additional ops coming in
    //接收新操作
    int type;
    if (pollfds[0].revents & POLLIN) {//如果poll告诉套接字上有新数据POLLIN，代码进入这个分支
      int closed = 0;
      res = flagcxSocketTryRecv(&sock, &type, sizeof(int), &closed,
                                false /*blocking*/);//flagcxSocketTryRecv非阻塞地尝试从套接字接收一个操作类型type
      if (res != flagcxSuccess && res != flagcxInProgress) {
        WARN("[Service thread] Could not receive type from rank %d, "
             "res=%u, "
             "closed=%d",
             comm->rank, res, closed);
        closeConn = 1;
      } else if (closed) {
        INFO(FLAGCX_PROXY, "[Service thread] Connection closed by rank %d",
             comm->rank);
        closeConn = 1;
      } else if (res == flagcxSuccess) {
        if (type == flagcxProxyMsgStop) {//停止消息
          stop = 1;
          closeConn = 1;
        } else if (proxyMatchOpType(type)) {//已知的通信操作类型，如send和revc，调用proxyServiceInitOp
          res = proxyServiceInitOp(type, &sock, &opHead, comm, &asyncOpCount);//从套接字读取操作需要的所有参数，创建一个新的flagcxProxyAsyncOp对象，加入到onHead链表的头部
          if (res != flagcxSuccess) {
            WARN("[Service thread] Error encountered initializing operation "
                 "with res=%d, closing connection",
                 res);
            closeConn = 1;
          }
        } else {
          INFO(FLAGCX_PROXY, "[Service thread] Unknown command %d from rank %d",
               type, comm->rank);
          closeConn = 1;
        }
      }
    }
    if (closeConn) {
      break;
    }
  }

//清理与退出：主线程退出后，执行最后的清理工作
out:
  // Stop progress thread before freeing any resource
  pthread_mutex_lock(&comm->proxyState->mutex);
  comm->proxyState->progressState.stop = 1;//通知flagcxProxyProgress工人线程停止，设置stop标志
  pthread_cond_signal(&comm->proxyState->cond);
  pthread_mutex_unlock(&comm->proxyState->mutex);
  pthread_join(comm->proxyState->progressState.thread, nullptr);//阻塞等待，直到工人线程完全退出，确保释放资源之前，没有其它任何线程都在使用它们

  // Close sockets
  //关闭套接字
  flagcxSocketClose(&sock);
  flagcxSocketClose(&comm->proxyState->listenSock);

  // Dequeue unhandled ops
  list = opHead;
  while (list) {
    struct flagcxProxyAsyncOp *opNext = list->next;
    asyncProxyOpDequeue(&opHead, list);
    list = opNext;
  }

  INFO(FLAGCX_PROXY,
       "[Service thread] Wait for progress thread joined and free resources");
  return NULL;
}

flagcxResult_t flagcxProxyFree(struct flagcxHeteroComm *comm) {
  for (int peer = 0; peer < comm->nRanks; peer++) {
    for (int c = 0; c < MAXCHANNELS; c++) {
      if (comm->channels[c].peers[peer]->recv[0].connected == 1) {
        struct flagcxConnector *conn = comm->channels[c].peers[peer]->recv;
        struct recvNetResources *resources =
            (struct recvNetResources *)
                conn->proxyConn.connection->transportResources;
        flagcxRecvProxyFree(resources);
      }
      if (comm->channels[c].peers[peer]->send[0].connected == 1) {
        struct flagcxConnector *conn = comm->channels[c].peers[peer]->send;
        struct sendNetResources *resources =
            (struct sendNetResources *)
                conn->proxyConn.connection->transportResources;
        flagcxSendProxyFree(resources);
      }
    }
  }
  return flagcxSuccess;
}

flagcxResult_t flagcxProxyDestroy(struct flagcxHeteroComm *comm) {
  if (comm->proxyState->initialized == 1) {
    int type = flagcxProxyMsgStop;
    flagcxSocketSend(&comm->proxyState->peerSock, &type, sizeof(int));
    pthread_join(comm->proxyState->thread, nullptr);
    flagcxProxyFree(comm);
  }
  return flagcxSuccess;
}
