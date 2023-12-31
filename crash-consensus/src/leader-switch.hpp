#pragma once

#include "error.hpp"
#include "log.hpp"
#include "message-identifier.hpp"

#include <iterator>
#include <set>

#include "context.hpp"
#include "remote-log-reader.hpp"

#include "config.hpp"
#include "fixed-size-majority.hpp"
#include "follower.hpp"
#include "pinning.hpp"

#include "timers.h"

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

#include "contexted-poller.hpp"

namespace dory {
class LeaderHeartbeat {
 private:
  // static constexpr std::chrono::nanoseconds heartbeatRefreshRate =
  // std::chrono::nanoseconds(500);
  static constexpr int history_length = 50;

 public:
  LeaderHeartbeat() {}
  LeaderHeartbeat(LeaderContext *ctx) : ctx{ctx}, want_leader{false} {
    // Careful, there is a move assignment happening!
  }

  void startPoller() {
    read_seq = 0;

    //extraction des infos à partir du contexte de connexion 
    rcs = &(ctx->cc.ce.connections());
    my_id = ctx->cc.my_id;
    loopback = &(ctx->cc.ce.loopback());
    ids = ctx->cc.remote_ids;
    ids.push_back(ctx->cc.my_id);
    
    //extraction des infos à partir du scratchpad
    offset = ctx->scratchpad.leaderHeartbeatSlotOffset();
    counter_from = reinterpret_cast<uint64_t *>(ctx->scratchpad.leaderHeartbeatSlot() + 64);
    *counter_from = 0;
    slots = ctx->scratchpad.readLeaderHeartbeatSlots();

    std::sort(ids.begin(), ids.end());
    max_id = *(std::minmax_element(ids.begin(), ids.end()).second);

    status = std::vector<ReadingStatus>(max_id + 1);
    status[ids[0]].consecutive_updates = history_length;

    //??
    ctx->poller.registerContext(quorum::LeaderHeartbeat);
    ctx->poller.endRegistrations(4);
    heartbeat_poller = ctx->poller.getContext(quorum::LeaderHeartbeat);

    post_id = 0;
    post_ids.resize(max_id + 1);
    for (auto &id : post_ids) {
      id = post_id;
    }
  }

  void retract() { want_leader.store(false); } //==> je ne veux plus être leader

