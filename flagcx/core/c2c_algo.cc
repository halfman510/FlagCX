#include "c2c_algo.h"
#include "c2c_ir.h"
#include <cstdint>
#include <cstdlib>
#include <stdlib.h>

// GCD using Euclidean algorithm
inline int gcd(int a, int b) {
  while (b != 0) {
    int tmp = b;
    b = a % b;
    a = tmp;
  }
  return a;
}

// LCM using GCD
inline int lcm(int a, int b) { return std::abs(a * b) / gcd(a, b); }

// LCM of clusterInterRankList
inline int getLcmOfInterRankList(
    const std::vector<std::vector<int>> &clusterInterRankList) {
  int result = 1;
  for (size_t i = 0; i < clusterInterRankList.size(); ++i) {
    result = lcm(result, clusterInterRankList[i].size());
  }
  return result;
}

size_t getC2cCommPatternHash(size_t count, size_t rootClusterId,
                             flagcxCommOp_t commOp, flagcxRedOp_t redOp,
                             flagcxComm_t comm) {
  std::size_t h1 = std::hash<size_t>()(count);
  std::size_t h2 = std::hash<size_t>()(rootClusterId);
  std::size_t h3 = std::hash<size_t>()(commOp);
  std::size_t h4 = std::hash<size_t>()(redOp);
  std::size_t h5 = std::hash<size_t>()((size_t)((uintptr_t)comm));
  std::size_t h = h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3) ^ (h5 << 4);
  return (static_cast<size_t>(h) << 4) + static_cast<size_t>(commOp);
}

flagcxInterRankBufferInfoManager::flagcxInterRankBufferInfoManager(
    size_t totalCount)
    : totalCount_(totalCount) {}

flagcxInterRankBufferInfoManager::~flagcxInterRankBufferInfoManager() {}

//检查新的传输请求是否与已有的请求时空上冲突
bool flagcxInterRankBufferInfoManager::checkIfPossibleToPush(int clusterId,
                                                             int rank,
                                                             size_t offset,
                                                             size_t count) {
  if (auto clusterSearch = bufferInfos_.find(clusterId);//三层结构查找
      clusterSearch != bufferInfos_.end()) {
    if (auto rankSearch = clusterSearch->second.find(rank);//三层结构查找
        rankSearch != clusterSearch->second.end()) {
      auto infoList = rankSearch->second;
      for (auto info : infoList) {//遍历列表中的计划
        //下面的if包括三种重叠情况，1、后半段落在info内 2、完全重合 3、前半段落在info内
        if ((offset < info.offset_ && offset + count > info.offset_) ||
            offset == info.offset_ ||
            (offset > info.offset_ && offset < info.offset_ + info.count_)) {
          return false;
        }
      }
    }
  }
  return true;
}

//如果大传输请求因为中间某个计划无法被直接调度，检查是否可以把大请求差分，利用冲突前面或后面空闲的时间，先传输一部分数据
bool flagcxInterRankBufferInfoManager::checkIfPossibleToSplitAndPush(
    int clusterId, int rank, size_t offset, size_t count, size_t *splitCount,
    int *pushMode) {
  size_t maxSplitCount = 0;//记录见缝插针后可传输的最大数据块大小
  int finalPushMode = 0; // 0: prePush, 1: postPush 返回在冲突块之前还是之后传输
  if (auto clusterSearch = bufferInfos_.find(clusterId);//三层结构查找
      clusterSearch != bufferInfos_.end()) {
    if (auto rankSearch = clusterSearch->second.find(rank);//三层结构查找
        rankSearch != clusterSearch->second.end()) {
      auto infoList = rankSearch->second;
      for (auto info : infoList) {//遍历列表中的计划
        if (offset < info.offset_ && offset + count > info.offset_) {//检查新请求[offset, offset+count)是否
        // 跨越已存在的计划info起始点。如果是，那info前面的区间可能是空闲的窗口，递归调用checkIfPossibleToPush确
        // 认小窗口是否可用，因为可能与更早的计划冲突
          if (checkIfPossibleToPush(clusterId, rank, offset,
                                    info.offset_ - offset)) {
            maxSplitCount = std::max(info.offset_ - offset, maxSplitCount);//如果可用，计算出长度并更新最大可用窗口
            finalPushMode = 0;
          }
        }
        //类似的，检测info后面是否有空闲的窗口可用
        if (offset >= info.offset_ && offset < info.offset_ + info.count_ &&
            offset + count > info.offset_ + info.count_) {//递归调用checkIfPossibleToPush确认后面的小窗口是否可用
          if (checkIfPossibleToPush(clusterId, rank, info.offset_ + info.count_,
                                    offset + count - info.offset_ -
                                        info.count_)) {
            maxSplitCount = std::max(
                offset + count - info.offset_ - info.count_, maxSplitCount);
            finalPushMode = 1;
          }
        }
      }
      if (maxSplitCount > 0) {//有最大可用窗口，就返回true
        *splitCount = maxSplitCount;
        *pushMode = finalPushMode;
        return true;
      }
    }
  }
  return false;
}

//下面是一套CURD接口，供C2C Planner等上层模块使用
//判断特定rank的缓冲通信区是否已满
bool flagcxInterRankBufferInfoManager::checkIsFull(int clusterId, int rank) {
  int rankCount = 0;
  if (auto clusterSearch = bufferInfos_.find(clusterId);
      clusterSearch != bufferInfos_.end()) {
    if (auto rankSearch = clusterSearch->second.find(rank);
        rankSearch != clusterSearch->second.end()) {
      auto infoList = rankSearch->second;
      for (auto info : infoList) {
        rankCount += info.count_;
      }
    }
  }
  if (rankCount == totalCount_) {//用计划中所有占用量的总和与总缓冲区大小totalCount_比较
    return true;
  }
  return false;
}

//检查特定rank的所有待处理的通信任务，是否都处于“已调度”的状态
//可用于同步点，进入下一个大规划之前，判断当前所有阶段已经提交的任务都被后台的Proxy Service接收并开始处理
bool flagcxInterRankBufferInfoManager::checkIsScheduled(int clusterId,
                                                        int rank) {
  if (auto clusterSearch = bufferInfos_.find(clusterId);
      clusterSearch != bufferInfos_.end()) {
    if (auto rankSearch = clusterSearch->second.find(rank);
        rankSearch != clusterSearch->second.end()) {
      auto infoList = rankSearch->second;
      for (auto info : infoList) {
        if (!info.isScheduled_) {//只要有一个isScheduled_标志为false，函数就返回false
          return false;
        }
      }
    }
  }
  return true;
}

//获取特定clusterId 和 rank对应的list表
std::list<flagcxBufferInfo> &
flagcxInterRankBufferInfoManager::getBufferInfoList(int clusterId, int rank) {
  if (auto clusterSearch = bufferInfos_.find(clusterId);
      clusterSearch != bufferInfos_.end()) {
    if (auto rankSearch = clusterSearch->second.find(rank);
        rankSearch != clusterSearch->second.end()) {
      return rankSearch->second;
    } else {//如果这两层有任意一层没有找到，就创建一个空的list并插入map中，返回这个新建list的引用
      clusterSearch->second[rank] = {};
      return clusterSearch->second[rank];
    }
  } else {
    bufferInfos_[clusterId][rank] = {};
    return bufferInfos_[clusterId][rank];
  }
}

//增
void flagcxInterRankBufferInfoManager::pushBackBufferInfo(
    int clusterId, int rank, size_t offset, size_t count, int clusterIdToSend,
    int isRecv, int isScheduled, int peerRank, int loopId) {
  bufferInfos_[clusterId][rank].emplace_back(
      offset, count, clusterIdToSend, isRecv, isScheduled, peerRank, loopId);
}

//删
void flagcxInterRankBufferInfoManager::popFrontBufferInfo(int clusterId,
                                                          int rank) {
  bufferInfos_[clusterId][rank].pop_front();
}

void flagcxInterRankBufferInfoManager::resetBufferInfo() {
  for (auto clusterIt = bufferInfos_.begin(); clusterIt != bufferInfos_.end();
       ++clusterIt) {
    for (auto rankIt = clusterIt->second.begin();
         rankIt != clusterIt->second.end(); ++rankIt) {
      rankIt->second.clear();
    }
  }
}

void flagcxInterRankBufferInfoManager::printBufferInfo(int step) {
  for (auto clusterIt = bufferInfos_.begin(); clusterIt != bufferInfos_.end();
       ++clusterIt) {
    for (auto rankIt = clusterIt->second.begin();
         rankIt != clusterIt->second.end(); ++rankIt) {
      for (auto bufferIt = rankIt->second.begin();
           bufferIt != rankIt->second.end(); ++bufferIt) {
        if (step == 0) {
          TRACE_CALL(
              "Initial InterRankBufferInfo: cluster_id = %d, rank = %d, "
              "offset = %lu, count = %lu, clusterIdToSend = %d, "
              "isRecv = %d, isScheduled = %d, peerRank = %d, loopId = %d",
              clusterIt->first, rankIt->first, bufferIt->offset_,
              bufferIt->count_, bufferIt->clusterIdToSend_, bufferIt->isRecv_,
              bufferIt->isScheduled_, bufferIt->peerRank_, bufferIt->loopId_);
        } else if (step == 1) {
          TRACE_CALL(
              "Internal InterRankBufferInfo: cluster_id = %d, rank = %d, "
              "offset = %lu, count = %lu, clusterIdToSend = %d, "
              "isRecv = %d, isScheduled = %d, peerRank = %d, loopId = %d",
              clusterIt->first, rankIt->first, bufferIt->offset_,
              bufferIt->count_, bufferIt->clusterIdToSend_, bufferIt->isRecv_,
              bufferIt->isScheduled_, bufferIt->peerRank_, bufferIt->loopId_);
        } else if (step == 2) {
          TRACE_CALL(
              "Final InterRankBufferInfo: cluster_id = %d, rank = %d, "
              "offset = %lu, count = %lu, clusterIdToSend = %d, "
              "isRecv = %d, isScheduled = %d, peerRank = %d, loopId = %d",
              clusterIt->first, rankIt->first, bufferIt->offset_,
              bufferIt->count_, bufferIt->clusterIdToSend_, bufferIt->isRecv_,
              bufferIt->isScheduled_, bufferIt->peerRank_, bufferIt->loopId_);
        }
      }
    }
  }
}

flagcxC2cP2pOp::flagcxC2cP2pOp(int rank, int peerRank, size_t offset,
                               size_t count, int isRecv)
    : rank_(rank), peerRank_(peerRank), offset_(offset), count_(count),
      isRecv_(isRecv) {}
flagcxC2cP2pOp::~flagcxC2cP2pOp() {}

flagcxResult_t flagcxC2cP2pOp::run(void *buff, flagcxDataType_t datatype,
                                   flagcxComm_t comm, flagcxStream_t stream) {
  TRACE_CALL("flagcxC2cP2pOp run: rank = %d, peerRank = %d, offset = %lu, "
             "count = %lu, "
             "isRecv = %d, datatype = %d",
             comm->rank, peerRank_, offset_, count_, isRecv_, datatype);
  void *ptr =
      static_cast<char *>(buff) + offset_ * getFlagcxDataTypeSize(datatype);
  if (isRecv_) {
    return flagcxHeteroRecv(static_cast<void *>(ptr), count_, datatype,
                            peerRank_, comm->hetero_comm, stream);
  } else {
    return flagcxHeteroSend(static_cast<void *>(ptr), count_, datatype,
                            peerRank_, comm->hetero_comm, stream);
  }
}

flagcxC2cHomoFunc::flagcxC2cHomoFunc(int rootRank, int sendType, int recvType,
                                     size_t sendOffset, size_t recvOffset,
                                     size_t count, int homoType,
                                     flagcxCommOp_t commOp)
    : rootRank_(rootRank), sendType_(sendType), recvType_(recvType),
      sendOffset_(sendOffset), recvOffset_(recvOffset), count_(count),
      homoType_(homoType), commOp_(commOp), interRankBufferInfoManager_(0) {}

flagcxC2cHomoFunc::flagcxC2cHomoFunc(
    int rootRank, int sendType, int recvType, size_t sendOffset,
    size_t recvOffset, size_t count, int homoType, flagcxCommOp_t commOp,
    flagcxInterRankBufferInfoManager interRankBufferInfoManager)
    : rootRank_(rootRank), sendType_(sendType), recvType_(recvType),
      sendOffset_(sendOffset), recvOffset_(recvOffset), count_(count),
      homoType_(homoType), commOp_(commOp),
      interRankBufferInfoManager_(interRankBufferInfoManager) {}

      //两个析构函数
flagcxC2cHomoFunc::flagcxC2cHomoFunc(FILE *file, size_t chunksize) {
  char line[LINE_LEN];

  while (fgets(line, sizeof(line), file)) {
    if (strstr(line, "</HomoFunc>"))
      break;
    if (strstr(line, "<rootRank>"))
      rootRank_ = readIntTag(line, "rootRank");
    if (strstr(line, "<sendType>"))
      sendType_ = readIntTag(line, "sendType");
    if (strstr(line, "<recvType>"))
      recvType_ = readIntTag(line, "recvType");
    if (strstr(line, "<sendOffset>"))
      sendOffset_ = readSizeTag(line, "sendOffset") * chunksize;
    if (strstr(line, "<recvOffset>"))
      recvOffset_ = readSizeTag(line, "recvOffset") * chunksize;
    if (strstr(line, "<count>"))
      count_ = readSizeTag(line, "count") * chunksize;
    if (strstr(line, "<homoType>"))
      homoType_ = readIntTag(line, "homoType");
    if (strstr(line, "<commOp>"))
      commOp_ = static_cast<flagcxCommOp_t>(readIntTag(line, "commOp"));
  }
}

flagcxC2cHomoFunc::~flagcxC2cHomoFunc() {}

