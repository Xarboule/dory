#pragma once

#include <iostream>
#include <sstream>
#include <string>

#include <dory/ctrl/block.hpp>
#include <dory/shared/logger.hpp>


#include <dory/extern/ibverbs.hpp>
#include <dory/extern/rdmacm.hpp>

namespace dory {
struct RemoteConnection {
  struct __attribute__((packed)) RemoteConnectionInfo {
    uint16_t lid;
    uint32_t qpn;

    uintptr_t buf_addr;
    uint64_t buf_size;
    uint32_t rkey;
  };

  RemoteConnection() {
    rci.lid = 0;

  }

  RemoteConnection(uint16_t lid, uint32_t qpn, uintptr_t buf_addr,
                   uint64_t buf_size, uint32_t rkey) {
    rci.lid = lid;
    rci.qpn = qpn;
    rci.buf_addr = buf_addr;
    rci.buf_size = buf_size;
    rci.rkey = rkey;
  }

  RemoteConnection(RemoteConnectionInfo rci) : rci{rci} {}

  std::string serialize() const {
    std::ostringstream os;

    os << std::hex << rci.lid << ":" << rci.qpn << ":" << rci.buf_addr << ":"
       << rci.buf_size << ":" << rci.rkey;
    return os.str();
  }

  static RemoteConnection fromStr(std::string const &str) {
    RemoteConnectionInfo rci;

    std::string res(str);

    std::replace(res.begin(), res.end(), ':', ' ');  // replace ':' by ' '

    std::stringstream ss(res);

    uint16_t lid;
    uint32_t qpn;

    uintptr_t buf_addr;
    uint64_t buf_size;
    uint32_t rkey;

    ss >> std::hex >> lid;
    ss >> std::hex >> qpn;
    ss >> std::hex >> buf_addr;
    ss >> std::hex >> buf_size;
    ss >> std::hex >> rkey;

    rci.lid = lid;
    rci.qpn = qpn;
    rci.buf_addr = buf_addr;
    rci.buf_size = buf_size;
    rci.rkey = rkey;

    return RemoteConnection(rci);
  }

  // private:
  RemoteConnectionInfo rci;
};

class ReliableConnection {
 public:
  enum CQ { SendCQ, RecvCQ };

  enum RdmaReq { RdmaRead = IBV_WR_RDMA_READ, RdmaWrite = IBV_WR_RDMA_WRITE };

  static constexpr int WRDepth = 128;
  static constexpr int SGEDepth = 16;
  static constexpr int MaxInlining = 256;
  static constexpr uint32_t DefaultPSN = 3185;

  ReliableConnection(ControlBlock &cb);

  void bindToPD(std::string pd_name);

  void bindToMR(std::string mr_name);

  void associateWithCQ(std::string send_cp_name, std::string recv_cp_name);

  void reset();

  void init(ControlBlock::MemoryRights rights);
  void reinit();

  void connect(RemoteConnection &rci);
  void reconnect();

  bool needsReset();
  bool changeRights(ControlBlock::MemoryRights rights);
  bool changeRightsIfNeeded(ControlBlock::MemoryRights rights);

  bool postSendSingle(RdmaReq req, uint64_t req_id, void *buf, uint32_t len,
                      uintptr_t remote_addr);

  // Only re-use this method when the previous WR posted by this method is
  // completed and a corresponding WC was consumed, otherwise unexpected
  // behaviour might occur. In case the WR is posted with `IBV_SEND_INLINE`
  // (which is the case when the length of the payload is smaller or equal to
  // `MaxInlining`) one can reuse this method right after it returns.
  bool postSendSingleCached(RdmaReq req, uint64_t req_id, void *buf,
                            uint32_t len, uintptr_t remote_addr);

  bool postSendSingle(RdmaReq req, uint64_t req_id, void *buf, uint32_t len,
                      uint32_t lkey, uintptr_t remote_addr);

  bool pollCqIsOK(CQ cq, std::vector<struct ibv_wc> &entries);

  RemoteConnection remoteInfo() const;

  uintptr_t remoteBuf() const { return rconn.rci.buf_addr; }

  const ControlBlock::MemoryRegion &get_mr() const { return mr; }

  void query_qp(ibv_qp_attr &qp_attr, ibv_qp_init_attr &init_attr,
                int attr_mask) const;

  /*Méthodes ajoutées pour pouvoir utiliser rdma_create_qp() dans exchanger.cpp*/
  struct ibv_pd* get_pd();

  struct ibv_qp_init_attr* get_init_attr();

  struct rdma_cm_id* get_cm_listen_id();

  struct rdma_cm_id* get_cm_id();
  
  void set_cm_id(rdma_cm_id* id);

  struct rdma_event_channel* get_event_channel();
  
  void associateWithCQ_for_cm_prel(std::string send_cp_name,
                                         std::string recv_cp_name);

  void associateWithCQ_for_cm();

  void configure_cm_channel();

  void * getLocalSetup();
  
  void setRemoteSetup(const void *network_data);

  void print_all_infos();

  void set_init_with_cm(ControlBlock :: MemoryRights rights);

 private:
  bool post_send(ibv_send_wr &wr);

  static void wr_deleter(struct ibv_send_wr *wr) { free(wr); }

  size_t roundUp(size_t numToRound, size_t multiple) {
    if (multiple == 0) return numToRound;

    size_t remainder = numToRound % multiple;
    if (remainder == 0) return numToRound;

    return numToRound + multiple - remainder;
  }

  ControlBlock &cb;   //?? 
  struct ibv_pd *pd;
  ControlBlock::MemoryRegion mr;
  deleted_unique_ptr<struct ibv_qp> uniq_qp;

  struct ibv_qp_init_attr create_attr;
  struct ibv_qp_attr conn_attr;   //normalement inutile 
  ControlBlock::MemoryRights init_rights;


  struct rdma_event_channel *cm_event_channel;
  struct rdma_cm_id *cm_listen_id;
  struct rdma_cm_id *cm_id;
  //TO DO : add cm_id_listen et corriger le reste ;

  RemoteConnection rconn;
  deleted_unique_ptr<struct ibv_send_wr> wr_cached;
  
  LOGGER_DECL(logger);


};
}  // namespace dory