  /*C'est la fonction la plus importante de cette thread : 
      -outstanding_pids = les pids à qui j'ai déjà envoyé une requête RDMA (Read ou Write )
    On envoie un write à son loopback pour incrémenter son heartbeat.
    On envoie un read à tous ceux qui ne sont pas dans outstanding_pid.
    On vérifie tous les wc, on lit les nouveaux heartbeats et on met à jour les scores 

    Remarque : ça a pris du temps de localiser l'erreur, car le code ne vérifie pas que le wc indique un truc de valide ! 
  */
  void scanHeartbeats() {
        //si mon id n'est pas en cours de vérification (avec un Write envoyé), alors je l'envoie maintenant
    if (outstanding_pids.find(my_id) == outstanding_pids.end()) {
      // Update my heartbeat
      *counter_from += 1;
      auto post_ret = loopback->postSendSingle(
          ReliableConnection::RdmaWrite, //request type
          quorum::pack(quorum::LeaderHeartbeat, my_id, 0), //request id 
          counter_from, //buffer sent 
          sizeof(uint64_t),  //size of the buffer
          loopback->remoteBuf() + offset); //where to write 

      if (!post_ret) {
        std::cout << "(Error in posting the update of heartbeat) Post returned " << post_ret << std::endl;
      }
      //std :: cout << "The address that my loopback (local heartbeat) is writing to is : " << loopback->remoteBuf() + offset << std::endl;
      outstanding_pids.insert(my_id);
      //std::cout << "State of the qp I just posted to (loopback) : " << loopback->query_qp_state() << std::endl;

      /*
      //test : est-ce que j'arrive à faire un RDMA READ de ma propre valeur ? (READ par ma loopback)
      std::cout << "Posting a local Read to my own heartbeat" << std::endl; 
      post_ret = loopback->postSendSingle(
        ReliableConnection::RdmaRead, 
        quorum::pack(quorum::LeaderHeartbeat, my_id, read_seq), 
        slots[my_id], //where to store the content read
        sizeof(uint64_t),
        loopback->remoteBuf() + offset); //where to read

      if (!post_ret) {
        std::cout << "(Error in posting read request to my own heartbeat) Post returned " << post_ret << std::endl;
      }*/
    }


        
    


    bool did_work = false;
    auto &rcs_ = *rcs;
    for (auto &[pid, rc] : rcs_) { 
      if (outstanding_pids.find(pid) != outstanding_pids.end()) {
        continue; //si une requête est déjà envoyé pour ce pid, alors je passe au suivant
      } 

      //sinon, je m'en occupe : je l'ajoute à la liste oustanding_pids et j'envoie la requête (READ)
      did_work = true;
      outstanding_pids.insert(pid);
      post_ids[pid] = post_id;

      
      auto post_ret = rc.postSendSingle(
          ReliableConnection::RdmaRead, 
          quorum::pack(quorum::LeaderHeartbeat, pid, read_seq), 
          slots[pid], //where to store the content read
          sizeof(uint64_t),
          rc.remoteBuf() + offset); //where to read
    
      if (!post_ret) {
        std::cout << "(Error in posting the reading of the heartbeats) Post returned " << post_ret << std::endl;
      }

      /*
      std::cout << "The following request has been posted" << std::endl;
      std::cout << "polling PID = " << pid <<"; my id = "<< my_id << std::endl;
      std::cout << "About the rc : " << std::endl; 
      rc.print_all_infos();
      std::cout << "Where to read : " << rc.remoteBuf()+offset<< std :: endl;

      std::cout << "State of the qp I just posted to : " << rc.query_qp_state() << std::endl;*/
    }

    

    if (did_work) { //?? 
      post_id += 1;
    }

    read_seq += 1;

    // If the number of outstanding requests goes out of hand, go slower    
    entries.resize(outstanding_pids.size());
    
    //on récupère les entrées qui concernent le heartbeat
    if (heartbeat_poller(ctx->cc.cq, entries)) {
      //std::cout << "Polled " << entries.size() << " entries" << std::endl;

      for (auto const &entry : entries) {
        auto [k, pid, seq] = quorum::unpackAll<int, uint64_t>(entry.wr_id);
        IGNORE(k);
        IGNORE(seq);

        outstanding_pids.erase(pid); //on enlève le pid concerné de la liste car on vient de récupérer l'ack de la requête 
        auto proc_post_id = post_ids[pid];

        volatile uint64_t *val = reinterpret_cast<uint64_t *>(slots[pid]); //on récupère la valeur
        if (pid == my_id) {
          val = reinterpret_cast<uint64_t *>(loopback->remoteBuf() + offset); //si c'est la mienne, c'est un peu spécial 
        }

        //std::cout << "Polling PID: " << pid << ", PostID: " << proc_post_id << ", Value: " << *val << std::endl;
        //std::cout << "About the associated work request'status : "<< ibv_wc_status_str(entry.status) << std::endl;      
        

        if (status[pid].value == *val) { //si la valeur du heartbeat est la même qu'avant
          status[pid].consecutive_updates = std::max(status[pid].consecutive_updates, 1) - 1; //score décrémenté
        } else { 
          if (post_id < proc_post_id + 3) { //sinon ? 
            status[pid].consecutive_updates = std::min(status[pid].consecutive_updates, history_length - 3) + 3;
          }
        }

        status[pid].value = *val; //maj de la valeur
      }
    }
    /*
    std::cout << "===========Scores=========="<<std::endl;
    for (auto& pid: ids) {
        std::cout << "PID:" << pid << ", score: " << status[pid].consecutive_updates << std::endl;
    }
    std::cout << "So the leader I consider is : " << std::to_string(leader_pid()) << std::endl;
    */
    if (leader_pid() == ctx->cc.my_id) {
      want_leader.store(true);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      //std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }

  std::atomic<bool> &wantLeaderSignal() { return want_leader; }

  // Move assignment operator
  LeaderHeartbeat &operator=(LeaderHeartbeat &&o) {
    if (&o == this) {
      return *this;
    }

    ctx = o.ctx;
    o.ctx = nullptr;
    want_leader.store(false);
    return *this;
  }

 private:
  struct ReadingStatus {
    ReadingStatus()
        : value{0},
          consecutive_updates{0},
          failed_attempts{0},
          loop_modulo{0},
          freshly_updated{false} {}

    int outstanding;
    uint64_t value;
    int consecutive_updates; 
    int failed_attempts; 
    int loop_modulo;  
    bool freshly_updated;  
  };

 private:
 /*D'après cette fonction, le leader est celui dont l'id est le plus petit ET avec plus de 2 consecutive_updates
 Le nombre de consecutive updates est le "score"*/
  int leader_pid() {
    int leader_id = -1;

    for (auto &pid : ids) {
      // std::cout << pid << " " << status[pid].consecutive_updates <<
      // std::endl;
      if (status[pid].consecutive_updates > 2) {
        leader_id = pid;
        break;
      }
    }

    return leader_id;
  }

  LeaderContext *ctx;
  std::atomic<bool> want_leader;  

  std::map<int, ReliableConnection> *rcs;
  ReliableConnection *loopback;

  uint64_t read_seq;

  uint64_t post_id;
  std::vector<uint64_t> post_ids;
  std::set<int> outstanding_pids;

  PollingContext heartbeat_poller;
  std::vector<ReadingStatus> status;

  ptrdiff_t offset;
  std::vector<uint8_t *> slots;
  std::vector<struct ibv_wc> entries;

  std::vector<int> ids;
  int max_id;
  int my_id;

  uint64_t *counter_from;
};
}  // namespace dory

namespace dory {
class LeaderPermissionAsker {
 public:
  LeaderPermissionAsker() {}
  LeaderPermissionAsker(LeaderContext *ctx)
      : ctx{ctx},
        c_ctx{&ctx->cc},
        scratchpad{&ctx->scratchpad},
        req_nr(c_ctx->my_id),
        grant_req_id{1} {
    auto quorum_size = c_ctx->remote_ids.size();
    modulo = Identifiers::maxID(c_ctx->my_id, c_ctx->remote_ids);

    // TODO:
    // We assume that these writes can never fail
    SequentialQuorumWaiter waiterLeaderWrite(quorum::LeaderReqWr,
                                             c_ctx->remote_ids, quorum_size, 1);
    leaderWriter = MajorityWriter(c_ctx, waiterLeaderWrite, c_ctx->remote_ids, 0);

    auto remote_slot_offset = scratchpad->writeLeaderChangeSlotsOffsets()[c_ctx->my_id];
    remote_mem_locations.resize(Identifiers::maxID(c_ctx->remote_ids) + 1);
    std::fill(remote_mem_locations.begin(), remote_mem_locations.end(),
              remote_slot_offset);
  }