//roxy Service的工人线程flagcxProxyProgress执行到HomoFunc步骤时，调用run方法
//接收执行时的动态上下文，如 sendbuff (总输入缓冲区), recvbuff (总输出缓冲区), 
// scratchbuff (临时缓冲区), comm (通信器句柄), stream (设备流) 等。
flagcxResult_t flagcxC2cHomoFunc::run(const void *sendbuff, void *recvbuff,
                                      void *scratchbuff,
                                      flagcxDataType_t datatype,
                                      flagcxRedOp_t redOp, int root,
                                      flagcxComm_t comm, flagcxStream_t stream,
                                      size_t *sendCounts, size_t *sDispls,
                                      size_t *recvCounts, size_t *rDispls) {
  if (homoType_ == 1 && comm->homoInterMyRank == -1) {//homoType_ == 1是集群间操作，comm->homoInterMyRank == -1检查
  // 当前进程是否为外交官，如果不是=-1，说明没有资格参与这次通信，函数直接返回，什么都不做
    return flagcxSuccess;
  }

  //用sendType_和recvType_标志使用哪个缓冲区，0=输入, 1=输出, 2=临时
  //sendType_如果是0=输入sendbuff, 1=输出recvbuff, 2=临时scratchbuff
  void *actualSendbuff = sendType_ == 0
                             ? const_cast<void *>(sendbuff)
                             : (sendType_ == 1 ? recvbuff : scratchbuff);
  //recvType_如果是1=输出recvbuff。否则是临时scratchbuff
  void *actualRecvbuff = recvType_ == 1 ? recvbuff : scratchbuff;

  TRACE_CALL("flagcxC2cHomoFunc run: rank = %d, rootRank = %d, sendType = %d, "
             "recvType = %d, sendOffset = %lu, "
             "recvOffset = %lu, count = %lu, "
             "homoType = %d, commOp = %d, datatype = %d, redOp = %d, root = %d",
             comm->rank, rootRank_, sendType_, recvType_, sendOffset_,
             recvOffset_, count_, homoType_, commOp_, datatype, redOp, root);

  switch (commOp_) {//根据不同的操作类型进入不同的分支，调用的终点
    case flagcxCommOpReduce:
      return cclAdaptors[flagcxCCLAdaptorDevice]->reduce(
          const_cast<const void *>(static_cast<void *>(
              static_cast<char *>(actualSendbuff) +
              sendOffset_ * getFlagcxDataTypeSize(datatype))),
          static_cast<void *>(static_cast<char *>(actualRecvbuff) +
                              recvOffset_ * getFlagcxDataTypeSize(datatype)),
          count_, datatype, redOp, (rootRank_ == -1) ? root : rootRank_,
          homoType_ == 1 ? comm->homoInterComm : comm->homo_comm, stream);//选择正确的通信器，集群间用homoInterComm，集群内用homo_comm
    case flagcxCommOpAllReduce:
      return cclAdaptors[flagcxCCLAdaptorDevice]->allReduce(
          const_cast<const void *>(static_cast<void *>(
              static_cast<char *>(actualSendbuff) +
              sendOffset_ * getFlagcxDataTypeSize(datatype))),
          static_cast<void *>(static_cast<char *>(actualRecvbuff) +
                              recvOffset_ * getFlagcxDataTypeSize(datatype)),
          count_, datatype, redOp,
          homoType_ == 1 ? comm->homoInterComm : comm->homo_comm, stream);
    case flagcxCommOpReduceScatter:
      return cclAdaptors[flagcxCCLAdaptorDevice]->reduceScatter(
          const_cast<const void *>(static_cast<void *>(
              static_cast<char *>(actualSendbuff) +
              sendOffset_ * getFlagcxDataTypeSize(datatype))),
          static_cast<void *>(static_cast<char *>(actualRecvbuff) +
                              recvOffset_ * getFlagcxDataTypeSize(datatype)),
          count_, datatype, redOp,
          homoType_ == 1 ? comm->homoInterComm : comm->homo_comm, stream);
    case flagcxCommOpAllGather:
      return cclAdaptors[flagcxCCLAdaptorDevice]->allGather(
          const_cast<const void *>(static_cast<void *>(
              static_cast<char *>(actualSendbuff) +
              sendOffset_ * getFlagcxDataTypeSize(datatype))),
          static_cast<void *>(static_cast<char *>(actualRecvbuff) +
                              recvOffset_ * getFlagcxDataTypeSize(datatype)),
          count_, datatype,
          homoType_ == 1 ? comm->homoInterComm : comm->homo_comm, stream);
    case flagcxCommOpGather:
      return cclAdaptors[flagcxCCLAdaptorDevice]->gather(
          const_cast<const void *>(static_cast<void *>(
              static_cast<char *>(actualSendbuff) +
              sendOffset_ * getFlagcxDataTypeSize(datatype))),
          static_cast<void *>(static_cast<char *>(actualRecvbuff) +
                              recvOffset_ * getFlagcxDataTypeSize(datatype)),
          count_, datatype, (rootRank_ == -1) ? root : rootRank_,
          homoType_ == 1 ? comm->homoInterComm : comm->homo_comm, stream);
    case flagcxCommOpScatter:
      return cclAdaptors[flagcxCCLAdaptorDevice]->scatter(
          const_cast<const void *>(static_cast<void *>(
              static_cast<char *>(actualSendbuff) +
              sendOffset_ * getFlagcxDataTypeSize(datatype))),
          static_cast<void *>(static_cast<char *>(actualRecvbuff) +
                              recvOffset_ * getFlagcxDataTypeSize(datatype)),
          count_, datatype, (rootRank_ == -1) ? root : rootRank_,
          homoType_ == 1 ? comm->homoInterComm : comm->homo_comm, stream);
    case flagcxCommOpSend:
      cclAdaptors[flagcxCCLAdaptorDevice]->groupStart();
      if (homoType_ == 0) {
        // send from root to inter-ranks
        if (comm->homo_rank == ((rootRank_ == -1) ? root : rootRank_)) {
          int clusterId = comm->cluster_ids[comm->rank];
          for (size_t i = 0; i < comm->clusterInterRankList[clusterId].size();
               ++i) {
            if (comm->homoInterMyRank != int(i)) {
              cclAdaptors[flagcxCCLAdaptorDevice]->send(
                  const_cast<const void *>(static_cast<void *>(
                      static_cast<char *>(actualSendbuff) +
                      sendOffset_ * getFlagcxDataTypeSize(datatype))),
                  count_, datatype,
                  comm->globalrank2homorank
                      [comm->clusterInterRankList[clusterId][i]],
                  comm->homo_comm, stream);
            }
          }
        }
      } else if (homoType_ == 1) {
        // send from inter-rank 1,2,...,n to inter-rank 0
        if (comm->homoInterMyRank > 0) {
          int clusterId = comm->cluster_ids[comm->rank];
          auto &buffList = interRankBufferInfoManager_.getBufferInfoList(
              clusterId, comm->rank);
          for (auto it = buffList.begin(); it != buffList.end(); it++) {
            if (it->isRecv_) {
              cclAdaptors[flagcxCCLAdaptorDevice]->send(
                  const_cast<const void *>(static_cast<void *>(
                      static_cast<char *>(actualSendbuff) +
                      it->offset_ * getFlagcxDataTypeSize(datatype))),
                  it->count_, datatype, 0, comm->homoInterComm, stream);
            }
          }
        }
      } else if (homoType_ == 2) {
        // send from inter-rank 0 to root
        if (comm->homoInterMyRank == 0 &&
            (comm->homo_rank != ((rootRank_ == -1) ? root : rootRank_))) {
          cclAdaptors[flagcxCCLAdaptorDevice]->send(
              const_cast<const void *>(static_cast<void *>(
                  static_cast<char *>(actualSendbuff) +
                  sendOffset_ * getFlagcxDataTypeSize(datatype))),
              count_, datatype, (rootRank_ == -1) ? root : rootRank_,
              comm->homo_comm, stream);
        }
      }
      cclAdaptors[flagcxCCLAdaptorDevice]->groupEnd();
      return flagcxSuccess;
    case flagcxCommOpRecv:
      cclAdaptors[flagcxCCLAdaptorDevice]->groupStart();
      if (homoType_ == 0) {
        // recv at inter-rank from root
        if (comm->homoInterMyRank != -1 &&
            comm->homo_rank != ((rootRank_ == -1) ? root : rootRank_)) {
          cclAdaptors[flagcxCCLAdaptorDevice]->recv(
              static_cast<void *>(
                  static_cast<char *>(const_cast<void *>(actualRecvbuff)) +
                  recvOffset_ * getFlagcxDataTypeSize(datatype)),
              count_, datatype, (rootRank_ == -1) ? root : rootRank_,
              comm->homo_comm, stream);
        }
      } else if (homoType_ == 1) {
        // recv at inter-rank 0 from inter-rank 1,2,...,n
        if (comm->homoInterMyRank == 0) {
          int clusterId = comm->cluster_ids[comm->rank];
          for (size_t i = 1; i < comm->clusterInterRankList[clusterId].size();
               ++i) {
            auto &buffList = interRankBufferInfoManager_.getBufferInfoList(
                clusterId, comm->clusterInterRankList[clusterId][i]);
            for (auto it = buffList.begin(); it != buffList.end(); it++) {
              if (it->isRecv_) {
                cclAdaptors[flagcxCCLAdaptorDevice]->recv(
                    static_cast<void *>(
                        static_cast<char *>(
                            const_cast<void *>(actualRecvbuff)) +
                        it->offset_ * getFlagcxDataTypeSize(datatype)),
                    it->count_, datatype, i, comm->homoInterComm, stream);
              }
            }
          }
        }
      } else if (homoType_ == 2) {
        // recv at root from inter-rank 0
        if (comm->homoInterMyRank != 0 &&
            comm->homo_rank == ((rootRank_ == -1) ? root : rootRank_)) {
          int clusterId = comm->cluster_ids[comm->rank];
          cclAdaptors[flagcxCCLAdaptorDevice]->recv(
              static_cast<void *>(
                  static_cast<char *>(const_cast<void *>(actualRecvbuff)) +
                  recvOffset_ * getFlagcxDataTypeSize(datatype)),
              count_, datatype,
              comm->globalrank2homorank[comm->clusterInterRankList[clusterId]
                                                                  [0]],
              comm->homo_comm, stream);
        }
      }
      cclAdaptors[flagcxCCLAdaptorDevice]->groupEnd();
      return flagcxSuccess;
    case flagcxCommOpBroadcast:
      return cclAdaptors[flagcxCCLAdaptorDevice]->broadcast(
          const_cast<const void *>(static_cast<void *>(
              static_cast<char *>(actualSendbuff) +
              sendOffset_ * getFlagcxDataTypeSize(datatype))),
          static_cast<void *>(static_cast<char *>(actualRecvbuff) +
                              recvOffset_ * getFlagcxDataTypeSize(datatype)),
          count_, datatype, (rootRank_ == -1) ? root : rootRank_,
          homoType_ == 1 ? comm->homoInterComm : comm->homo_comm, stream);
    case flagcxCommOpAlltoAll:
      return cclAdaptors[flagcxCCLAdaptorDevice]->alltoAll(
          const_cast<const void *>(static_cast<void *>(
              static_cast<char *>(actualSendbuff) +
              sendOffset_ * getFlagcxDataTypeSize(datatype))),
          static_cast<void *>(static_cast<char *>(actualRecvbuff) +
                              recvOffset_ * getFlagcxDataTypeSize(datatype)),
          count_, datatype,
          homoType_ == 1 ? comm->homoInterComm : comm->homo_comm, stream);
    case flagcxCommOpAlltoAllv:
      cclAdaptors[flagcxCCLAdaptorDevice]->groupStart();
      for (size_t i = 0; i < comm->nranks; ++i) {
        if (flagcxCCLAdaptorNeedSendrecv(sendCounts[i])) {
          if (comm->cluster_ids[comm->rank] == comm->cluster_ids[i]) {
            FLAGCXCHECK(cclAdaptors[flagcxCCLAdaptorDevice]->send(
                const_cast<const void *>(static_cast<void *>(
                    static_cast<char *>(actualSendbuff) +
                    sDispls[i] * getFlagcxDataTypeSize(datatype))),
                sendCounts[i], datatype, comm->globalrank2homorank[i],
                comm->homo_comm, stream));
          }
        }
        if (flagcxCCLAdaptorNeedSendrecv(recvCounts[i])) {
          if (comm->cluster_ids[comm->rank] == comm->cluster_ids[i]) {
            FLAGCXCHECK(cclAdaptors[flagcxCCLAdaptorDevice]->recv(
                static_cast<void *>(static_cast<char *>(actualRecvbuff) +
                                    rDispls[i] *
                                        getFlagcxDataTypeSize(datatype)),
                recvCounts[i], datatype, comm->globalrank2homorank[i],
                comm->homo_comm, stream));
          }
        }
      }
      cclAdaptors[flagcxCCLAdaptorDevice]->groupEnd();
      return flagcxSuccess;
    default:
      return flagcxSuccess;
  }
}

flagcxC2cHeteroFunc::flagcxC2cHeteroFunc(FILE *file, size_t chunksize) {
  char line[LINE_LEN];

  while (fgets(line, sizeof(line), file)) {
    if (strstr(line, "</HeteroFunc>"))
      break;
    if (strstr(line, "<P2pOp>")) {
      int rank;
      int peerRank;
      size_t offset;
      size_t count;
      int isRecv;
      char line[LINE_LEN];

      while (fgets(line, sizeof(line), file)) {
        if (strstr(line, "</P2pOp>"))
          break;
        if (strstr(line, "<rank>"))
          rank = readIntTag(line, "rank");
        if (strstr(line, "<peerRank>"))
          peerRank = readIntTag(line, "peerRank");
        if (strstr(line, "<offset>"))
          offset = readSizeTag(line, "offset") * chunksize;
        if (strstr(line, "<count>"))
          count = readSizeTag(line, "count") * chunksize;
        if (strstr(line, "<isRecv>"))
          isRecv = readIntTag(line, "isRecv");
      }
      addP2pOp(rank, peerRank, offset, count, isRecv);
    }
  }
}

flagcxC2cHeteroFunc::flagcxC2cHeteroFunc() {}
flagcxC2cHeteroFunc::~flagcxC2cHeteroFunc() {}

void flagcxC2cHeteroFunc::addP2pOp(int rank, int peerRank, size_t offset,
                                   size_t count, int isRecv) {
  p2pOps_.emplace_back(rank, peerRank, offset, count, isRecv);
}

flagcxResult_t flagcxC2cHeteroFunc::run(void *sendbuff, void *recvbuff,
                                        flagcxDataType_t datatype,
                                        flagcxComm_t comm,
                                        flagcxStream_t stream) {
  flagcxHeteroGroupStart();
  for (auto op : p2pOps_) {
    if (op.isRecv_) {
      FLAGCXCHECK(op.run(recvbuff, datatype, comm, stream));
    } else {
      FLAGCXCHECK(op.run(sendbuff, datatype, comm, stream));
    }
  }
  flagcxHeteroGroupEnd();
  return flagcxSuccess;
}

flagcxC2cRefreshFunc::flagcxC2cRefreshFunc()
    : bufftype_(-1), start_(0), offset_(0), count_(0), totalCount_(0),
      redOp_(flagcxSum) {}
flagcxC2cRefreshFunc::flagcxC2cRefreshFunc(size_t offset, size_t count,
                                           size_t totalCount,
                                           flagcxRedOp_t redOp)
    : bufftype_(-1), start_(0), offset_(offset), count_(count),
      totalCount_(totalCount), redOp_(redOp) {}
