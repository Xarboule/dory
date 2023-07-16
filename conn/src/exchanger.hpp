#pragma once

#include <chrono>
#include <cstddef>
#include <future>
#include <map>
#include <memory>
#include <thread>
#include <vector>

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

  void announce(int proc_id, MemoryStore& store, std::string const& prefix);

  void announce_all(MemoryStore& store, std::string const& prefix);

  void connect(int proc_id, MemoryStore& store, std::string const& prefix,
               ControlBlock::MemoryRights rights = ControlBlock::LOCAL_READ);

  void connect_all(
      MemoryStore& store, std::string const& prefix,
      ControlBlock::MemoryRights rights = ControlBlock::LOCAL_READ);

  void announce_ready(MemoryStore& store, std::string const& prefix,
                      std::string const& reason);

  void wait_ready(int proc_id, MemoryStore& store, std::string const& prefix,
                  std::string const& reason);

  void wait_ready_all(MemoryStore& store, std::string const& prefix,
                      std::string const& reason);

  std::map<int, ReliableConnection>& connections() { return rcs; }

  void addLoopback(std::string const& pd, std::string const& mr,
                   std::string send_cq_name, std::string recv_cq_name);

  void connectLoopback(ControlBlock::MemoryRights rights);
  ReliableConnection& loopback() { return *(loopback_.get()); }


    //nouvelles fonctions pour utiliser CM 
    void configure_with_cm(int proc_id, std::string const& pd,
                                            std::string const& mr,
                                        std::string send_cq_name,
                                        std::string recv_cq_name);
    
    void configure_all_with_cm(std::string const& pd,
                                            std::string const& mr,
                                            std::string send_cq_name,
                                            std::string recv_cq_name);

    void connect_with_cm(int proc_id,std::string const& prefix,
        ControlBlock::MemoryRights rights = ControlBlock::LOCAL_READ);

    void connect_all_with_cm(MemoryStore& store,
                                        std::string const& prefix,
        ControlBlock::MemoryRights rights = ControlBlock::LOCAL_READ);

    int start_server(int proc_id, ControlBlock::MemoryRights rights);

    int start_client(int proc_id, ControlBlock::MemoryRights rights); 

    int process_rdma_cm_event(struct rdma_event_channel *echannel,
            enum rdma_cm_event_type expected_event,
            struct rdma_cm_event **cm_event);

    int get_addr(char *dst, struct sockaddr *addr);

    void show_rdma_cmid(struct rdma_cm_id *id);

    static int get_num_conn();

    void incr_num_conn();

    void addLoopback_with_cm(std::string const& pd,
                                      std::string const& mr,
                                      std::string send_cq_name,
                                      std::string recv_cq_name);

    void connectLoopback_with_cm(ControlBlock::MemoryRights rights);

    void threaded_client(ControlBlock::MemoryRights rights);

    int start_loopback_server(ControlBlock::MemoryRights rights);

    int start_loopback_client(ControlBlock::MemoryRights rights);

 private:
  std::pair<bool, int> valid_ids() const;

 /*public :
  struct rdma_event_channel *cm_event_channel;
  struct rdma_cm_id *cm_id;*/
 
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

  static int num_conn;

  ControlBlock :: MemoryRights myrights;
};
}  // namespace dory