  void startPoller() {
    ctx->poller.registerContext(quorum::LeaderReqWr);
    ctx->poller.registerContext(quorum::LeaderGrantWr);
    ctx->poller.endRegistrations(4);

    ask_perm_poller = ctx->poller.getContext(quorum::LeaderReqWr);
    give_perm_poller = ctx->poller.getContext(quorum::LeaderGrantWr);
  }

  // TODO: Refactor
  std::unique_ptr<MaybeError> givePermissionStep1(int pid, uint64_t response) {
    return givePermission(pid, response);
  }

  std::unique_ptr<MaybeError> givePermissionStep2(int pid, uint64_t response) {
    return givePermission(pid, response + modulo);
  }

  std::unique_ptr<MaybeError> givePermission(int pid, uint64_t response) {
    auto &offsets = scratchpad->readLeaderChangeSlotsOffsets();
    auto offset = offsets[c_ctx->my_id];

    auto &rcs = c_ctx->ce.connections();
    auto rc_it = rcs.find(pid);
    if (rc_it == rcs.end()) {
      throw std::runtime_error("Bug: connection does not exist");
    }

    uint64_t *temp = reinterpret_cast<uint64_t *>(scratchpad->leaderResponseSlot());
    *temp = response;

    auto &rc = rc_it->second;
    rc.postSendSingle(ReliableConnection::RdmaWrite,
                      quorum::pack(quorum::LeaderGrantWr, pid, grant_req_id),
                      temp, sizeof(temp), rc.remoteBuf() + offset);

    grant_req_id += 1;

    int expected_nr = 1;

    while (true) {
      entries.resize(expected_nr);
      if (give_perm_poller(c_ctx->cq, entries)) {
        // if (c_ctx->cb.pollCqIsOK(c_ctx->cq, entries)) {
        for (auto const &entry : entries) {
          auto [reply_k, reply_pid, reply_seq] =
              quorum::unpackAll<uint64_t, uint64_t>(entry.wr_id);

          if (reply_k != quorum::LeaderGrantWr || reply_pid != uint64_t(pid) ||
              reply_seq != (grant_req_id - 1)) {
            continue;
          }

          if (entry.status != IBV_WC_SUCCESS) {
            throw std::runtime_error(
                "Unimplemented: We assume the leader election connections never fail");
          } else {
            //std::cout << "RDMA Write sucessful : answer to permission request to pid = " << pid << std::endl;
            return std::make_unique<NoError>();
          }
        }
      } else {
        std::cout << "Poll returned an error" << std::endl;
      }
    }

    return std::make_unique<NoError>();
  }