flagcxC2cRefreshFunc::flagcxC2cRefreshFunc(int bufftype, size_t start,
                                           size_t offset, size_t count,
                                           size_t totalCount,
                                           flagcxRedOp_t redOp)
    : bufftype_(bufftype), start_(start), offset_(offset), count_(count),
      totalCount_(totalCount), redOp_(redOp) {}
flagcxC2cRefreshFunc::~flagcxC2cRefreshFunc() {}

flagcxResult_t flagcxC2cRefreshFunc::run(void *recvbuff, void *scratchbuff,
                                         flagcxDataType_t datatype,
                                         flagcxStream_t stream) {
  void *refreshbuff = bufftype_ == 1 ? recvbuff : scratchbuff;
  refreshbuff = static_cast<void *>(static_cast<char *>(refreshbuff) +
                                    start_ * getFlagcxDataTypeSize(datatype));
  TRACE_CALL(
      "flagcxC2cRefreshFunc run: offset = %lu, count = %lu, totalCount = %lu, "
      "datatype = %d, redOp = %d",
      offset_, count_, totalCount_, datatype, redOp_);
  if (redOp_ == flagcxSum) {
    deviceAdaptor->deviceMemset(refreshbuff, 0,
                                offset_ * getFlagcxDataTypeSize(datatype),
                                flagcxMemDevice, stream);
    deviceAdaptor->deviceMemset(
        static_cast<void *>(static_cast<char *>(refreshbuff) +
                            (offset_ + count_) *
                                getFlagcxDataTypeSize(datatype)),
        0, (totalCount_ - offset_ - count_) * getFlagcxDataTypeSize(datatype),
        flagcxMemDevice, stream);
  }
  return flagcxSuccess;
}

flagcxC2cPlanner::flagcxC2cPlanner(size_t sendCount, size_t recvCount,
                                   int rootRank, flagcxComm_t comm,
                                   flagcxCommOp_t commOp, flagcxRedOp_t redOp)
    : sendCount_(sendCount), recvCount_(recvCount), rootRank_(rootRank),
      comm_(comm), commOp_(commOp), redOp_(redOp), sendCounts_(nullptr),
      sDispls_(nullptr), recvCounts_(nullptr), rDispls_(nullptr),
      clusterInterRankList_(comm->clusterInterRankList),
      clusterId_(comm->cluster_ids[comm->rank]), rank_(comm->rank),
      homoMyRank_(comm->homo_rank), homoRootRank_(comm->homo_root_rank),
      homoRanks_(comm->homo_ranks), homoInterMyRank_(comm->homoInterMyRank),
      homoInterRootRank_(comm->homoInterRootRank),
      homoInterRanks_(comm->homoInterRanks) {
  // set totalCount_
  totalCount_ = (sendCount_ >= recvCount_) ? sendCount_ : recvCount_;
  nchunks_ = totalCount_;

  // set rootClusterId_ and isRootCluster_
  rootClusterId_ = comm_->cluster_ids[rootRank_];
  isRootCluster_ = (rootClusterId_ == clusterId_) ? 1 : 0;

  // calculate clusterOffset_ and clusterCount_
  clusterOffset_ = 0;
  for (int i = 0; i < clusterId_; ++i) {
    clusterOffset_ += comm_->cluster_sizes[i];
  }
  clusterCount_ = comm_->cluster_sizes[clusterId_];

  // if inter ranks in all clusters equal to 1 （single-nic）
  if (commOp_ == flagcxCommOpAllReduce ||
      commOp_ == flagcxCommOpReduceScatter || commOp_ == flagcxCommOpReduce) {
    multiNic_ = 1;
    for (size_t i = 0; i < clusterInterRankList_.size(); ++i) {
      if (clusterInterRankList_[i].size() == 1) {
        multiNic_ = 0;
        break;
      }
    }
  } else {
    multiNic_ = 0;
    for (size_t i = 0; i < clusterInterRankList_.size(); ++i) {
      if (clusterInterRankList_[i].size() != 1) {
        multiNic_ = 1;
        break;
      }
    }
  }

  // if inter ranks in a cluster is superier to totalCount_ (single-nic)
  for (size_t i = 0; i < clusterInterRankList_.size(); ++i) {
    if (clusterInterRankList_[i].size() > totalCount_) {
      multiNic_ = 0;
      break;
    }
  }

  // if inter ranks in current cluster equal to homo ranks
  eachNicPerRank_ = 1;
  for (size_t i = 0; i < clusterInterRankList_.size(); ++i) {
    if (clusterInterRankList_[i].size() != comm->cluster_sizes[i]) {
      eachNicPerRank_ = 0;
      break;
    }
  }

  // initialize #steps and algorithm, sequential implementation by default
  algorithm_ = flagcxAlgoSequential;
  nSeqPreSteps_ = 1;
  nPipePreSteps_ = 0;
  nSeqInterSteps_ = 1;
  nPipePostSteps_ = 0;
  nSeqPostSteps_ = 1;
  // use ring pipeline algo if FLAGCX_C2C_ALGO=RING_PIPELINED
  const char *algorithm = getenv("FLAGCX_C2C_ALGO");
  if (algorithm != NULL && strcmp(algorithm, "RING_PIPELINED") == 0) {
    // pipeline optimizations for AllGather
    if (commOp_ == flagcxCommOpAllGather) {
      algorithm_ = flagcxAlgoPipeline;
      if (eachNicPerRank_) { // rank_local
        nSeqPreSteps_ = 0;
        nPipePreSteps_ = 1;
        nSeqInterSteps_ = 0;
        nPipePostSteps_ = comm_->nclusters - 2;
      } else if (multiNic_) { // general multi-nic
        nSeqPreSteps_ = 1;
        nPipePreSteps_ = 0;
        nSeqInterSteps_ = 1;
        nPipePostSteps_ = comm_->nclusters - 2;
      } else { // single nic
        nSeqPreSteps_ = 1;
        nPipePreSteps_ = 0;
        nSeqInterSteps_ = 0;
        nPipePostSteps_ = comm_->nclusters - 1;
      }
    } else if (commOp_ == flagcxCommOpReduceScatter && multiNic_) {
      algorithm_ = flagcxAlgoPipeline;
      nPipePreSteps_ = comm_->nclusters - 2;
      nSeqInterSteps_ = 1;
    } else if (commOp_ == flagcxCommOpAllReduce && multiNic_) {
      algorithm_ = flagcxAlgoPipeline;
      nPipePreSteps_ = comm_->nclusters - 2;
      nSeqInterSteps_ = 1;
      nPipePostSteps_ = comm_->nclusters - 1;
    }
  }
  // initialize an empty func queue for each step
  for (int i = 0; i < nSeqPreSteps_ + nPipePreSteps_; ++i) {
    preHomoFuncSteps_.emplace_back();
  }
  for (int i = 0; i < nSeqInterSteps_ + nPipePreSteps_ + nPipePostSteps_; ++i) {
    heteroFuncSteps_.emplace_back();
    homoInterFuncSteps_.emplace_back();
  }
  for (int i = 0; i < nPipePostSteps_ + nSeqPostSteps_; ++i) {
    postHomoFuncSteps_.emplace_back();
  }
  TRACE_CALL("flagcxC2cPlanner(nSeqPreSteps, nPipePreSteps, nSeqInterSteps, "
             "nPipePostSteps, nSeqPostSteps) = (%d, %d, %d, %d, %d)",
             nSeqPreSteps_, nPipePreSteps_, nSeqInterSteps_, nPipePostSteps_,
             nSeqPostSteps_);

  // set strategyFound_ to 0
  strategyFound_ = 0;

  // init inter-rank buffer info manager
  interRankBufferInfoManager_ = flagcxInterRankBufferInfoManager(totalCount_);
}

flagcxC2cPlanner::flagcxC2cPlanner(const char *path) {
  INFO(FLAGCX_ENV, "FLAGCX_ALGO_IMPORT_PATH set by environment to %s", path);
  importXml(path);
}

flagcxC2cPlanner::~flagcxC2cPlanner() {}

// homoType: 0, pre; 1, homoInter; 2, post,
// mode: 0, multiNic+eachNicPerRank; 1, normal; 2, single-nic
// For now, we support AllReduce, AllGather, ReduceScatter, Reduce, Broadcast,
// AlltoAll/v operator mapping
flagcxCommOp_t flagcxC2cPlanner::getC2cHomoCommOp(int homoType, int mode) {
  switch (commOp_) {
    case flagcxCommOpSend:
      return flagcxCommOpSend;
    case flagcxCommOpRecv:
      return flagcxCommOpRecv;
    case flagcxCommOpBroadcast:
      switch (homoType) {
        case 0:
          switch (isRootCluster_) {
            case 0:
              return flagcxCommNoOp;
            case 1:
              return flagcxCommOpBroadcast;
          }
        case 1:
          return flagcxCommNoOp;
        case 2:
          switch (isRootCluster_) {
            case 0:
              return flagcxCommOpBroadcast;
            case 1:
              return flagcxCommNoOp;
          }
      }
    case flagcxCommOpGather:
      switch (homoType) {
        case 0:
          switch (mode) {
            case 0:
              return flagcxCommOpAllGather;
            case 1:
              return flagcxCommOpAllGather;
            case 2:
              return flagcxCommOpGather;
          }
        case 1:
          switch (isRootCluster_) {
            case 0:
              return flagcxCommNoOp;
            case 1:
              switch (homoInterMyRank_ == 0) {
                case 0:
                  return flagcxCommOpSend;
                case 1:
                  return flagcxCommOpRecv;
              }
          }
        case 2:
          switch (isRootCluster_) {
            case 0:
              return flagcxCommNoOp;
            case 1:
              switch (rank_ == rootRank_) {
                case 0:
                  return flagcxCommOpSend;
                case 1:
                  return flagcxCommOpRecv;
              }
          }
      }
    case flagcxCommOpScatter:
      switch (homoType) {
        case 0:
          switch (isRootCluster_) {
            case 0:
              return flagcxCommNoOp;
            case 1:
              switch (rank_ == rootRank_) {
                case 0:
                  return flagcxCommOpRecv;
                case 1:
                  return flagcxCommOpSend;
              }
          }
        case 1:
          switch (isRootCluster_) {
            case 0:
              switch (homoInterMyRank_ == 0) {
                case 0:
                  return flagcxCommOpSend;
                case 1:
                  return flagcxCommOpRecv;
              }
            case 1:
              return flagcxCommNoOp;
          }
        case 2:
          return flagcxCommOpScatter;
      }
    case flagcxCommOpReduce:
      switch (homoType) {
        case 0:
          switch (mode) {
            case 0:
              return flagcxCommOpReduceScatter;
            case 1:
              return flagcxCommOpReduce;
            case 2:
              return flagcxCommOpReduce;
          }
        case 1:
          switch (isRootCluster_) {
            case 0:
              return flagcxCommNoOp;
            case 1:
              return flagcxCommOpReduce;
          }
        case 2:
          return flagcxCommNoOp;
      }
    case flagcxCommOpAllReduce:
      switch (homoType) {
        case 0:
          switch (mode) {
            case 0:
              return flagcxCommOpReduceScatter;
            case 1:
              return flagcxCommOpReduce;
            case 2:
              return flagcxCommOpReduce;
          }
        case 1:
          if (algorithm_ == flagcxAlgoSequential) {
            return flagcxCommOpAllReduce;
          }
          switch (mode) {
            case 0:
              return flagcxCommOpReduceScatter;
            case 1:
              return flagcxCommOpAllReduce;
            case 2:
              return flagcxCommOpAllReduce;
          }
        case 2:
          if (algorithm_ == flagcxAlgoPipeline) {
            return flagcxCommOpBroadcast;
          }
          switch (mode) {
            case 0:
              return flagcxCommNoOp;
            case 1:
              return flagcxCommOpAllReduce;
            case 2:
              return flagcxCommNoOp;
          }
      }
    case flagcxCommOpAllGather:
      switch (homoType) {
        case 0:
          switch (mode) {
            case 0:
              return flagcxCommOpAllGather;
            case 1:
              return flagcxCommOpAllGather;
            case 2:
              return flagcxCommOpGather;
          }
        case 1:
          return flagcxCommNoOp;
        case 2:
          return flagcxCommOpBroadcast;
      }
    case flagcxCommOpReduceScatter:
      switch (homoType) {
        case 0:
          switch (mode) {
            case 0:
              return flagcxCommOpReduceScatter;
            case 1:
              return flagcxCommOpReduce;
            case 2:
              return flagcxCommOpReduce;
          }
        case 1:
          switch (mode) {
            case 0:
              if (algorithm_ == flagcxAlgoPipeline) {
                return flagcxCommOpReduceScatter;
              } else {
                return flagcxCommOpAllReduce;
              }
            case 1:
              return flagcxCommOpAllReduce;
            case 2:
              return flagcxCommOpAllReduce;
          }
        case 2:
          switch (mode) {
            case 0:
              if (algorithm_ == flagcxAlgoPipeline) {
                return flagcxCommNoOp;
              } else {
                return flagcxCommOpReduceScatter;
              }
            case 1:
              return flagcxCommOpReduceScatter;
            case 2:
              return flagcxCommOpReduceScatter;
          }
      }
    case flagcxCommOpAlltoAll:
      switch (homoType) {
        case 0:
          return flagcxCommOpAlltoAll;
        case 1:
          return flagcxCommNoOp;
        case 2:
          return flagcxCommNoOp;
      }
    case flagcxCommOpAlltoAllv:
      switch (homoType) {
        case 0:
          return flagcxCommOpAlltoAllv;
        case 1:
          return flagcxCommNoOp;
        case 2:
          return flagcxCommNoOp;
      }
    default:
      return flagcxCommNoOp;
  }
}

