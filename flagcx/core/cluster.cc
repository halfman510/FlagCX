#include "cluster.h"
#include <cstring>

flagcxResult_t parseClusterSplitList(const char *input,
                                     std::vector<int> &output) {
  output.clear();
  std::stringstream ss(input);
  std::string token;

  while (std::getline(ss, token, ',')) {
    try {
      int value = std::stoi(token);
      output.push_back(value);
    } catch (const std::exception &e) {
      WARN("Invalid cluster split info, its format should be like '2,4,8,...'");
      return flagcxSystemError;
    }
  }

  return flagcxSuccess;
}

//通信器类型分类过程
//根据全局硬件厂商信息，判断通信组是同构还是异构，计算当前进程在同构子集群中的本地排名等信息，可选地对同构集群进行更细
// 粒度的虚拟划分
flagcxResult_t flagcxCollectClusterInfos(const flagcxVendor *allData,
                                         flagcxCommunicatorType_t *type,
                                         int *homo_rank, int *homo_root_rank,
                                         int *homo_ranks, int *cluster_id,
                                         int *cluster_inter_rank, int *ncluster,
                                         int rank, int nranks) {
  //开始时把所有的输出参数初始化成简单的单机单进程的同构环境默认值
  *homo_rank = rank;
  *homo_root_rank = 0;
  *homo_ranks = 1;
  *cluster_id = 0;
  *cluster_inter_rank = -1; // deprecated, to be removed
  *ncluster = 1;
  *type = flagcxCommunicatorHomo;

  if (nranks <= 1)
    return flagcxSuccess;

  std::map<std::string, int> clusterMap;
  clusterMap[allData[0].internal] = 1;
  int numClusters = 1;
  int currCluster = 0;
  int aggRanks = 1;
  int homoRootRank = 0;
  std::string myCls = allData[rank].internal;
  for (int i = 1; i < nranks; ++i) {//遍历allDate数组，包含所有硬件厂商字符串
    std::string cls = allData[i].internal;
    auto it = clusterMap.find(cls);//一个map统计一种硬件厂商std::string出现了多少次
    if (it != clusterMap.end()) {
      it->second = it->second + 1;
    } else {
      clusterMap[cls] = 1;
      numClusters += 1;//不同的硬件厂商总数
      if (myCls == cls) {//计算本地排名homo_rank
        *homo_rank = *homo_rank - aggRanks;//计算当前进程，在它所属的同构子集群内部是第几个
        currCluster = numClusters - 1;
        homoRootRank = i;
      }
    }
    aggRanks += 1;

    if (i == rank) {
      *homo_root_rank = homoRootRank;
    }
  }

  //宏观分类
  *homo_ranks = clusterMap[myCls];//从map中查当前进程所属的同构子集群的总大小

  if (clusterMap.size() > 1) {//如果map的大小大于1，也就是不同厂商的数量大于1，把通信器的类型设为异构混合
    *type = flagcxCommunicatorHybrid;
  } else {
    *type = flagcxCommunicatorHomo;
  }

  if (*type == flagcxCommunicatorHybrid) {//如果是异构，设置集群ID和总集群数
    *cluster_id = currCluster;
    *ncluster = numClusters;
  }

  //子集群划分【可选】
  // split and obtain sub-clusters
  const char *clusterSplitInfo = flagcxGetEnv("FLAGCX_CLUSTER_SPLIT_LIST");//通过环境变量来看是否分割为更小的逻辑子集群
  if (clusterSplitInfo != NULL) {
    std::vector<int> clusterSplitList;
    FLAGCXCHECK(parseClusterSplitList(clusterSplitInfo, clusterSplitList));//parseClusterSplitList解析环境变量字符串
    if (*ncluster != int(clusterSplitList.size())) {
      WARN("Invalid cluster split info, its length should be equal to the "
           "number of homogeneous cluster");
      return flagcxSystemError;
    }

    //sub系列变量，根据clusterSplitList的配置，计算除法和取模，为当前进程重新计算全新的子集群身份信息
    int subClusterId = 0;//虚拟子集群ID
    for (int i = 0; i < currCluster; ++i) {
      subClusterId += clusterSplitList[i];
    }
    int subHomoRanks = (*homo_ranks) / clusterSplitList[currCluster];//虚拟子集群大小
    int hasRes =
        (((*homo_rank) / subHomoRanks) >= clusterSplitList[currCluster]) ? 1
                                                                         : 0;
    subClusterId += (hasRes == 1) ? ((*homo_rank) / subHomoRanks) - 1
                                  : ((*homo_rank) / subHomoRanks);
    int subHomoRank = (hasRes == 1)
                          ? subHomoRanks + ((*homo_rank) % subHomoRanks)
                          : ((*homo_rank) % subHomoRanks);//虚拟子集群内的序号
    int subHomoRootRank = rank - subHomoRank;
    if (hasRes == 1 ||
        ((*homo_rank) / subHomoRanks) == clusterSplitList[currCluster] - 1) {
      subHomoRanks += (*homo_ranks) % clusterSplitList[currCluster];
    }
    int subNClusters = 0;//虚拟子集群总数
    for (int i = 0; i < (*ncluster); ++i) {
      subNClusters += clusterSplitList[i];
    }
    *homo_rank = subHomoRank;
    *homo_root_rank = subHomoRootRank;
    *homo_ranks = subHomoRanks;
    *cluster_id = subClusterId;
    *ncluster = subNClusters;
    *type =
        (subNClusters > 1) ? flagcxCommunicatorHybrid : flagcxCommunicatorHomo;//根据新的总集群数，判断系统是
        // 同构还是异构。eg：一个物理上的同构集群，被虚拟划分为多个子集群后，就变成了逻辑上的异构系统
  }

  return flagcxSuccess;
}

//展示了集群映射到厂商映射表的过程，根据计算好的每个rank的集群归属信息，创建从集群ID—硬件厂商类型的对应关系。
flagcxResult_t flagcxFillClusterVendorInfo(const flagcxVendor *allData,
                                           flagcxComm *comm, int *clusterIdData,
                                           int nranks, int ncluster) {
  comm->clusterVendorMap.resize(ncluster);
  for (int i = 0; i < nranks; i++) {//遍历所有rank
    std::string vendor = allData[i].internal;
    int cluster = clusterIdData[i];
    if (vendor == "NVIDIA") {//用枚举类型判断比字符串判断快得多、也更不容易出错，避免了大小写、拼写错误等问题。
      comm->clusterVendorMap[cluster] = FLAGCX_VENDOR_NVIDIA;
    } else if (vendor == "ILUVATAR_COREX") {
      comm->clusterVendorMap[cluster] = FLAGCX_VENDOR_ILUVATAR_COREX;
    } else if (vendor == "MLU") {
      comm->clusterVendorMap[cluster] = FLAGCX_VENDOR_MLU;
    } else if (vendor == "METAX") {
      comm->clusterVendorMap[cluster] = FLAGCX_VENDOR_METAX;
    }
  }
  return flagcxSuccess;
}