  bool waitForApprovalStep1(Leader current_leader,
                            std::atomic<Leader> &leader) {
    auto &slots = scratchpad->readLeaderChangeSlots();
    auto ids = c_ctx->remote_ids;
    auto constexpr shift = 8 * sizeof(uintptr_t) - 1;

    // TIMESTAMP_T start, end;
    // GET_TIMESTAMP(start);
    // uint64_t sec = 1000000000UL;

    while (true) {
      int eliminated_one = -1;
      for (int i = 0; i < static_cast<int>(ids.size()); i++) {
        auto pid = ids[i];
        uint64_t volatile *temp = reinterpret_cast<uint64_t *>(slots[pid]);
        uint64_t val = *temp;
        val &= (1UL << shift) - 1;

        if (val + 2 * modulo == req_nr || val + modulo == req_nr) {
          eliminated_one = i;
          // std::cout << "Eliminating " << pid << std::endl;
          break;
        }
      }

      if (eliminated_one >= 0) {
        ids[eliminated_one] = ids[ids.size() - 1];
        ids.pop_back();

        if (ids.empty()) {
          return true;
        }
      }


      if (leader.load().requester != current_leader.requester) {
        return false;
      }
    }
  }

  bool waitForApprovalStep2(Leader current_leader,
                            std::atomic<Leader> &leader) {
    auto &slots = scratchpad->readLeaderChangeSlots();
    auto ids = c_ctx->remote_ids;
    auto constexpr shift = 8 * sizeof(uintptr_t) - 1;

    // TIMESTAMP_T start, end;
    // GET_TIMESTAMP(start);
    // uint64_t sec = 1000000000UL;

    while (true) {
      int eliminated_one = -1;
      for (int i = 0; i < static_cast<int>(ids.size()); i++) {
        auto pid = ids[i];
        uint64_t volatile *temp = reinterpret_cast<uint64_t *>(slots[pid]);
        uint64_t val = *temp;
        val &= (1UL << shift) - 1;

        // std::cout << "(" << val << ", " << pid << ")" << std::endl;

        if (val + modulo == req_nr) {
          eliminated_one = i;
          // std::cout << "Eliminating " << pid << std::endl;
          break;
        }
      }

      if (eliminated_one >= 0) {
        ids[eliminated_one] = ids[ids.size() - 1];
        ids.pop_back();

        if (ids.empty()) {
          return true;
        }
      }

      if (leader.load().requester != current_leader.requester) {
        return false;
      }
    }
  }

  std::unique_ptr<MaybeError> askForPermissions(bool hard_reset = false) {
    uint64_t *temp =  reinterpret_cast<uint64_t *>(scratchpad->leaderRequestSlot());
    if (hard_reset) {
      *temp = (1UL << 63) | req_nr;
    } else {
      *temp = req_nr;
    }

    std::cout << "AskForPermissions_Write" << std::endl;
    // Wait for the request to reach all followers
    auto err = leaderWriter.write(temp, sizeof(req_nr), remote_mem_locations,
                                  ask_perm_poller);
    std::cout << "AskForPermissions_Write Done" << std::endl;

    if (!err->ok()) {
      std::cout << "The AskForPermissions failed" << std::endl;
      return err;
    }

    req_nr += 2 * modulo;

    return std::make_unique<NoError>();
  }

  inline uint64_t requestNr() const { return req_nr; }

 private:
  LeaderContext *ctx;
  ConnectionContext *c_ctx;
  ScratchpadMemory *scratchpad;
  uint64_t req_nr;
  uint64_t grant_req_id;

  using MajorityWriter = FixedSizeMajorityOperation<SequentialQuorumWaiter,
                                                    LeaderSwitchRequestError>;
  MajorityWriter leaderWriter;

  std::vector<uintptr_t> remote_mem_locations;

  int modulo;
  std::vector<struct ibv_wc> entries;
  PollingContext ask_perm_poller;
  PollingContext give_perm_poller;
};
}  // namespace dory

namespace dory {
class LeaderSwitcher {
 public:
  LeaderSwitcher() : read_slots{dummy} {}