flagcxResult_t flagcxC2cPlanner::importXml(const char *prefix) {
  algorithm_ = flagcxAlgoInput;
  char filename[128];
  sprintf(filename, "%s_%d.xml", prefix, rank_);
  TRACE_CALL("rank %d algo input set to %s", rank_, filename);
  FILE *file = fopen(filename, "r");
  if (!file)
    return flagcxInternalError;

  // primitive fields
  char line[LINE_LEN];
  while (fgets(line, sizeof(line), file)) {
    if (strstr(line, "<nChunks>"))
      break;
  }
  nchunks_ = readSizeTag(line, "nChunks");
  size_t chunksize = totalCount_ / nchunks_;

  while (fgets(line, sizeof(line), file)) {
    if (strstr(line, "<nSeqPreSteps>"))
      break;
  }
  nSeqPreSteps_ = readIntTag(line, "nSeqPreSteps");
  fgets(line, sizeof(line), file);
  nPipePreSteps_ = readIntTag(line, "nPipePreSteps");
  fgets(line, sizeof(line), file);
  nSeqInterSteps_ = readIntTag(line, "nSeqInterSteps");
  fgets(line, sizeof(line), file);
  nPipePostSteps_ = readIntTag(line, "nPipePostSteps");
  fgets(line, sizeof(line), file);
  nSeqPostSteps_ = readIntTag(line, "nSeqPostSteps");
  TRACE_CALL("flagcxC2cPlanner import from xml: (nSeqPreSteps, nPipePreSteps, "
             "nSeqInterSteps, "
             "nPipePostSteps, nSeqPostSteps) = (%d, %d, %d, %d, %d)",
             nSeqPreSteps_, nPipePreSteps_, nSeqInterSteps_, nPipePostSteps_,
             nSeqPostSteps_);

  // load refreshFunc
  int buffType;
  size_t startOffset;
  size_t offset;
  size_t count;
  size_t totalCount;
  flagcxRedOp_t redOp;
  while (fgets(line, sizeof(line), file)) {
    if (strstr(line, "</RefreshFunc>"))
      break;
    if (strstr(line, "<buffType>"))
      buffType = readIntTag(line, "buffType");
    if (strstr(line, "<start>"))
      startOffset = readSizeTag(line, "start");
    if (strstr(line, "<offset>"))
      offset = readSizeTag(line, "offset");
    if (strstr(line, "<count>"))
      count = readSizeTag(line, "count");
    if (strstr(line, "<totalCount>"))
      totalCount = readSizeTag(line, "totalCount");
    if (strstr(line, "<redOp>"))
      redOp = static_cast<flagcxRedOp_t>(readIntTag(line, "redOp"));
  }
  TRACE_CALL(
      "init refreshFunc with: offset = %lu, count = %lu, totalCount = %lu, "
      "redOp = %d",
      offset, count, totalCount, redOp);
  refreshFunc_ = flagcxC2cRefreshFunc(buffType, startOffset * chunksize,
                                      offset * chunksize, count * chunksize,
                                      totalCount * chunksize, redOp);

  // function sequences
  preHomoFuncSteps_ =
      readFunc2DVector<flagcxC2cHomoFunc>(file, chunksize, "PreHomoFuncSteps");
  heteroFuncSteps_ =
      readFunc2DVector<flagcxC2cHeteroFunc>(file, chunksize, "HeteroFuncSteps");
  homoInterFuncSteps_ = readFunc2DVector<flagcxC2cHomoFunc>(
      file, chunksize, "HomoInterFuncSteps");
  postHomoFuncSteps_ =
      readFunc2DVector<flagcxC2cHomoFunc>(file, chunksize, "PostHomoFuncSteps");

  fclose(file);
  return flagcxSuccess;
}

flagcxResult_t flagcxC2cPlanner::exportXml(const char *prefix) {
  char filename[128];
  sprintf(filename, "%s_%d.xml", prefix, rank_);
  FILE *file = fopen(filename, "w");
  if (!file)
    return flagcxInternalError;

  fprintf(file, "<FlagcxC2cPlanner>\n");
  fprintf(file, "  <nChunks>%ld</nChunks>\n", nchunks_);
  size_t chunksize = totalCount_ / nchunks_;

  // Serialize primitive members
  fprintf(file, "  <nSeqPreSteps>%d</nSeqPreSteps>\n", nSeqPreSteps_);
  fprintf(file, "  <nPipePreSteps>%d</nPipePreSteps>\n", nPipePreSteps_);
  fprintf(file, "  <nSeqInterSteps>%d</nSeqInterSteps>\n", nSeqInterSteps_);
  fprintf(file, "  <nPipePostSteps>%d</nPipePostSteps>\n", nPipePostSteps_);
  fprintf(file, "  <nSeqPostSteps>%d</nSeqPostSteps>\n", nSeqPostSteps_);

  // Serialize refreshFunc
  serializRefreshFunc(file, chunksize, refreshFunc_);

  // Serialize function steps
  serializeFunc2DVector(file, chunksize, preHomoFuncSteps_, "PreHomoFuncSteps");
  serializeFunc2DVector(file, chunksize, heteroFuncSteps_, "HeteroFuncSteps");
  serializeFunc2DVector(file, chunksize, homoInterFuncSteps_,
                        "HomoInterFuncSteps");
  serializeFunc2DVector(file, chunksize, postHomoFuncSteps_,
                        "PostHomoFuncSteps");

  fprintf(file, "</FlagcxC2cPlanner>\n");
  fclose(file);
  return flagcxSuccess;
}

flagcxResult_t flagcxC2cPlanner::refresh(int isSendRecv) {
  if (isSendRecv == 2) {
    for (size_t i = 0; i < clusterInterRankList_.size(); ++i) {
      for (size_t j = 0; j < clusterInterRankList_[i].size(); ++j) {
        auto &rankList = interRankBufferInfoManager_.getBufferInfoList(
            i, clusterInterRankList_[i][j]);
        for (auto it = rankList.begin(); it != rankList.end();) {
          int erased = 0;
          if (it->peerRank_ != -1) {
            it = rankList.erase(it);
            erased = 1;
          }
          if (!erased) {
            it++;
          }
        }
      }
    }
    interRankBufferInfoManager_.printBufferInfo(1);
  } else if (isSendRecv) {
    int clusterOffset = 0;
    int lcm = getLcmOfInterRankList(clusterInterRankList_);
    // we use fine-grained granularity to search for balanced hetero-send/recv
    // workloads
    std::string searchGranularity;
    const char *searchGranularityPtr =
        flagcxGetEnv("FLAGCX_C2C_SEARCH_GRANULARITY");
    if (searchGranularityPtr == NULL) {
      searchGranularity = std::string("COARSE");
    } else {
      searchGranularity = std::string(searchGranularityPtr);
      if (searchGranularity != "COARSE" && searchGranularity != "FINE") {
        searchGranularity = std::string("COARSE");
      }
    }
    interRankBufferInfoManager_.resetBufferInfo();
    for (size_t i = 0; i < clusterInterRankList_.size(); ++i) {
      size_t nClusterInterRanks =
          multiNic_ ? clusterInterRankList_[i].size() : 1;
      size_t myCount =
          (sendCount_ >= recvCount_)
              ? totalCount_ / nClusterInterRanks
              : (sendCount_ * comm_->cluster_sizes[i]) / nClusterInterRanks;
      size_t myRes =
          (sendCount_ >= recvCount_)
              ? totalCount_ % nClusterInterRanks
              : (sendCount_ * comm_->cluster_sizes[i]) % nClusterInterRanks;
      size_t minCount = (sendCount_ >= recvCount_)
                            ? totalCount_ / lcm
                            : (sendCount_ * comm_->cluster_sizes[i]) / lcm;
      if (searchGranularity == "COARSE") {
        minCount = myCount;
      }
      // for root-required ops, root cluster send or recv buffers based on
      // comm op type we use two flags to avoid redundant sendrecv ops
      int isScheduled = 0;
      int isUseless = 0;
      if (rootClusterId_ >= 0) {
        if ((commOp_ == flagcxCommOpReduce || commOp_ == flagcxCommOpGather) &&
            i == rootClusterId_) {
          isScheduled = 1;
        }
        if ((commOp_ == flagcxCommOpScatter ||
             commOp_ == flagcxCommOpBroadcast) &&
            i != rootClusterId_) {
          isUseless = 1;
        }
      }
      for (size_t j = 0; j < nClusterInterRanks; ++j) {
        size_t clusterdataoffset = 0;
        for (size_t z = 0; z < clusterInterRankList_.size(); ++z) {
          if ((commOp_ == flagcxCommOpReduceScatter ||
               commOp_ == flagcxCommOpAllReduce) &&
              algorithm_ == flagcxAlgoPipeline) {
            size_t rankCount = totalCount_ / comm_->nranks;
            myCount = rankCount * comm_->cluster_sizes[z] / nClusterInterRanks;
            myRes = rankCount * comm_->cluster_sizes[z] % nClusterInterRanks;
            minCount = rankCount * comm_->cluster_sizes[z] / lcm;
            for (int k = 0; k < myCount / minCount; ++k) {
              interRankBufferInfoManager_.pushBackBufferInfo(
                  i, clusterInterRankList_[i][j],
                  clusterdataoffset * rankCount + myCount * j + minCount * k,
                  minCount, z, 0, (isScheduled || i == z), -1, -1);
            }
            if (j == nClusterInterRanks - 1 && myRes > 0) {
              interRankBufferInfoManager_.pushBackBufferInfo(
                  i, clusterInterRankList_[i][j],
                  clusterdataoffset * rankCount + myCount * j, myRes, z, 0,
                  (isScheduled || i == z), -1, -1);
            }
            clusterdataoffset += comm_->cluster_sizes[z];
          } else if (i != z) {
            if (isUseless == 0) {
              for (int k = 0; k < myCount / minCount; ++k) {
                interRankBufferInfoManager_.pushBackBufferInfo(
                    i, clusterInterRankList_[i][j],
                    (sendCount_ >= recvCount_) ? myCount * j + minCount * k
                                               : clusterOffset * sendCount_ +
                                                     myCount * j + minCount * k,
                    minCount, z, 0, isScheduled, -1, -1);
              }
              if (j == nClusterInterRanks - 1 && myRes > 0) {
                interRankBufferInfoManager_.pushBackBufferInfo(
                    i, clusterInterRankList_[i][j],
                    (sendCount_ >= recvCount_)
                        ? myCount * (j + 1)
                        : clusterOffset * sendCount_ + myCount * (j + 1),
                    myRes, z, 0, isScheduled, -1, -1);
              }
            }
          }
        }
      }
      clusterOffset += comm_->cluster_sizes[i];
    }
    interRankBufferInfoManager_.printBufferInfo(0);
  } else {
    for (size_t i = 0; i < clusterInterRankList_.size(); ++i) {
      for (size_t j = 0; j < clusterInterRankList_[i].size(); ++j) {
        auto &rankList = interRankBufferInfoManager_.getBufferInfoList(
            i, clusterInterRankList_[i][j]);
        for (auto it = rankList.begin(); it != rankList.end();) {
          int erased = 0;
          if (it->isRecv_) {
            it = rankList.erase(it);
            erased = 1;
          }
          if (!erased) {
            it++;
          }
        }
      }
    }
    interRankBufferInfoManager_.printBufferInfo(1);
  }
  return flagcxSuccess;
}

//异构通信P2P调度算法，是负载均衡和调度逻辑的具体实现
//遍历所有需要进行的跨集群数据传输请求，通过可配置的搜索策略（轮询或顺序），为每一个请求从模板集群的代理细节中寻找有空的
//接收者，进行整体调度或拆分调度
flagcxResult_t flagcxC2cPlanner::searchHeteroSendRecvOps(int searchMethod,
                                                         int loopId) {
  // cluster j send to cluster z, cluster z recv from cluster j
  //三层循环是为了系统性地找出所有需要调度的发送任务
  for (size_t j = 0; j < clusterInterRankList_.size(); ++j) {//遍历所有源集群
    for (size_t z = j + 1; z < clusterInterRankList_.size(); ++z) {//遍历所有目标集群
      for (size_t r1 = 0; r1 < clusterInterRankList_[j].size(); ++r1) {//遍历源集群中的每一个代理
        auto &jList = interRankBufferInfoManager_.getBufferInfoList(
            j, clusterInterRankList_[j][r1]);
        for (auto it = jList.begin(); it != jList.end();) {
          int erased = 0;
          if (!it->isScheduled_ && !it->isRecv_ && it->clusterIdToSend_ == z) {
            for (size_t r2 = 0; r2 < clusterInterRankList_[z].size(); ++r2) {
              size_t newR2 = (searchMethod == 1)
                                 ? (r2 + r1) % clusterInterRankList_[z].size()
                                 : r2;
              if (interRankBufferInfoManager_.checkIfPossibleToPush(
                      z, clusterInterRankList_[z][newR2], it->offset_,
                      it->count_)) {
                interRankBufferInfoManager_.pushBackBufferInfo(
                    z, clusterInterRankList_[z][newR2], it->offset_, it->count_,
                    0, 1, 1, clusterInterRankList_[j][r1], loopId);
                it->isScheduled_ = 1;
                it->peerRank_ = clusterInterRankList_[z][newR2];
                it->loopId_ = loopId;
                break;
              }
            }
            if (!it->isScheduled_) {
              size_t splitCount = 0;
              size_t maxSplitCount = 0;
              int pushMode = 0;
              int finalPushMode = 0;
              int splitRank = clusterInterRankList_[z][0];
              for (size_t r2 = 0; r2 < clusterInterRankList_[z].size(); ++r2) {
                size_t newR2 = (r2 + r1) % clusterInterRankList_[z].size();
                if (interRankBufferInfoManager_.checkIfPossibleToSplitAndPush(
                        z, clusterInterRankList_[z][newR2], it->offset_,
                        it->count_, &splitCount, &pushMode)) {
                  if (maxSplitCount < splitCount) {
                    maxSplitCount = splitCount;
                    finalPushMode = pushMode;
                    splitRank = clusterInterRankList_[z][newR2];
                  }
                }
              }
              if (maxSplitCount > 0) {
                if (finalPushMode == 0) {
                  interRankBufferInfoManager_.pushBackBufferInfo(
                      z, splitRank, it->offset_, maxSplitCount, 0, 1, 1,
                      clusterInterRankList_[j][r1], loopId);
                  interRankBufferInfoManager_.pushBackBufferInfo(
                      j, clusterInterRankList_[j][r1], it->offset_,
                      maxSplitCount, it->clusterIdToSend_, 0, 1, splitRank,
                      loopId);
                  interRankBufferInfoManager_.pushBackBufferInfo(
                      j, clusterInterRankList_[j][r1],
                      it->offset_ + maxSplitCount, it->count_ - maxSplitCount,
                      it->clusterIdToSend_, 0, 0, -1, -1);
                } else if (finalPushMode == 1) {
                  interRankBufferInfoManager_.pushBackBufferInfo(
                      z, splitRank, it->offset_ + it->count_ - maxSplitCount,
                      maxSplitCount, 0, 1, 1, clusterInterRankList_[j][r1],
                      loopId);
                  interRankBufferInfoManager_.pushBackBufferInfo(
                      j, clusterInterRankList_[j][r1],
                      it->offset_ + it->count_ - maxSplitCount, maxSplitCount,
                      it->clusterIdToSend_, 0, 1, splitRank, loopId);
                  interRankBufferInfoManager_.pushBackBufferInfo(
                      j, clusterInterRankList_[j][r1], it->offset_,
                      it->count_ - maxSplitCount, it->clusterIdToSend_, 0, 0,
                      -1, -1);
                }
                it = jList.erase(it);
                erased = 1;
              }
            }
          }
          if (!erased) {
            it++;
          }
        }
      }
    }
  }
  // cluster z send to cluster j, cluster j recv from cluster z
  for (size_t j = 0; j < clusterInterRankList_.size(); ++j) {
    for (size_t z = j + 1; z < clusterInterRankList_.size(); ++z) {
      for (size_t r1 = 0; r1 < clusterInterRankList_[z].size(); ++r1) {
        auto &zList = interRankBufferInfoManager_.getBufferInfoList(
            z, clusterInterRankList_[z][r1]);
        for (auto it = zList.begin(); it != zList.end();) {
          int erased = 0;
          if (!it->isScheduled_ && !it->isRecv_ && it->clusterIdToSend_ == j) {
            for (size_t r2 = 0; r2 < clusterInterRankList_[j].size(); ++r2) {
              size_t newR2 = (searchMethod == 1)
                                 ? (r2 + r1) % clusterInterRankList_[j].size()
                                 : r2;
              if (interRankBufferInfoManager_.checkIfPossibleToPush(
                      j, clusterInterRankList_[j][newR2], it->offset_,
                      it->count_)) {
                interRankBufferInfoManager_.pushBackBufferInfo(
                    j, clusterInterRankList_[j][newR2], it->offset_, it->count_,
                    0, 1, 1, clusterInterRankList_[z][r1], loopId);
                it->isScheduled_ = 1;
                it->peerRank_ = clusterInterRankList_[j][newR2];
                it->loopId_ = loopId;
                break;
              }
            }
            if (!it->isScheduled_) {
              size_t splitCount = 0;
              size_t maxSplitCount = 0;
              int pushMode = 0;
              int finalPushMode = 0;
              int splitRank = clusterInterRankList_[j][0];
              for (size_t r2 = 0; r2 < clusterInterRankList_[j].size(); ++r2) {
                size_t newR2 = (r2 + r1) % clusterInterRankList_[j].size();
                if (interRankBufferInfoManager_.checkIfPossibleToSplitAndPush(
                        j, clusterInterRankList_[j][newR2], it->offset_,
                        it->count_, &splitCount, &pushMode)) {
                  if (maxSplitCount < splitCount) {
                    maxSplitCount = splitCount;
                    finalPushMode = pushMode;
                    splitRank = clusterInterRankList_[j][newR2];
                  }
                }
              }
              if (maxSplitCount > 0) {
                if (finalPushMode == 0) {
                  interRankBufferInfoManager_.pushBackBufferInfo(
                      j, splitRank, it->offset_, maxSplitCount, 0, 1, 1,
                      clusterInterRankList_[z][r1], loopId);
                  interRankBufferInfoManager_.pushBackBufferInfo(
                      z, clusterInterRankList_[z][r1], it->offset_,
                      maxSplitCount, it->clusterIdToSend_, 0, 1, splitRank,
                      loopId);
                  interRankBufferInfoManager_.pushBackBufferInfo(
                      z, clusterInterRankList_[z][r1],
                      it->offset_ + maxSplitCount, it->count_ - maxSplitCount,
                      it->clusterIdToSend_, 0, 0, -1, -1);
                } else if (finalPushMode == 1) {
                  interRankBufferInfoManager_.pushBackBufferInfo(
                      j, splitRank, it->offset_ + it->count_ - maxSplitCount,
                      maxSplitCount, 0, 1, 1, clusterInterRankList_[z][r1],
                      loopId);
                  interRankBufferInfoManager_.pushBackBufferInfo(
                      z, clusterInterRankList_[z][r1],
                      it->offset_ + it->count_ - maxSplitCount, maxSplitCount,
                      it->clusterIdToSend_, 0, 1, splitRank, loopId);
                  interRankBufferInfoManager_.pushBackBufferInfo(
                      z, clusterInterRankList_[z][r1], it->offset_,
                      it->count_ - maxSplitCount, it->clusterIdToSend_, 0, 0,
                      -1, -1);
                }
                it = zList.erase(it);
                erased = 1;
              }
            }
          }
          if (!erased) {
            it++;
          }
        }
      }
    }
  }
  return flagcxSuccess;
}

