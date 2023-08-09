#pragma once

#include <chrono>
#include <cstddef>
#include <future>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include <fstream> //to read config.txt

#include <dory/ctrl/block.hpp>
#include <dory/shared/logger.hpp>
#include <dory/store.hpp>
#include "rc.hpp"

/*les librairies pour cm */
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <dory/extern/ibverbs.hpp>
#include <dory/extern/rdmacm.hpp>


namespace dory {
class ConnectionExchanger {
 private:
  static constexpr double gapFactor = 2;
  static constexpr auto retryTime = std::chrono::milliseconds(20);

 public:
  ConnectionExchanger(int my_id, std::vector<int> remote_ids, ControlBlock& cb);

  void configure(int proc_id, std::string const& pd, std::string const& mr,
                 std::string send_cp_name, std::string recv_cp_name);

  void configure_all(std::string const& pd, std::string const& mr,
                     std::string send_cp_name, std::string recv_cp_name);

  void configure_with_cm(int proc_id, std::string const& pd,
                                      std::string const& mr,
                                      std::string send_cq_name,
                                      std::string recv_cq_name);
    
  void configure_all_with_cm( std::string const& pd,
                              std::string const& mr,
                              std::string send_cq_name,
                              std::string recv_cq_name);

  void addLoopback(std::string const& pd, std::string const& mr,
                   std::string send_cq_name, std::string recv_cq_name);

  void connectLoopback(ControlBlock::MemoryRights rights);
  ReliableConnection& loopback() { return *(loopback_.get()); }
  ReliableConnection& getTofinoRC(){return *(rc_tofino_.get()); }

  void addLoopback_with_cm( std::string const& pd,
                            std::string const& mr,
                            std::string send_cq_name,
                            std::string recv_cq_name);

  void connectLoopback_with_cm(ControlBlock::MemoryRights rights);

  int start_loopback_server(ControlBlock::MemoryRights rights);

  int start_loopback_client(ControlBlock::MemoryRights rights);


  void connect(int proc_id, MemoryStore& store, std::string const& prefix,
               ControlBlock::MemoryRights rights = ControlBlock::LOCAL_READ);

  void connect_all(
      MemoryStore& store, std::string const& prefix, int base_port,
      ControlBlock::MemoryRights rights = ControlBlock::LOCAL_READ);

  void start_server(int proc_id, int my_port, ControlBlock::MemoryRights rights);

  int start_client(int proc_id, int dest_port, ControlBlock::MemoryRights rights); 

  int get_addr(std:: string dst, struct sockaddr *addr);
  
  int process_rdma_cm_event(struct rdma_event_channel *echannel,
          enum rdma_cm_event_type expected_event,
          struct rdma_cm_event **cm_event);
  
  void build_conn_param(rdma_conn_param *cm_params);
  
  //useful for debugging
  void show_rdma_cmid(struct rdma_cm_id *id);
  void check_all_qp_states();
  
  std::map<int, ReliableConnection>& connections() { return rcs; }

  
  int setup_tofino();

  void init_addr_tofino();
  void init_my_addr();
 

 private:
  std::pair<bool, int> valid_ids() const;

 private:
  int my_id;
  std::vector<int> remote_ids;
  ControlBlock& cb;
  int max_id;
  std::map<int, ReliableConnection> rcs;
  
  std::unique_ptr<ReliableConnection> loopback_;
  std::unique_ptr<ReliableConnection> remote_loopback_;
  int loopback_port;

  LOGGER_DECL(logger);

  std::map<int, std::string> ipAddresses;
  std::ifstream ifs;

  sockaddr_in addr_tofino;
  sockaddr_in my_addr;
  
  std::unique_ptr<ReliableConnection> rc_tofino_; 
  bypass::connection conn_tof;
  
  
};
}  // namespace dory