  LeaderSwitcher(LeaderContext *ctx, LeaderHeartbeat *heartbeat)
      : ctx{ctx},
        c_ctx{&ctx->cc},
        want_leader{&heartbeat->wantLeaderSignal()}, 
        read_slots{ctx->scratchpad.writeLeaderChangeSlots()},
        sz{read_slots.size()},
        permission_asker{ctx} {
    prepareScanner();
  }

  void startPoller() { permission_asker.startPoller(); }


  //tourne en boucle dans le thread Switcher
  void scanPermissions() {
    // Scan the memory for new messages
    int requester = -1;
    int force_reset = 0;
    auto constexpr shift = 8 * sizeof(uintptr_t) - 1;

    for (int i = 0; i < static_cast<int>(sz); i++) {
      reading[i] = *reinterpret_cast<uint64_t *>(read_slots[i]);
      force_reset = static_cast<int>(reading[i] >> shift); //Conserve le premier bit. C'est lui qui informe si le nouveau leader veut forcer un reset ou non
      reading[i] &= (1UL << shift) - 1;   //mets le premier bit à 0


      //si on découvre une demande
      if (reading[i] > current_reading[i]) { //donc la permission se fait en écrivant une valeur plus grande dans le tableau 
        current_reading[i] = reading[i]; 
        requester = i;
        break;
      }
    }

    // If you discovered a new request for a leader, notify the main event loop 
    // to give permissions to him and switch to follower.
    //Rappel : dans Mu, dès qu'un noeud demande les droits, on le lui donne 
    if (requester > 0) {
      std::cout << "Process with pid " << requester << " asked for permissions" << std::endl;
      leader.store(dory::Leader(requester, reading[requester], force_reset)); //la thread consensus, qui appelle en boucl checkAndApplyPermissions, regarde ça
      //c'est à travers ce "leader" que l'on apprend ce qui se passe
      want_leader->store(false); //si quelqu'un veut être leader, alors je ne le veux plus 
    } else {
      // Check if my leader election declared me as leader 
      //(on scanne tout le tableau pour les requêtes des autres, maintenant on vérifie son propre besoin)
      if (want_leader->load()) {
        auto expected = leader.load();
        if (expected.unused()) { // on vérifie si on a pas déjà donné les droits à ce leader 
          dory::Leader desired(c_ctx->my_id, permission_asker.requestNr());  
          auto ret = leader.compare_exchange_strong(expected, desired); //atomic compare and swap 
          //will update leader to the new value (desired) if it matches the expected value (expected)
          //won't be bothered by other threads while doing it 
          if (ret) {
            //std::cout << "Process " << c_ctx->my_id << " (which is this node) wants to become leader"<< std::endl;
            want_leader->store(false);
          }
        }
      }
    }
  }