flagcxResult_t flagcxC2cPlanner::findStrategy() {
  if (algorithm_ == flagcxAlgoPipeline) {
    nchunks_ = comm_->nranks;
    for (int i = 0; i < comm_->nclusters; ++i) {
      nchunks_ = lcm(nchunks_, lcm(comm_->cluster_sizes[i],
                                   comm_->clusterInterRankList[i].size()));
    }
  }
  refresh(1);//重置规划器内部状态
  // setup refreshFunc
  //创建一个refreshFunc对象
  int bufftype = 1;
  int startoffset = 0;
  if (commOp_ == flagcxCommOpReduceScatter || commOp_ == flagcxCommOpScatter ||
      (commOp_ == flagcxCommOpGather && rank_ != rootRank_)) {
    bufftype = 2;
  }
  if ((commOp_ == flagcxCommOpReduceScatter ||
       commOp_ == flagcxCommOpAllReduce) &&
      algorithm_ == flagcxAlgoPipeline) {
    startoffset = clusterOffset_ * totalCount_ / comm_->nranks;
  }
  //初始化
  //根据已存在的流水线状态，智能创建一个flagcxC2cRefreshFunc对象，以便本次规划操作完成后，正确更新和清理缓冲区
  if (!interRankBufferInfoManager_.getBufferInfoList(clusterId_, rank_)
           .empty()) {//如果当前rank的缓冲区调度列表不为空，说明当前有正在进行的流水线操作，进入复杂逻辑计算
    auto &bufferList =
        interRankBufferInfoManager_.getBufferInfoList(clusterId_, rank_);
    size_t offset = 0;
    size_t count = 0;
    size_t totalCount = 0;
    int counter = 0;
    for (auto it = bufferList.begin(); it != bufferList.end(); ++it) {//遍历调度列表，找到未被调度的ReduceScatter
    // 或AllReduce操作
      if ((commOp_ == flagcxCommOpReduceScatter ||
           commOp_ == flagcxCommOpAllReduce) &&
          !(it->isScheduled_) && algorithm_ == flagcxAlgoPipeline) {
        continue;
      }
      if (algorithm_ == flagcxAlgoPipeline) {//复杂偏移计算
        offset = it->offset_;
        count = it->count_;
        totalCount = totalCount_;
        if (commOp_ == flagcxCommOpReduceScatter) {
          offset -= clusterOffset_ * recvCount_;//全局的缓冲区偏移量需要被转换成相对于当前同构子集群的本地偏移量
          totalCount = comm_->cluster_sizes[clusterId_] * recvCount_;
        } else if (commOp_ == flagcxCommOpAllReduce) {
          offset -= clusterOffset_ * totalCount_ / comm_->nranks;
          totalCount =
              comm_->cluster_sizes[clusterId_] * totalCount_ / comm_->nranks;
        }
        break;
      }
      if (counter == 0) {
        offset = it->offset_;
        totalCount = totalCount_;
      }
      count += it->count_;
      counter++;
    }
    //用计算出的本地偏移量和数据量，创建flagcxC2cRefreshFunc对象
    refreshFunc_ = flagcxC2cRefreshFunc(bufftype, startoffset, offset, count,
                                        totalCount, redOp_);
  } else {//如果当前rank的缓冲区调度列表为空，说明是全新的操作，没有任何流水线状态
    //直接用0和totalCount_创建一个默认的refreshFunc_
    refreshFunc_ = flagcxC2cRefreshFunc(0, 0, totalCount_, redOp_);
  }

  //根据通信类型，重置优化标志——Planner根据不同的通信操作，动态调整优化策略
  // reset multiNic_ based on comm op type
  // since broadcast, alltoall, alltoallv, scatter, gather ops behave
  // identically in both single-nic and multi-nic modes, no special handling is
  // required
  if (commOp_ == flagcxCommOpBroadcast || commOp_ == flagcxCommOpAlltoAll ||
      commOp_ == flagcxCommOpAlltoAllv || commOp_ == flagcxCommOpScatter ||
      commOp_ == flagcxCommOpGather) {
    multiNic_ = 1;//重置是否启用多网卡负载均衡优化标志
    //强制设置成1，单网卡模式，假装只有一张网卡，多网卡优化对它们不适用，与其设计一套复杂的多网卡算法，直接走简单的单网卡
  }

  // reset eachNicPerRank_ based on comm op type
  // since alltoall, alltoallv ops behave identically in both
  // normal and rank-local multi-nic modes
  if (commOp_ == flagcxCommOpAlltoAll || commOp_ == flagcxCommOpAlltoAllv) {
    eachNicPerRank_ = 1;//控制每个rank使用独立的网卡优化模式，精细的rank-NIC绑定很复杂，为了简化强制设置1普通模式
  }

  //根据commOp_，为后续创建flagcxC2cHomoFunc等步骤预先确定应该使用哪种类型的发送和接收缓冲区
  int recvType = 1;//接收缓冲区默认是1
  // if scratch buffer is needed
  if (commOp_ == flagcxCommOpReduceScatter || commOp_ == flagcxCommOpScatter ||
      (commOp_ == flagcxCommOpGather && rank_ != rootRank_)) {
    recvType = 2;//对于ReduceScatter、Scatter等操作，每个rank只接收最终结果的一部分，如果直接写入最终的recvbuff很混
    // 乱，所以先把接收到的数据写入临时的scratchbuff
  }
  int sendType =
      (commOp_ == flagcxCommOpAlltoAll || commOp_ == flagcxCommOpAlltoAllv ||
       (commOp_ == flagcxCommOpScatter && rank_ == rootRank_) ||
       (commOp_ == flagcxCommOpAllGather && eachNicPerRank_ && multiNic_))
          ? 0
          : recvType;//对于AlltoAll, Scatter等操作，数据从原始的输入缓冲区sendbuff发出，所以sendType设为0；对于其它
          // 大部分操作，发送的数据是上一阶段的计算结果，已经存在了，所以直接沿用recvType的值

  //findStrategy的核心实现部分，负责preHomoFuncSteps构建同构预处理步骤的执行计划
  //根据多网卡multiNic_和rank本地网卡eachNicPerRank_的配置，还有上层通信操作commOp_的类型，分层的决定在集群内应该执行哪种
  // 原生通信库，并为该操作精确地计算出所有参数，创建flagcxC2cHomoFunc对象并存入步骤列表中
  if (multiNic_) {
    // multi-nic
    // setup preHomoFuncs
    //给多网卡环境设置了两种规划
    if (eachNicPerRank_) {//if的情况：每个rank都使用独立的网卡
      //简化的多网卡模型，假设每个rank都独立地参与跨集群通信
      // inter ranks equaling to homo ranks
      // setup preHomoFuncs
      flagcxCommOp_t preHomoFuncCommOp = getC2cHomoCommOp(0, 0);//调用辅助函数getC2cHomoCommOp，pre-homo阶段
      // 应该执行的最佳原生集体通信操作，0, 0 可能是代表 (stage=pre, mode=each_rank_is_proxy) 的标志
      auto &buffer =
          interRankBufferInfoManager_.getBufferInfoList(clusterId_, rank_)
              .front();
      if (preHomoFuncCommOp == flagcxCommOpReduceScatter) {//如果getC2cHomoCommOp返回的最佳操作是ReduceScatter，
      // 是AllReduce的第一步，进入一个复杂的参数计算逻辑
        if (commOp_ == flagcxCommOpReduce ||
            algorithm_ == flagcxAlgoSequential) {
          preHomoFuncSteps_[0].emplace_back(
              -1, 0, recvType, 0, homoMyRank_ * buffer.count_, buffer.count_, 0,
              preHomoFuncCommOp);
        } else {
          size_t dataoffset = 0;
          size_t rankCount = totalCount_ / comm_->nranks;
          for (int c = 0; c < comm_->nclusters; ++c) {//循环为了流水线Pipelining，为未来和其它集群c的通信，提前规
          // 划好本次pre-homo操作所需的数据
            size_t clusterdata = rankCount * comm_->cluster_sizes[c];
            size_t preHomoFuncCount =
                clusterdata / clusterInterRankList_[clusterId_].size();//计算要发送给c的那部分数据，当前集群内需
                // 要处理的数据块大小
            size_t preHomoFuncRes =
                clusterdata % clusterInterRankList_[clusterId_].size();
                //step的计算是环形流水线的核心，确保了环上不同集群同一时间步骤处理不同的数据块
            int step =
                (clusterId_ + comm_->nclusters - 1 - c) % comm_->nclusters;
            if (step == comm_->nclusters - 1) {
              step = 0;
            }
            //最终的执行指令，创建一个flagcxC2cHomoFunc对象，并放入对应步骤step列表中
            preHomoFuncSteps_[step].emplace_back(
                -1, 0, recvType, dataoffset,
                dataoffset + preHomoFuncCount * homoMyRank_, preHomoFuncCount,
                0, preHomoFuncCommOp);
            if (preHomoFuncRes > 0) {
              preHomoFuncSteps_[step].emplace_back(
                  comm_->globalrank2homorank[clusterInterRankList_[clusterId_]
                                                 .back()],
                  0, recvType, dataoffset + clusterdata - preHomoFuncRes,
                  dataoffset + clusterdata - preHomoFuncRes, preHomoFuncRes, 0,
                  flagcxCommOpReduce);
            }
            dataoffset += clusterdata;
          }
        }
      } else if (preHomoFuncCommOp == flagcxCommOpAllGather) {//处理Reduce和AllGather等其它情况，为其它commOp_
        //类型都提供了对应的更简单的pre-homo步骤生成逻辑，简单创建一个flagcxC2cHomoFunc对象并放入preHomoFuncSteps_[0]
        preHomoFuncSteps_[0].emplace_back(-1, 0, recvType, 0,
                                          clusterOffset_ * sendCount_,
                                          sendCount_, 0, preHomoFuncCommOp);
      } else if (preHomoFuncCommOp == flagcxCommOpBroadcast) {
        preHomoFuncSteps_[0].emplace_back(-1, 0, recvType, 0, 0, totalCount_, 0,
                                          preHomoFuncCommOp);
      } else if (preHomoFuncCommOp == flagcxCommOpSend) {
        preHomoFuncSteps_[0].emplace_back(comm_->globalrank2homorank[rootRank_],
                                          0, recvType, 0, 0, totalCount_, 0,
                                          preHomoFuncCommOp);
      } else if (preHomoFuncCommOp == flagcxCommOpRecv) {
        preHomoFuncSteps_[0].emplace_back(comm_->globalrank2homorank[rootRank_],
                                          0, recvType, 0, 0, totalCount_, 0,
                                          preHomoFuncCommOp);
      } else if (preHomoFuncCommOp == flagcxCommOpAlltoAll) {
        preHomoFuncSteps_[0].emplace_back(
            -1, 0, recvType, clusterOffset_ * sendCount_,
            clusterOffset_ * recvCount_, totalCount_,
            0, // sendCount_ = recvCount_ = totalCount_
            preHomoFuncCommOp);
      } else if (preHomoFuncCommOp == flagcxCommOpAlltoAllv) {
        preHomoFuncSteps_[0].emplace_back(-1, 0, recvType, 0, 0, 0, 0,
                                          preHomoFuncCommOp);
      } else if (preHomoFuncCommOp == flagcxCommNoOp) {
        preHomoFuncSteps_[0].emplace_back(-1, 0, recvType, 0, 0, totalCount_, 0,
                                          preHomoFuncCommOp);
      }
    } else {//else的情况：多个rank共享网卡，需要代理外交官，只有被选中的rank才能跨集群通信（标准的、复杂的多网卡模型）
      // otherwise
      //为每一个外交官都生成一个Reduce任务，让集群内一部分成员，把它们的局部计算结果规约到这个外交官身上
      flagcxCommOp_t preHomoFuncCommOp = getC2cHomoCommOp(0, 1);//调用getC2cHomoCommOp，这次参数（0，1）代表
      //(stage=pre, mode=proxy_based)
      if (preHomoFuncCommOp == flagcxCommOpReduce) {//这种模式下，pre-homo阶段的最佳操作通常是Reduce
        for (int i = 0; i < clusterInterRankList_[clusterId_].size(); ++i) {//遍历本集群所有外交官
          for (auto &buffer : interRankBufferInfoManager_.getBufferInfoList(//遍历每个外交官需要处理的缓冲区信息
                   clusterId_, clusterInterRankList_[clusterId_][i])) {
            int step = 0;
            if (commOp_ != flagcxCommOpReduce &&
                algorithm_ == flagcxAlgoPipeline) {
              step = (clusterId_ + comm_->nclusters - 1 -
                      comm_->cluster_ids[buffer.clusterIdToSend_]) %
                     comm_->nclusters;
              if (step == comm_->nclusters - 1) {
                step = 0;
              }
            }
            //创建Reduce指令，指令的目标root是clusterInterRankList_[clusterId_][i]，也就是其中一个外交官
            preHomoFuncSteps_[step].emplace_back(
                clusterInterRankList_[clusterId_][i] - (rank_ - homoMyRank_), 0,
                recvType, buffer.offset_, buffer.offset_, buffer.count_, 0,
                preHomoFuncCommOp);
          }
        }//类型都提供了对应的更简单的pre-homo步骤生成逻辑，简单创建一个flagcxC2cHomoFunc对象并放入preHomoFuncSteps_[0]
      } else if (preHomoFuncCommOp == flagcxCommOpAllGather) {
        preHomoFuncSteps_[0].emplace_back(-1, 0, recvType, 0,
                                          clusterOffset_ * sendCount_,
                                          sendCount_, 0, preHomoFuncCommOp);
      } else if (preHomoFuncCommOp == flagcxCommOpBroadcast) {
        preHomoFuncSteps_[0].emplace_back(-1, 0, recvType, 0, 0, totalCount_, 0,
                                          preHomoFuncCommOp);
      } else if (preHomoFuncCommOp == flagcxCommOpSend) {
        preHomoFuncSteps_[0].emplace_back(comm_->globalrank2homorank[rootRank_],
                                          0, recvType, 0, 0, totalCount_, 0,
                                          preHomoFuncCommOp);
      } else if (preHomoFuncCommOp == flagcxCommOpRecv) {
        preHomoFuncSteps_[0].emplace_back(comm_->globalrank2homorank[rootRank_],
                                          0, recvType, 0, 0, totalCount_, 0,
                                          preHomoFuncCommOp);
      } else if (preHomoFuncCommOp == flagcxCommNoOp) {
        preHomoFuncSteps_[0].emplace_back(-1, 0, recvType, 0, 0, totalCount_, 0,
                                          preHomoFuncCommOp);
      }
    }

    //---确定和构建跨集群通信heteroFuncSteps_和代理间通信homoInterFuncSteps_的具体执行步骤
    //根据不同的通信操作类型commOp_，决定是用简单的点对点通信模式，还是用复杂的基于缓冲区管理器和搜索算法优化的流水线调度模式
    // determine hetero send/recv strategies
    // and setup homoInterFuncs

    //AlltoAll绕过复杂的代理和流水线机制，直接让每一个rank和其它集群的rank建立P2P连接交换数据，虽然不够高效，但是逻辑简单
    if (commOp_ == flagcxCommOpAlltoAll) {//针对AlltoAll全交换通信模式的简化策略
      flagcxC2cHeteroFunc heteroFunc = flagcxC2cHeteroFunc();
      for (size_t i = 0; i < comm_->nranks; ++i) {//循环遍历所有ranks
        if (clusterId_ != comm_->cluster_ids[i]) {//判断目标rank i是否与当前进程不属于同一个集群
          //如果不属于一个集群，直接创建点对点的send和recv操作
          heteroFunc.addP2pOp(rank_, i, i * sendCount_, sendCount_, 0);
          heteroFunc.addP2pOp(rank_, i, i * recvCount_, recvCount_, 1);
        }
      }
      heteroFuncSteps_[0].push_back(std::move(heteroFunc));
      homoInterFuncSteps_[0].emplace_back(-1, sendType, recvType, 0, 0,
                                          totalCount_, 1, flagcxCommNoOp);//简化，不需要复杂的代理间协调，代理什么都不用做
    } else if (commOp_ == flagcxCommOpAlltoAllv) {
      flagcxC2cHeteroFunc heteroFunc = flagcxC2cHeteroFunc();
      for (size_t i = 0; i < comm_->nranks; ++i) {
        if (flagcxCCLAdaptorNeedSendrecv(sendCounts_[i]) &&
            clusterId_ != comm_->cluster_ids[i]) {
          heteroFunc.addP2pOp(rank_, i, sDispls_[i], sendCounts_[i], 0);
        }
        if (flagcxCCLAdaptorNeedSendrecv(recvCounts_[i]) &&
            clusterId_ != comm_->cluster_ids[i]) {
          heteroFunc.addP2pOp(rank_, i, rDispls_[i], recvCounts_[i], 1);
        }
      }
      heteroFuncSteps_[0].push_back(std::move(heteroFunc));
      homoInterFuncSteps_[0].emplace_back(-1, sendType, recvType, 0, 0,
                                          totalCount_, 1, flagcxCommNoOp);
    } else {//针对AllReduce,ReduceScatter,AllGather等可以被高度优化的集体通信操作的的核心智能调度逻辑
      int heteroAndHomoInterFuncLoops = 1;
      for (int i = 0; i < heteroAndHomoInterFuncLoops; ++i) {
        // search by BFS or DFS
        searchHeteroSendRecvOps(1, i);//路径搜索算法，和flagcxInterRankBufferInfoManager紧密协作，可能基于BFS或
        //DFS在当前的拓扑和缓冲区状态下，搜索出一条或多条最优的无冲突的P2P路径

        int scheduleCompleted = 1;
        for (size_t j = 0; j < clusterInterRankList_.size(); ++j) {
          for (size_t z = 0; z < clusterInterRankList_[j].size(); ++z) {
            //checkIsScheduled是检查点，确保searchHeteroSendRecvOps已为所有代理节点的所有缓冲区都制定好了传输计划
            if (!interRankBufferInfoManager_.checkIsScheduled(
                    j, clusterInterRankList_[j][z])) {
              scheduleCompleted = 0;
              break;
            }
          }
          if (!scheduleCompleted) {
            break;
          }
        }

        // setup heteroFuncs
        //在searchHeteroSendRecvOps执行完之后，时刻表interRankBufferInfoManager_中已有所有被批准的P2P传输
        std::vector<flagcxC2cHeteroFunc> heteroFuncStep;
        for (size_t s = 0;
             s < nSeqInterSteps_ + nPipePreSteps_ + nPipePostSteps_; ++s) {
          heteroFuncStep.emplace_back();
        }
        //遍历时刻表，对每一个被标记为isScheduled_的条目，创建对应的flagcxC2cP2pOp对象，添加到heteroFuncStep列表中
        for (size_t j = 0; j < clusterInterRankList_.size(); ++j) {
          for (size_t z = 0; z < clusterInterRankList_[j].size(); ++z) {
            if (rank_ == clusterInterRankList_[j][z]) {
              auto &rankList =
                  interRankBufferInfoManager_.getBufferInfoList(j, rank_);
              for (auto it = rankList.begin(); it != rankList.end(); ++it) {//遍历时刻表
                if (it->isScheduled_ && it->loopId_ == i) {//对每一个被标记为isScheduled_的条目
                  size_t offset =
                      ((!it->isRecv_ && commOp_ == flagcxCommOpAllGather &&
                        eachNicPerRank_)
                           ? 0
                           : it->offset_);//计算偏移
                  if (nPipePreSteps_ + nSeqInterSteps_ + nPipePostSteps_ > 1) {
                    if (it->peerRank_ == -1) {
                      continue;
                    }
                    int sendClusterId = it->isRecv_
                                            ? comm_->cluster_ids[it->peerRank_]
                                            : clusterId_;
                    int recvClusterId = it->isRecv_
                                            ? clusterId_
                                            : comm_->cluster_ids[it->peerRank_];
                    size_t step =
                        (sendClusterId + comm_->nclusters - 1 - recvClusterId) %
                        comm_->nclusters;//流水线步骤step计算，体现了环形流水线的设计，根据P2P的发送方集群ID和接收方
                        // 集群ID，计算出这个P2P操作应该属于流水线的哪一个时间步骤step
                    //创建对应的flagcxC2cP2pOp对象
                    heteroFuncStep[step].addP2pOp(rank_, it->peerRank_, offset,
                                                  it->count_, it->isRecv_);
                  } else {
                    heteroFuncStep[0].addP2pOp(rank_, it->peerRank_, offset,
                                               it->count_, it->isRecv_);
                  }
                }
              }
            }
          }
        }
        //AllReduce特殊处理，分解为Reduce-Scatter+All-Gather两个阶段，因此需要更特殊的、两阶段的处理
        if (commOp_ == flagcxCommOpAllReduce &&
            algorithm_ == flagcxAlgoPipeline) {
          refresh(2);//重置缓冲区，准备进入第二阶段
          size_t clusterOffset = 0;
          for (size_t i = 0; i < clusterInterRankList_.size(); ++i) {
            for (size_t j = 0; j < clusterInterRankList_[i].size(); ++j) {
              auto &rankList = interRankBufferInfoManager_.getBufferInfoList(
                  i, clusterInterRankList_[i][j]);
              for (auto it = rankList.begin(); it != rankList.end();) {
                if (it->isScheduled_ && it->peerRank_ == -1) {
                  // broadcast local cluster data at post step 0
                  if (i == clusterId_) {
                    postHomoFuncSteps_[0].emplace_back(//第一阶段Reduce-Scatter结束后，在本集群内部，把外交官收到
                      //的规约好的数据块，广播给集群内所有成员，这里创建一个Broadcast同构操作
                        clusterInterRankList_[i][j] - (rank_ - homoMyRank_), 1,
                        1, it->offset_, it->offset_, it->count_, 2,
                        flagcxCommOpBroadcast);
                  }
                  // refresh buffer info for the allgather phase
                  for (int c = 0; c < comm_->nclusters; ++c) {
                    if (c == i) {
                      continue;
                    }
                    //为All-Gather第二阶段重建一批新的、待调度的缓冲区传输请求
                    interRankBufferInfoManager_.pushBackBufferInfo(
                        i, j + clusterOffset, it->offset_, it->count_, c, 0, 0,
                        -1, -1);
                  }
                  it = rankList.erase(it);
                } else if (it->isScheduled_) {
                  it = rankList.erase(it);
                } else {
                  ++it;
                }
              }
            }
            clusterOffset += comm_->cluster_sizes[i];
          }
          interRankBufferInfoManager_.printBufferInfo(0);
          searchHeteroSendRecvOps(1, 0);//再次调用调度器，为二阶段All-Gather搜索最优的P2P路径
          for (size_t j = 0; j < clusterInterRankList_.size(); ++j) {
            for (size_t z = 0; z < clusterInterRankList_[j].size(); ++z) {
              if (rank_ == clusterInterRankList_[j][z]) {
                auto &rankList =
                    interRankBufferInfoManager_.getBufferInfoList(j, rank_);
                for (auto it = rankList.begin(); it != rankList.end(); ++it) {
                  if (it->isScheduled_) {
                    int sendClusterId = it->isRecv_
                                            ? comm_->cluster_ids[it->peerRank_]
                                            : clusterId_;
                    int recvClusterId = it->isRecv_
                                            ? clusterId_
                                            : comm_->cluster_ids[it->peerRank_];
                    if (sendClusterId == recvClusterId) {
                      continue;
                    }
                    size_t step =
                        (sendClusterId + comm_->nclusters - 1 - recvClusterId) %
                            comm_->nclusters +
                        comm_->nclusters - 1;
                    heteroFuncStep[step].addP2pOp(rank_, it->peerRank_,
                                                  it->offset_, it->count_,
                                                  it->isRecv_);//再次生成heteroFuncSteps，把第二阶段调度
                                            // 好的P2P操作也添加到heteroFuncStep列表中，但放入的是后续的时间步骤
                  }
                }
              }
            }
          }
        }

        //异构通信策略生成的后半部分，构建homoInterFuncSteps_代理间步骤和postHomoFuncSteps_同构后处理步骤，包括对
        // 单网卡场景的独立处理逻辑
        for (size_t s = 0;
             s < nPipePreSteps_ + nSeqInterSteps_ + nPipePostSteps_; ++s) {
          heteroFuncSteps_[s].push_back(std::move(heteroFuncStep[s]));
        }

        // setup homoInterFuncs
        //构建homoInterFuncSteps_代理间通信
        //定义代理节点之间需要执行的集体通信操作
        flagcxCommOp_t homoInterFuncCommOp =
            eachNicPerRank_ ? getC2cHomoCommOp(1, 0) : getC2cHomoCommOp(1, 1);//调用辅助函数getC2cHomoCommOp
            //获取在inter阶段（阶段1），根据不同的多网卡模式，应该执行哪种通信操作
        if (homoInterFuncCommOp == flagcxCommOpAllReduce) {//如果是AllReduce操作，创建一个flagcxC2cHomoFunc对象
          //AllReduce只在代理节点组成的小通信组执行
          homoInterFuncSteps_[0].emplace_back(-1, sendType, recvType, 0, 0,
                                              totalCount_, 1,
                                              homoInterFuncCommOp);
        } else if (homoInterFuncCommOp == flagcxCommOpReduce) {
          homoInterFuncSteps_[0].emplace_back(-1, sendType, recvType, 0, 0,
                                              totalCount_,
                                              0, // use homo_comm
                                              homoInterFuncCommOp);
        } else if (homoInterFuncCommOp == flagcxCommOpReduceScatter) {//ReduceScatter操作进入复杂流水线for循环
          for (int c = 0; c < comm_->nclusters; ++c) {
            int step = algorithm_ == flagcxAlgoSequential
                           ? 0
                           : (clusterId_ + comm_->nclusters - 1 - c) %
                                 comm_->nclusters;//环形流水线step计算，说明代理间通信也分成多个步骤，实现流水线并行
            if (step == comm_->nclusters - 1) {
              continue;
            }
            size_t rankCount = totalCount_ / comm_->nranks;
            size_t recvoffset =
                clusterOffset_ * rankCount + homoMyRank_ * rankCount;
            int recvFlag = algorithm_ == flagcxAlgoPipeline &&
                           commOp_ == flagcxCommOpReduceScatter &&
                           eachNicPerRank_ && step == comm_->nclusters - 2;
            //为每个流水线步骤，都创建一个ReduceScatter的HomoFunc指令
            homoInterFuncSteps_[step].emplace_back(
                -1, sendType, recvFlag ? 1 : recvType,
                clusterOffset_ * rankCount, recvFlag ? 0 : recvoffset,
                rankCount, 2, homoInterFuncCommOp);
          }
        } else if (homoInterFuncCommOp == flagcxCommOpSend) {
          homoInterFuncSteps_[0].emplace_back(
              -1, sendType, recvType, 0, 0, totalCount_, 1, homoInterFuncCommOp,
              interRankBufferInfoManager_);
        } else if (homoInterFuncCommOp == flagcxCommOpRecv) {
          homoInterFuncSteps_[0].emplace_back(
              -1, sendType, recvType, 0, 0, totalCount_, 1, homoInterFuncCommOp,
              interRankBufferInfoManager_);
        } else if (homoInterFuncCommOp == flagcxCommNoOp) {
          homoInterFuncSteps_[0].emplace_back(-1, sendType, recvType, 0, 0,
                                              totalCount_, 1,
                                              homoInterFuncCommOp);
        }

        //调度循环，允许迭代式调度：对于复杂的无法一次性完成调度的场景，可以一层一层的、迭代的完成调度
        if (!scheduleCompleted) {//查询没有安排好P2P路径的
          refresh(0);
          heteroAndHomoInterFuncLoops += 1;//循环计数器增加，整个“搜索-调度-生成指令”过程再来一遍
        }
      }
    }
    interRankBufferInfoManager_.printBufferInfo(2);

    // setup postHomoFuncs
    //构建同构后处理
    //在代理间通信完成之后，每个同构子集群内部需要执行的操作
    flagcxCommOp_t postHomoFuncCommOp =
        eachNicPerRank_ ? getC2cHomoCommOp(2, 0) : getC2cHomoCommOp(2, 1);//调用辅助函数getC2cHomoCommOp获取
        // post阶段（阶段2）应该执行的操作
    if (postHomoFuncCommOp == flagcxCommOpAllReduce) {
      postHomoFuncSteps_[0].emplace_back(-1, sendType, 1, 0, 0, recvCount_, 2,
                                         postHomoFuncCommOp);
    } else if (postHomoFuncCommOp == flagcxCommOpReduceScatter) {
      postHomoFuncSteps_[0].emplace_back(-1, sendType, 1,
                                         clusterOffset_ * recvCount_, 0,
                                         recvCount_, 2, postHomoFuncCommOp);
    } else if (postHomoFuncCommOp == flagcxCommOpBroadcast) {//Broadcast是最常见的操作
      //代理节点完成了代理间通信后，有了最终的全局结果，现在把结果广播给集群内所有的其它成员
      //可能有多个外交官，每个有结果的一部分
      for (size_t i = 0; i < clusterInterRankList_[clusterId_].size(); ++i) {//遍历所有外交官
        auto &buffList = interRankBufferInfoManager_.getBufferInfoList(
            clusterId_, clusterInterRankList_[clusterId_][i]);
        for (auto it = buffList.begin(); it != buffList.end(); it++) {
          if (it->isRecv_) {
            if (commOp_ == flagcxCommOpAllGather && eachNicPerRank_) {
              size_t step = (comm_->cluster_ids[it->peerRank_] +
                             comm_->nclusters - 1 - clusterId_) %
                            comm_->nclusters;//step的计算表明广播操作也可以是流水线式，来匹配hetero和pre-homo阶段
              if (algorithm_ == flagcxAlgoPipeline) {
                step = 0;
              }
              postHomoFuncSteps_[step].emplace_back(
                  clusterInterRankList_[clusterId_][i] - (rank_ - homoMyRank_),
                  1, 1, it->offset_, it->offset_, it->count_, 2,
                  postHomoFuncCommOp);//为每个外交官创建一个broadcast指令，root参数是外交官的rank
            } else if (nPipePostSteps_ + nSeqPostSteps_ > 1) {
              size_t step = (comm_->cluster_ids[it->peerRank_] +
                             comm_->nclusters - clusterId_) %
                            comm_->nclusters;
              postHomoFuncSteps_[step].emplace_back(
                  clusterInterRankList_[clusterId_][i] - (rank_ - homoMyRank_),
                  1, 1, it->offset_, it->offset_, it->count_, 2,
                  postHomoFuncCommOp);
            } else {
              postHomoFuncSteps_[0].emplace_back(
                  clusterInterRankList_[clusterId_][i] - (rank_ - homoMyRank_),
                  sendType, 1, it->offset_, it->offset_, it->count_, 2,
                  postHomoFuncCommOp);
            }
          }
        }
      }
    } else if (postHomoFuncCommOp == flagcxCommOpScatter) {
      postHomoFuncSteps_[0].emplace_back(
          clusterInterRankList_[clusterId_][0] - (rank_ - homoMyRank_),
          sendType, 1, clusterOffset_ * recvCount_, 0, recvCount_, 2,
          postHomoFuncCommOp);
    } else if (postHomoFuncCommOp == flagcxCommOpSend) {
      postHomoFuncSteps_[0].emplace_back(comm_->globalrank2homorank[rootRank_],
                                         sendType, 1, 0, 0, totalCount_, 2,
                                         postHomoFuncCommOp);
    } else if (postHomoFuncCommOp == flagcxCommOpRecv) {
      postHomoFuncSteps_[0].emplace_back(comm_->globalrank2homorank[rootRank_],
                                         sendType, 1, 0, 0, totalCount_, 2,
                                         postHomoFuncCommOp);
    } else if (postHomoFuncCommOp == flagcxCommNoOp) {
    }
  } else {//单网卡场景，对应!multiNic_
    // single-nic
    // setup preHomoFuncs
    flagcxCommOp_t preHomoFuncCommOp = getC2cHomoCommOp(0, 2);//用辅助操作getC2cHomoCommOp获取单网卡模式（2）
    // 模式下，pre-homo阶段（0）的操作
    auto &buffer =
        interRankBufferInfoManager_
            .getBufferInfoList(clusterId_, clusterInterRankList_[clusterId_][0])
            .front();
    if (preHomoFuncCommOp == flagcxCommOpReduce) {//单网卡模式下，通常只有一个代理节点，pre-homo阶段是个简单reduce
      //创建一个reduce指令，root参数指向唯一的代理节点clusterInterRankList_[clusterId_][0]
      //单网卡模式的逻辑比多网卡模式简单，没有复杂的流水线和负载均衡计算
      preHomoFuncSteps_[0].emplace_back(
          clusterInterRankList_[clusterId_][0] - (rank_ - homoMyRank_), 0,
          recvType, buffer.offset_, buffer.offset_, buffer.count_, 0,
          preHomoFuncCommOp);
    } else if (preHomoFuncCommOp == flagcxCommOpGather) {
      preHomoFuncSteps_[0].emplace_back(
          clusterInterRankList_[clusterId_][0] - (rank_ - homoMyRank_), 0,
          recvType, 0, clusterOffset_ * sendCount_, sendCount_, 0,
          preHomoFuncCommOp);
    } else if (preHomoFuncCommOp == flagcxCommNoOp) {
      preHomoFuncSteps_[0].emplace_back(-1, 0, recvType, 0, 0, totalCount_, 0,
                                        preHomoFuncCommOp);
    }

    //专门处理单网卡场景下的pre-homo，hetero，homo-inter和post-homo
    //展示了一种不同于多网卡的、特殊优化的环形流水线算法
    // setup heteroFuncs
    //为不同的操作类型commOp_提供了不同的P2P策略

    //AllReduce、ReduceScatter和Reduce的策略
    //AllReduce的跨集群部分，被分解成一系列P2P操作，用取模运算，为每个代理节点分配接收数据的上家和发送数据的下家
    //代理节点之间形成了流水线式的环形Reduce-Scatter+All-Gather过程，每个步骤step中，每个代理节点都同时、并行地
    //send和recv，最大化利用网络带宽
    if (commOp_ == flagcxCommOpAllReduce ||
        commOp_ == flagcxCommOpReduceScatter || commOp_ == flagcxCommOpReduce) {
      flagcxC2cHeteroFunc heteroFunc = flagcxC2cHeteroFunc();
      for (size_t j = 0; j < clusterInterRankList_.size(); ++j) {//循环遍历所有其它集群
        if (clusterId_ == j) {
          continue;
        }
        if (isRootCluster_ || commOp_ != flagcxCommOpReduce) {
          //取模，为当前集群的每一个代理节点homoMyRank_，指定了一个需要负责接收数据的源集群，构成逻辑上数据流环
          int homoRankToRecvFromCluster =
              (comm_
                   ->globalrank2homorank[clusterInterRankList_[clusterId_][0]] -
               j - 1 + homoRanks_) %
              homoRanks_;
          if (homoMyRank_ == homoRankToRecvFromCluster) {
            heteroFunc.addP2pOp(rank_, clusterInterRankList_[j][0], 0,
                                totalCount_, 1);
          }
        }
        if (!isRootCluster_ || commOp_ != flagcxCommOpReduce) {
          //同理，计算为每个代理节点，指定需要发送数据的目标rank
          int homoRankToSendToCluster =
              (comm_->globalrank2homorank[clusterInterRankList_[j][0]] -
               clusterId_ - 1 + comm_->cluster_sizes[j]) %
              comm_->cluster_sizes[j];
          int globalRankToSendToCluster =
              homoRankToSendToCluster -
              comm_->globalrank2homorank[clusterInterRankList_[j][0]] +
              clusterInterRankList_[j][0];
          //每个代理节点homoMyRank_在循环中检查自己是否被分配到了send或recv的任务
          if (homoMyRank_ ==
              comm_
                  ->globalrank2homorank[clusterInterRankList_[clusterId_][0]]) {
            if ((commOp_ == flagcxCommOpReduce &&
                 comm_->cluster_ids[globalRankToSendToCluster] ==
                     rootClusterId_) ||
                (commOp_ == flagcxCommOpAllReduce ||
                 commOp_ == flagcxCommOpReduceScatter)) {
              heteroFunc.addP2pOp(rank_, globalRankToSendToCluster, 0,
                                  totalCount_, 0);
            }
          }
        }
      }
      heteroFuncSteps_[0].push_back(std::move(heteroFunc));
    } else if (commOp_ == flagcxCommOpAllGather) {//为AllGather设计的流水线式的P2P交换策略
      std::vector<flagcxC2cHeteroFunc> heteroFuncStep;
      for (size_t i = 0; i < nPipePreSteps_ + nSeqInterSteps_ + nPipePostSteps_;
           ++i) {
        heteroFuncStep.emplace_back();
      }
      int recvOffset = 0;
      for (size_t i = 0; i < clusterInterRankList_.size(); ++i) {
        if (clusterId_ == i) {
          recvOffset += comm_->cluster_sizes[i];
          continue;
        }
        if (homoInterMyRank_ != -1) {//确保只有代理节点才能参与
          if (algorithm_ == flagcxAlgoPipeline) {
            //环形流水线的step计算，表明AllGather的P2P交换也被分成了多个时间步骤
            size_t step =
                (clusterId_ + comm_->nclusters - 1 - i) % comm_->nclusters;
            heteroFuncStep[step].addP2pOp(rank_, clusterInterRankList_[i][0],
                                          clusterOffset_ * sendCount_,
                                          clusterCount_ * sendCount_, 0);
            heteroFuncStep[comm_->nclusters - 1 - step].addP2pOp(
                rank_, clusterInterRankList_[i][0], recvOffset * sendCount_,
                comm_->cluster_sizes[i] * sendCount_, 1);
          } else if (algorithm_ == flagcxAlgoSequential) {
            //每个步骤step中，代理节点向某个目标发送自己有的数据块，在另一个对称的步骤comm_->nclusters - 1 - step中，
            //从另一个集群接收它需要的数据块
            heteroFuncStep[0].addP2pOp(rank_, clusterInterRankList_[i][0],
                                       clusterOffset_ * sendCount_,
                                       clusterCount_ * sendCount_, 0);
            heteroFuncStep[0].addP2pOp(rank_, clusterInterRankList_[i][0],
                                       recvOffset * sendCount_,
                                       comm_->cluster_sizes[i] * sendCount_, 1);
          }
        }
        recvOffset += comm_->cluster_sizes[i];
      }
      for (size_t s = 0; s < heteroFuncStep.size(); ++s) {
        heteroFuncSteps_[s].push_back(std::move(heteroFuncStep[s]));
      }
    }

    // setup homoInterFuncs
    //代理间通信
    flagcxCommOp_t homoInterFuncCommOp = getC2cHomoCommOp(1, 2);//获取单网卡模式（2）下，inter阶段（1）的操作
    //这里被设置为flagcxCommNoOp无操作或一个简单的AllReduce，因为单网卡模式下，每个集群只有一个代理，代理间通信就
    //退化成了pre-homo和post-homo阶段的P2P操作，不需要独立的集体通信阶段
    if (homoInterFuncCommOp == flagcxCommOpAllReduce) {
      //emplace_back在vector容器末尾添加新元素
      homoInterFuncSteps_[0].emplace_back(-1, sendType, recvType, 0, 0,
                                          totalCount_,
                                          0, // use homo_comm
                                          homoInterFuncCommOp);
    } else if (homoInterFuncCommOp == flagcxCommOpReduce) {
      homoInterFuncSteps_[0].emplace_back(-1, sendType, recvType, 0, 0,
                                          totalCount_,
                                          0, // use homo_comm
                                          homoInterFuncCommOp);
    } else if (homoInterFuncCommOp == flagcxCommNoOp) {
      homoInterFuncSteps_[0].emplace_back(-1, sendType, recvType, 0, 0,
                                          totalCount_, 1, homoInterFuncCommOp);
    }

    // setup postHomoFuncs
    //同构后处理
    flagcxCommOp_t postHomoFuncCommOp = getC2cHomoCommOp(2, 2);//获取单网卡模式下，post阶段的操作
    if (postHomoFuncCommOp == flagcxCommOpReduceScatter) {
      postHomoFuncSteps_[0].emplace_back(-1, sendType, 1,
                                         clusterOffset_ * recvCount_, 0,
                                         recvCount_, 2, postHomoFuncCommOp);
    } else if (postHomoFuncCommOp == flagcxCommOpBroadcast) {//最常见的操作是Broadcast
      size_t clusterOffset = 0;
      for (size_t i = 0; i < clusterInterRankList_.size(); ++i) {
        if (nPipePreSteps_ + nSeqInterSteps_ + nPipePostSteps_ > 1) {
          size_t step = (i + comm_->nclusters - clusterId_) % comm_->nclusters;
          //代理节点同构hetero步骤收集到所有数据后，创建一系列broadcast指令，把最终的完整结果分发给集群内所有其它成员
          //step表欧美，分发过程也可以是流水线式的，匹配数据到达的节奏
          postHomoFuncSteps_[step].emplace_back(
              clusterInterRankList_[clusterId_][0] - (rank_ - homoMyRank_),
              sendType, 1, clusterOffset * sendCount_,
              clusterOffset * sendCount_, comm_->cluster_sizes[i] * sendCount_,
              2, postHomoFuncCommOp);
        } else {
          postHomoFuncSteps_[0].emplace_back(
              clusterInterRankList_[clusterId_][0] - (rank_ - homoMyRank_),
              sendType, 1, clusterOffset * sendCount_,
              clusterOffset * sendCount_, comm_->cluster_sizes[i] * sendCount_,
              2, postHomoFuncCommOp);
        }
        clusterOffset += comm_->cluster_sizes[i];
      }
    }
  }
  if (getenv("FLAGCX_ALGO_EXPORT_PREFIX")) {
    exportXml(getenv("FLAGCX_ALGO_EXPORT_PREFIX"));
  } else if (getenv("FLAGCX_ALGO_EXPORT_PATH")) {
    const char *algo_path = getenv("FLAGCX_ALGO_EXPORT_PATH");
    size_t algo_hash =
        genC2cAlgoHash(sendCount_, recvCount_, rootClusterId_, commOp_, redOp_);
    char prefix[128];
    snprintf(prefix, sizeof(prefix), "%s/%lu", algo_path, algo_hash);
    exportXml(prefix);
  }
  return flagcxSuccess;
}