  //est appelé par la thread consensus
  bool checkAndApplyPermissions(std::map<int, ReliableConnection> *replicator_rcs, Follower &follower,
      std::atomic<bool> &leader_mode, bool &force_permission_request) {
    Leader current_leader = leader.load();
    
    if (current_leader != prev_leader || force_permission_request) {
      //si force_permission_request est true, alors on va mettre à jour le leader, même si c'est toujours le meme noeud
      //surtout, on va faire un hard reset 

      //maj leader
      auto orig_leader = prev_leader;
      prev_leader = current_leader;


      //force le reset si c'est demandé 
      bool hard_reset = force_permission_request;
      force_permission_request = false;


      if (current_leader.requester == c_ctx->my_id) { //si c'est moi le 'nouveau' leader 
        if (!leader_mode.load()) { //et que je ne l'étais pas avant 
          permission_asker.askForPermissions();
         
          // In order to avoid a distributed deadlock (when two processes try
          // to become leaders at the same time), we bail whe the leader
          // changes.
          if (!permission_asker.waitForApprovalStep1(current_leader, leader)) {
            force_permission_request = true;
            return false;
          };

          auto expected = current_leader;
          auto desired = expected;
          desired.makeUnused();
          leader.compare_exchange_strong(expected, desired);

          if (hard_reset) {
            // Reset everybody
            //std::cout << "hard_reset asked by myself ==> soft reset "<<std::endl;
            /*
            for (auto &[pid, rc] : *replicator_rcs) {
              IGNORE(pid);
              rc.reset();
            }
            // Re-configure the connections
            for (auto &[pid, rc] : *replicator_rcs) {
              IGNORE(pid);
              rc.init(ControlBlock::LOCAL_READ | ControlBlock::LOCAL_WRITE);
              rc.reconnect();
            }*/

            
            //soft reset
            for (auto &[pid, rc] : *replicator_rcs) {
              IGNORE(pid);
              //std::cout << "calling change right on a replicator rc, to revoke its rights (soft reset triggered by myself)" << std::endl;
              rc.changeRights(ControlBlock::LOCAL_READ | ControlBlock::LOCAL_WRITE);
            }
          } else if (orig_leader.requester != c_ctx->my_id) {
            // If I am going from follower to leader, then I need to revoke
            // write permissions to old leader. Otherwise, I do nothing.
            auto old_leader = replicator_rcs->find(orig_leader.requester);
            if (old_leader != replicator_rcs->end()) {
              auto &rc = old_leader->second;
              auto rights = ControlBlock::LOCAL_READ | ControlBlock::LOCAL_WRITE;
              //std::cout << "I'm the new leader ==> reset of the right rc (former leader)" << std::endl;
              if (!rc.changeRights(rights)) {
                //std::cout << "changeRights() failed, calling the big guns "<<std::endl;
                throw std::runtime_error("changeRights failed or Hard set ==> stop !");
                rc.reset();
                rc.init(rights);
                rc.reconnect();
              }
            }
          }

          follower.block();

          if (!permission_asker.waitForApprovalStep2(current_leader, leader)) {
            force_permission_request = true;
            return false;
          };

          leader_mode.store(true);

          std::cout << "Permissions granted" << std::endl;
        } else {/*si c'est moi le 'nouveau' leader, mais que je l'étais déjà avant, alors on ne fait rien */}
      } else { //si un nouveau leader autre que moi se manifeste
        leader_mode.store(false); //j'informe la thread consensus que je ne suis pas le leader 
  
        if (current_leader.reset()) { //si le leader (remote) a demandé un reset
          // Hard reset every connection
          // Reset everybody
          std::cout << "Hard-reset asked by a remote leader" << std::endl;
      
          for (auto &[pid, rc] : *replicator_rcs) {
            IGNORE(pid);
            rc.reset();
          }
          // Re-configure the connections
          for (auto &[pid, rc] : *replicator_rcs) {
            if (pid == current_leader.requester) {
              rc.init(ControlBlock::LOCAL_READ | ControlBlock::LOCAL_WRITE |
                      ControlBlock::REMOTE_READ | ControlBlock::REMOTE_WRITE);
            } else {
              rc.init(ControlBlock::LOCAL_READ | ControlBlock::LOCAL_WRITE);
            }
            rc.reconnect();
          }
        } else { //sinon, on lui donne les permissions normalement 
          // Notify the remote party
          permission_asker.givePermissionStep1(current_leader.requester,
                                               current_leader.requester_value);

          // First revoke from old leader
          auto old_leader = replicator_rcs->find(orig_leader.requester);
          if (old_leader != replicator_rcs->end()) {
            auto &rc = old_leader->second;
            auto rights = ControlBlock::LOCAL_READ | ControlBlock::LOCAL_WRITE;
            //std::cout << "changeRights() to revoke former leader's rights" << std::endl; 
            if (!rc.changeRights(rights)) {
              //std::cout << "changeRights() failed (to revoke former leader), calling the big guns (ce qui va provoquer une erreur) "<<std::endl;
              throw std::runtime_error("changeRights failed or Hard set ==> stop !");
              rc.reset();
              rc.init(rights);
              rc.reconnect();
            }
          }

          // Then grant to new leader
          auto new_leader = replicator_rcs->find(current_leader.requester);
          if (new_leader != replicator_rcs->end()) {
            auto &rc = new_leader->second;
            auto rights = ControlBlock::LOCAL_READ | ControlBlock::LOCAL_WRITE |
                          ControlBlock::REMOTE_READ |
                          ControlBlock::REMOTE_WRITE;
            //std::cout << "changeRights() is called to grant new rights to : "<< current_leader.requester << std::endl;
            if (!rc.changeRights(rights)) {
              //std::cout << "changeRights() failed (to grant new rights), calling the big guns  (which will cause an error)"<<std::endl;
              
              throw std::runtime_error("changeRights failed or Hard set ==> stop !");
              rc.reset();
              rc.init(rights);
              rc.reconnect();
            }
          }
        }

        permission_asker.givePermissionStep2(current_leader.requester,
                                             current_leader.requester_value);

        follower.unblock();
        

        std::cout << "Giving permissions to " << int(current_leader.requester) << std::endl;
        auto expected = current_leader;
        auto desired = expected;
        desired.makeUnused();
        leader.compare_exchange_strong(expected, desired);  
        /*On peut pas directement faire current_leader.makeUnused(), car il peut y avoir des appels concurrents 
        venant de la thread switcher. Du coup, on passe par une copie, qu'on modifie, et ensuite on fait un cas atomic 
        (qui fonctionne ssi il n'y a pas eu de modification du leader venant de l'autre thread)
        Si ça échoue, c'est qu'il y a un nouveau leader, et donc le "makeUnused()" ne sert plus à rien 
        (makeUnused, c'est pour éviter, plus tard, de redonner les droits à un leader qui les a déjà) */
      }
    }
    return true;
  }