flagcxResult_t flagcxC2cPlanner::execute(const void *sendbuff, void *recvbuff,
                                         flagcxDataType_t datatype, int root,
                                         flagcxStream_t stream,
                                         size_t *sendCounts, size_t *sDispls,
                                         size_t *recvCounts, size_t *rDispls) {
  // redOp validation
  if (redOp_ != flagcxRedNoOp) {
    if (redOp_ != flagcxSum && redOp_ != flagcxMax && redOp_ != flagcxMin) {
      WARN("Unsupported reduction operation %d", redOp_);
      return flagcxNotSupported;
    }
  }

  // root validation
  if (root != -1 && comm_->cluster_ids[root] != rootClusterId_) {
    WARN("Sorry, the input root cluster id is not valid %d[%d]",
         comm_->cluster_ids[root], rootClusterId_);
    return flagcxInvalidArgument;
  }

  // commOp validation
  // abort if nclusters > n-inter-ranks
  if (commOp_ == flagcxCommOpAllReduce ||
      commOp_ == flagcxCommOpReduceScatter) {
    int clusterCountValid_ = 1;
    for (int i = 0; i < comm_->nclusters; ++i) {
      int interRanks = int(clusterInterRankList_[i].size());
      if (comm_->nclusters > interRanks && comm_->nclusters > 2 &&
          interRanks > 1) {
        clusterCountValid_ = 0;
        break;
      }
    }
    if (!clusterCountValid_) {
      WARN("Unsupported communication operation %d since cluster count is "
           "larger than inter-rank count",
           commOp_);
      return flagcxNotSupported;
    }
  }
  // sendrecv counts and displs validation and initialization
  if (commOp_ == flagcxCommOpAlltoAllv) {
    if (sendCounts == nullptr || sDispls == nullptr || recvCounts == nullptr ||
        rDispls == nullptr) {
      WARN("Sorry, sendrecv counts and displacements need to be set for "
           "AlltoAllv operation");
      return flagcxInvalidArgument;
    }
    sendCounts_ = sendCounts;
    sDispls_ = sDispls;
    recvCounts_ = recvCounts;
    rDispls_ = rDispls;
  }

  int importAlgoFromXmlFile = 0;
  const char *algorithm = getenv("FLAGCX_C2C_ALGO");
  if (algorithm != NULL && strcmp(algorithm, "XML_INPUT") == 0) {
    const char *algo_path = getenv("FLAGCX_ALGO_IMPORT_PATH");
    const char *algo_prefix = getenv("FLAGCX_ALGO_IMPORT_PREFIX");
    if (algo_prefix) {
      FLAGCXCHECK(importXml(algo_prefix));
      importAlgoFromXmlFile = 1;
    } else if (algo_path) {
      size_t algo_hash = genC2cAlgoHash(sendCount_, recvCount_, rootClusterId_,
                                        commOp_, redOp_);
      char prefix[128];
      snprintf(prefix, sizeof(prefix), "%s/%lu", algo_path, algo_hash);
      FLAGCXCHECK(importXml(prefix));
      importAlgoFromXmlFile = 1;
    }
  }

  if (!strategyFound_ && !importAlgoFromXmlFile) {
    TRACE_CALL("Unable to load existing algorithm. Calling `findStrategy`...");
    FLAGCXCHECK(findStrategy());
    strategyFound_ = 1;
  }

  // init scratch buffer if need
  if (commOp_ == flagcxCommOpReduceScatter || commOp_ == flagcxCommOpScatter ||
      (commOp_ == flagcxCommOpGather && rank_ != rootRank_)) {
    deviceAdaptor->deviceMalloc(&scratchBuffer_,
                                totalCount_ * getFlagcxDataTypeSize(datatype),
                                flagcxMemDevice, stream);
  } else {
    scratchBuffer_ = nullptr;
  }

  void *recvTmpBuff = (scratchBuffer_ == nullptr) ? recvbuff : scratchBuffer_;
  void *sendTmpBuff =
      (commOp_ == flagcxCommOpAlltoAll || commOp_ == flagcxCommOpAlltoAllv ||
       (commOp_ == flagcxCommOpScatter && rank_ == rootRank_) ||
       (commOp_ == flagcxCommOpAllGather && eachNicPerRank_ && multiNic_))
          ? const_cast<void *>(sendbuff)
          : recvTmpBuff;

  flagcxStream_t het_stream;
  deviceAdaptor->streamCreate(&het_stream);

  // execute sequential preHomoFunc steps
  cclAdaptors[flagcxCCLAdaptorDevice]->groupStart();
  for (int s = 0; s < nSeqPreSteps_; ++s) {
    for (int i = 0; i < preHomoFuncSteps_[s].size(); ++i) {
      preHomoFuncSteps_[s][i].run(sendbuff, recvbuff, scratchBuffer_, datatype,
                                  redOp_, comm_->globalrank2homorank[root],
                                  comm_, stream, sendCounts_, sDispls_,
                                  recvCounts_, rDispls_);
    }
  }
  cclAdaptors[flagcxCCLAdaptorDevice]->groupEnd();
  deviceAdaptor->streamSynchronize(stream);

  // execute pipelined preHomoFunc and heteroFunc steps
  // execute refreshFunc
  if (refreshFunc_.bufftype_ == -1) {
    refreshFunc_.bufftype_ = scratchBuffer_ == nullptr ? 1 : 2;
    if ((commOp_ == flagcxCommOpReduceScatter ||
         commOp_ == flagcxCommOpAllReduce) &&
        algorithm_ == flagcxAlgoPipeline) {
      refreshFunc_.start_ = clusterOffset_ * totalCount_ / comm_->nranks;
    }
  }
  refreshFunc_.run(recvbuff, scratchBuffer_, datatype, stream);
  deviceAdaptor->streamSynchronize(stream);
  for (int s = 0; s < nPipePreSteps_; ++s) {
    cclAdaptors[flagcxCCLAdaptorDevice]->groupStart();
    for (int i = 0; i < preHomoFuncSteps_[nSeqPreSteps_ + s].size(); ++i) {
      preHomoFuncSteps_[nSeqPreSteps_ + s][i].run(
          sendbuff, recvbuff, scratchBuffer_, datatype, redOp_,
          comm_->globalrank2homorank[root], comm_, stream, sendCounts_,
          sDispls_, recvCounts_, rDispls_);
    }
    cclAdaptors[flagcxCCLAdaptorDevice]->groupEnd();
    flagcxHeteroGroupStart();
    for (int i = 0; i < heteroFuncSteps_[s].size(); ++i) {
      // TODO: use stream wait rather than stream sync to avoid cpu blocking
      // deviceAdaptor->streamSynchronize(stream);

      // execute heteroFuncs
      heteroFuncSteps_[s][i].run(sendTmpBuff, recvTmpBuff, datatype, comm_,
                                 het_stream);

      if (homoInterFuncSteps_[s].size() > i) {
        // TODO: use stream wait rather than stream sync to avoid cpu blocking
        deviceAdaptor->streamSynchronize(het_stream);

        // execute homoInterFuncs
        homoInterFuncSteps_[s][i].run(
            sendbuff, recvbuff, scratchBuffer_, datatype, redOp_,
            comm_->globalrank2homorank[root], comm_, het_stream);
        refreshFunc_.run(recvbuff, scratchBuffer_, datatype, stream);
      }
    }
    flagcxHeteroGroupEnd();
    // todo: double-check the synchronization logic
    deviceAdaptor->streamSynchronize(stream);
    deviceAdaptor->streamSynchronize(het_stream);
  }

  // execute sequential heteroFunc steps
  for (int s = 0; s < nSeqInterSteps_; ++s) {
    for (int i = 0; i < heteroFuncSteps_[nPipePreSteps_ + s].size(); ++i) {
      // execute refreshFunc
      if (algorithm_ == flagcxAlgoSequential ||
          (nPipePreSteps_ == 0 && nPipePostSteps_ == 0)) {
        refreshFunc_.run(recvbuff, scratchBuffer_, datatype, stream);
      }

      // TODO: use stream wait rather than stream sync to avoid cpu blocking
      // deviceAdaptor->streamSynchronize(stream);

      // execute heteroFuncs
      heteroFuncSteps_[nPipePreSteps_ + s][i].run(sendTmpBuff, recvTmpBuff,
                                                  datatype, comm_, stream);

      if (homoInterFuncSteps_[nPipePreSteps_ + s].size() > i) {
        // TODO: use stream wait rather than stream sync to avoid cpu blocking
        deviceAdaptor->streamSynchronize(stream);

        // execute homoInterFuncs
        homoInterFuncSteps_[nPipePreSteps_ + s][i].run(
            sendbuff, recvbuff, scratchBuffer_, datatype, redOp_,
            comm_->globalrank2homorank[root], comm_, stream);
        if (algorithm_ == flagcxAlgoPipeline &&
            (nPipePreSteps_ > 0 || nPipePostSteps_ > 0)) {
          refreshFunc_.run(recvbuff, scratchBuffer_, datatype, stream);
        }
      }
    }
  }
  deviceAdaptor->streamSynchronize(stream);

  // execute pipelined heteroFunc and postHomoFunc steps
  for (int s = 0; s < nPipePostSteps_; ++s) {
    cclAdaptors[flagcxCCLAdaptorDevice]->groupStart();
    // execute postHomoFunc
    for (int i = 0; i < postHomoFuncSteps_[s].size(); ++i) {
      postHomoFuncSteps_[s][i].run(sendbuff, recvbuff, scratchBuffer_, datatype,
                                   redOp_, comm_->globalrank2homorank[root],
                                   comm_, stream);
    }
    cclAdaptors[flagcxCCLAdaptorDevice]->groupEnd();

    flagcxHeteroGroupStart();
    for (int i = 0;
         i < heteroFuncSteps_[nPipePreSteps_ + nSeqInterSteps_ + s].size();
         ++i) {
      // TODO: use stream wait rather than stream sync to avoid cpu blocking
      // deviceAdaptor->streamSynchronize(stream);

      // execute heteroFuncs
      heteroFuncSteps_[nPipePreSteps_ + nSeqInterSteps_ + s][i].run(
          sendTmpBuff, recvTmpBuff, datatype, comm_, het_stream);

      if (homoInterFuncSteps_[nPipePreSteps_ + nSeqInterSteps_ + s].size() >
          i) {
        // TODO: use stream wait rather than stream sync to avoid cpu blocking
        deviceAdaptor->streamSynchronize(het_stream);

        // execute homoInterFuncs
        homoInterFuncSteps_[nPipePreSteps_ + nSeqInterSteps_ + s][i].run(
            sendbuff, recvbuff, scratchBuffer_, datatype, redOp_,
            comm_->globalrank2homorank[root], comm_, het_stream);
      }
    }
    flagcxHeteroGroupEnd();

    deviceAdaptor->streamSynchronize(stream);
    deviceAdaptor->streamSynchronize(het_stream);
  }

  // execute sequential postHomoFunc steps
  cclAdaptors[flagcxCCLAdaptorDevice]->groupStart();
  for (int s = 0; s < nSeqPostSteps_; ++s) {
    for (int i = 0; i < postHomoFuncSteps_[nPipePostSteps_ + s].size(); ++i) {
      // execute refresh func
      if (algorithm_ == flagcxAlgoSequential ||
          (nPipePreSteps_ == 0 && nPipePostSteps_ == 0)) {
        refreshFunc_.run(recvbuff, scratchBuffer_, datatype, stream);
      }

      // execute postHomoFunc
      postHomoFuncSteps_[nPipePostSteps_ + s][i].run(
          sendbuff, recvbuff, scratchBuffer_, datatype, redOp_,
          comm_->globalrank2homorank[root], comm_, stream);
    }
  }
  cclAdaptors[flagcxCCLAdaptorDevice]->groupEnd();

  // free scratch buffer if needed
  if (scratchBuffer_ != nullptr) {
    deviceAdaptor->deviceFree(scratchBuffer_, flagcxMemDevice, stream);
  }

  // destroy temporary hetero comm stream
  deviceAdaptor->streamDestroy(het_stream);

  return flagcxSuccess;
}