  std::atomic<Leader> &leaderSignal() { return leader; }

  // Move assignment operator
  LeaderSwitcher &operator=(LeaderSwitcher &&o) {
    if (&o == this) {
      return *this;
    }

    ctx = o.ctx;
    o.ctx = nullptr;
    c_ctx = o.c_ctx;
    o.c_ctx = nullptr;
    want_leader = o.want_leader;
    o.want_leader = nullptr;
    prev_leader = o.prev_leader;
    leader.store(o.leader.load());
    dummy = o.dummy;
    read_slots = o.read_slots;
    sz = o.sz;
    permission_asker = o.permission_asker;
    current_reading = o.current_reading;
    reading = o.reading;
    return *this;
  }

 private:
  void prepareScanner() {
    current_reading.resize(sz);

    auto constexpr shift = 8 * sizeof(uintptr_t) - 1; //nb de bits de uintptr_t -1 
    for (size_t i = 0; i < sz; i++) {
      current_reading[i] = *reinterpret_cast<uint64_t *>(read_slots[i]); //va chercher les valeurs de read_slot et les mettre dans current_reading
      current_reading[i] &= (1UL << shift) - 1;   // mets le premier bit à 0 
    }

    reading.resize(sz);
  }

 private:
  LeaderContext *ctx;
  ConnectionContext *c_ctx;
  std::atomic<bool> *want_leader;
  Leader prev_leader;
  std::atomic<Leader> leader;

  std::vector<uint8_t *> dummy;
  std::vector<uint8_t *> &read_slots;
  size_t sz;

  LeaderPermissionAsker permission_asker;

  std::vector<uint64_t> current_reading;
  std::vector<uint64_t> reading;
};
}  // namespace dory

namespace dory {
class LeaderElection {
 public:
  LeaderElection(ConnectionContext &cc, ScratchpadMemory &scratchpad,
                 ConsensusConfig::ThreadConfig threadConfig)
      : ctx{cc, scratchpad},
        threadConfig{threadConfig},
        hb_started{false},
        switcher_started{false},
        response_blocked{false} {
    startHeartbeat();       //lance une thread
    startLeaderSwitcher();  //lancer une thread
  }

  LeaderContext *context() { return &ctx; }

  ~LeaderElection() {
    stopLeaderSwitcher();
    stopHeartbreat();
  }

  void attachReplicatorContext(ReplicationContext *replicator_ctx) {
    auto &ref = replicator_ctx->cc.ce.connections();
    replicator_conns = &ref;
  }

  inline bool checkAndApplyConnectionPermissionsOK(
      Follower &follower, std::atomic<bool> &leader_mode,
      bool &force_permission_request) {
    return leader_switcher.checkAndApplyPermissions(
        replicator_conns, follower, leader_mode, force_permission_request);
  }

  inline std::atomic<Leader> &leaderSignal() {
    return leader_switcher.leaderSignal();
  }

 private:
  void startHeartbeat() {
    if (hb_started) {
      throw std::runtime_error("Already started");
    }
    hb_started = true;

    leader_heartbeat = LeaderHeartbeat(&ctx);
    std::future<void> ftr = hb_exit_signal.get_future();
    heartbeat_thd = std::thread([this, ftr = std::move(ftr)]() {
      leader_heartbeat.startPoller();

      std::string fifo("/tmp/fifo-" + std::to_string(ctx.cc.my_id));
      if (unlink(fifo.c_str())) {
        if (errno != ENOENT) {
          throw std::runtime_error("Could not delete the fifo: " + std::string(std::strerror(errno)));
        }
      }
      
      // Create a named pipe with read and write permissions for all users (0666)
      if (mkfifo(fifo.c_str(), 0666)) {
        throw std::runtime_error("Could not create the fifo: " + std::string(std::strerror(errno)));
      }

      int fd = open(fifo.c_str(), O_RDWR);
      if (fd == -1) {
        throw std::runtime_error("Could not open the fifo: " +
                                 std::string(std::strerror(errno)));
      }

      std::atomic<char> command{'c'};  // 'p' for pause, 'c' for continue
      char prev_command = 'c';

      auto file_watcher_thd = std::thread([&command, &fd]() {
        while (true) {
          char tmp;
          int ret = static_cast<int>(read(fd, &tmp, 1));
          if (ret == -1) {
            // if (errno != EAGAIN) {
            throw std::runtime_error("Could not read from the fifo: " +
                                     std::string(std::strerror(errno)));
            // }
          } else if (ret == 1) {
            std::cout <<"[FILEWATCHER] Storing in command : " << tmp << std::endl;
            command.store(tmp);
          }
        }
      });

      if (threadConfig.pinThreads) {
        pinThreadToCore(file_watcher_thd, threadConfig.fileWatcherThreadCoreID);
      }

      if (ConsensusConfig::nameThreads) {
        setThreadName(file_watcher_thd, ConsensusConfig::fileWatcherThreadName);
      }

      for (unsigned long long i = 0;; i = (i + 1) & iterations_ftr_check) {
        char current_command = command.load();
        //std::cout <<"Previous command :" << prev_command << "; Current command : " << current_command << std::endl;
        if (current_command == 'c') {
          response_blocked.store(false);
          leader_heartbeat.scanHeartbeats(); //important !
        } else if (prev_command == 'c') {
          response_blocked.store(true);
          leader_heartbeat.retract();
        }

        prev_command = current_command;

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      file_watcher_thd.join();
    });

    if (threadConfig.pinThreads) {
      pinThreadToCore(heartbeat_thd, threadConfig.heartbeatThreadCoreID);
    }

    if (ConsensusConfig::nameThreads) {
      setThreadName(heartbeat_thd, ConsensusConfig::heartbeatThreadName);
    }
  }

  void stopHeartbreat() {
    std::cout <<"stopHeartbeat() called" << std::endl;
    if (hb_started) {
      hb_exit_signal.set_value();
      heartbeat_thd.join();
      hb_started = false;
    }
  }

  void startLeaderSwitcher() {
    if (switcher_started) {
      throw std::runtime_error("Already started");
    }
    switcher_started = true;
    //std::cout << "Starting leader switcher thread ! " << std::endl;

    leader_switcher = LeaderSwitcher(&ctx, &leader_heartbeat);
    std::future<void> ftr = switcher_exit_signal.get_future();  //permet de gérer des évènements de manière asynchrone 
    switcher_thd = std::thread([this, ftr = std::move(ftr)]() {
      leader_switcher.startPoller();
      
      for (unsigned long long i = 0;; i = (i + 1) & iterations_ftr_check) {
        leader_switcher.scanPermissions();
        if (i == 0) {
          //vérifie le résultat sans bloquer 
          if (ftr.wait_for(std::chrono::seconds(0)) != std::future_status::timeout) { 
            //std::cout << "the leader switcher thread is about the exit " << std::endl;
            break; //sort totalement de la boucle ?? (pk pas juste "continue" ? )
            //ça veut dire que si le résultat n'est pas disponible, on quitte 
          }
        }
      }

    });

    if (threadConfig.pinThreads) {
      pinThreadToCore(switcher_thd, threadConfig.switcherThreadCoreID);
    }

    if (ConsensusConfig::nameThreads) {
      setThreadName(switcher_thd, ConsensusConfig::switcherThreadName);
    }
  }

  void stopLeaderSwitcher() {
    if (switcher_started) {
      switcher_exit_signal.set_value();
      switcher_thd.join();
      switcher_started = false;
    }
  }

 private:
  // Must be power of 2 minus 1
  static constexpr unsigned long long iterations_ftr_check = (2 >> 13) - 1;
  LeaderContext ctx; 
  ConsensusConfig::ThreadConfig threadConfig;
  std::map<int, ReliableConnection> *replicator_conns; //la connexion du replication plane 

  // For heartbeat thread
  LeaderHeartbeat leader_heartbeat;
  std::thread heartbeat_thd;
  bool hb_started;
  std::promise<void> hb_exit_signal;

  // For the leader switcher thread
  LeaderSwitcher leader_switcher;
  std::thread switcher_thd;
  bool switcher_started;
  std::promise<void> switcher_exit_signal;

 public:
  std::atomic<bool> response_blocked;
};
}  // namespace dory
