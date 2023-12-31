#pragma once

#include <vector>

#include "branching.hpp"
#include "context.hpp"
#include "error.hpp"
#include "quorum-waiter.hpp"
#include "timers.h"

#define MODULO (1UL * 1024 *1024 *1024)


namespace dory {
template <class QuorumWaiter, class ErrorType> class FixedSizeMajorityOperation {
 public:
  FixedSizeMajorityOperation() {}
  FixedSizeMajorityOperation(ConnectionContext *context, QuorumWaiter qw,
                             std::vector<int> &remote_ids)
      : ctx{context}, qw{qw}, kind{qw.kindOfOp()} {
    quorum_size =  static_cast<int>(quorum::majority(ctx->remote_ids.size() + 1)) - 1;
    replicas_size = static_cast<int>(ctx->remote_ids.size());

    successful_ops.resize(remote_ids.size());
    successful_ops.clear();

    int tolerated_failures = static_cast<int>(quorum::minority(ctx->remote_ids.size() + 1)); 
    failed_majority = FailureTracker(kind, ctx->remote_ids, tolerated_failures);
    failed_majority.track(qw.reqID()); //ça veut dire qu'on traque les failures à partir de maitenant 

    auto &rcs = ctx->ce.connections();
    for (auto &[pid, rc] : rcs) {
      if (std::find(remote_ids.begin(), remote_ids.end(), pid) !=  remote_ids.end()) {
        connections.push_back(Conn(pid, &rc));
      }
    }
  }

  FixedSizeMajorityOperation(ConnectionContext *context, QuorumWaiter qw,
                             std::vector<int> &remote_ids,
                             size_t tolerated_failures)
      : ctx{context}, qw{qw}, kind{qw.kindOfOp()} {
    successful_ops.resize(remote_ids.size());
    successful_ops.clear();

    failed_majority = FailureTracker(kind, ctx->remote_ids,
                                     static_cast<int>(tolerated_failures));
    failed_majority.track(qw.reqID());

    auto &rcs = ctx->ce.connections();
    for (auto &[pid, rc] : rcs) {
      if (std::find(remote_ids.begin(), remote_ids.end(), pid) !=
          remote_ids.end()) {
        connections.push_back(Conn(pid, &rc));
      }
    }
  }

  typename QuorumWaiter::ReqIDType reqID() { return qw.reqID(); }

  //Reset le failure traquer et le quorum waiter
  void recoverFromError(std::unique_ptr<MaybeError> &supplied_error) {
    if (supplied_error->type() == ErrorType::value) {
      ErrorType &error = *dynamic_cast<ErrorType *>(supplied_error.get());

      auto req_id = error.req();

      failed_majority.reset();
      failed_majority.track(req_id);
      qw.reset(req_id);

      // TODO (question):
      // To reuse the same req_id, doe we need to make sure no outstanding
      // requests are on the wire?
    } else {
      throw std::runtime_error("Unimplemented handing of this error");
    }
  }


  //operations with a leader : 

  std::unique_ptr<MaybeError> read(std::vector<void *> &to_local_memories,
                                   size_t size, //si on ne fournit qu'une seule taille, alors on suppose que tous les emplacements sont de cette taille
                                   std::vector<uintptr_t> &from_remote_memories,
                                   std::atomic<Leader> &leader) {
    std::vector<size_t> size_v(from_remote_memories.size());
    std::fill(size_v.begin(), size_v.end(), size);
    return op_with_leader_bail(ReliableConnection::RdmaRead, to_local_memories,
                               size_v, from_remote_memories, leader);
  }

  std::unique_ptr<MaybeError> read(std::vector<void *> &to_local_memories,
                                   std::vector<size_t> &size,
                                   std::vector<uintptr_t> &from_remote_memories,
                                   std::atomic<Leader> &leader) {
    return op_with_leader_bail(ReliableConnection::RdmaRead, to_local_memories,
                               size, from_remote_memories, leader);
  }

  std::unique_ptr<MaybeError> write(void *from_local_memory, 
                                    size_t size, //même chose
                                    std::vector<uintptr_t> &to_remote_memories,
                                    std::atomic<Leader> &leader) {
    std::vector<size_t> size_v(to_remote_memories.size());
    std::fill(size_v.begin(), size_v.end(), size);
    return op_with_leader_bail(ReliableConnection::RdmaWrite, from_local_memory,
                               size_v, to_remote_memories, leader);
  }

  std::unique_ptr<MaybeError> write(void *from_local_memory,
                                    std::vector<size_t> &size,
                                    std::vector<uintptr_t> &to_remote_memories,
                                    std::atomic<Leader> &leader) {
    return op_with_leader_bail(ReliableConnection::RdmaWrite, from_local_memory,
                               size, to_remote_memories, leader);
  }

  std::unique_ptr<MaybeError> write(std::vector<void *> &from_local_memories,
                                    std::vector<size_t> &size, 
                                    std::vector<uintptr_t> &to_remote_memories,
                                    std::atomic<Leader> &leader) {
    return op_with_leader_bail(ReliableConnection::RdmaWrite,
                               from_local_memories, size, to_remote_memories,
                               leader);
  }


//operations without a leader : 
  template <typename Poller>  std::unique_ptr<MaybeError> read(std::vector<void *> &to_local_memories,
                                                                size_t size,
                                                                std::vector<uintptr_t> &from_remote_memories,
                                                                Poller p) {
    return op_without_leader(to_local_memories, size, from_remote_memories, p);
  }

  //si on ne précise pas le poller
  std::unique_ptr<MaybeError> read(std::vector<void *> &to_local_memories, 
                                    size_t size,
                                    std::vector<uintptr_t> &from_remote_memories) {
    return op_without_leader(to_local_memories, size, from_remote_memories,ctx->cb.pollCqIsOK);
  }

  template <typename Poller> std::unique_ptr<MaybeError> write(void *from_local_memory, size_t size,
                                                                std::vector<uintptr_t> &to_remote_memories,
                                                                Poller p) {
    return op_without_leader(from_local_memory, size, to_remote_memories, p);
  }

  std::unique_ptr<MaybeError> write(void *from_local_memory, 
                                      size_t size,
                                      std::vector<uintptr_t> &to_remote_memories) {
    return op_without_leader(from_local_memory, size, to_remote_memories,
                             ctx->cb.pollCqIsOK);
  }



  //fastWrite (avec un leader)
  bool fastWrite(void *from_local_memory, size_t size,
                 std::vector<uintptr_t> &to_remote_memories, uintptr_t offset,
                 std::atomic<Leader> &leader, int outstanding_req, 
                 bool use_tofino) {
    IGNORE(leader);

    auto req_id = qw.fetchAndIncFastID();
    auto next_req_id = qw.nextFastReqID();

    if (use_tofino){
      //posting a single WR to the QP connected to the tofino
      //std::cout << "Posting to all through byp4ss" << std::endl;
      auto& tofino_rc = ctx->ce.getTofinoRC();
      auto ok = tofino_rc.postSendSingle(
          ReliableConnection::RdmaWrite,
          QuorumWaiter::packer( quorum::TofinoWr, 2, req_id), from_local_memory,     //comme pour le moment seulement le noeud 1 utilise la fastWrite() avec tofino, l'id 2 est toujours là
          static_cast<uint32_t>(size), //si on enlève le hardcodage, alors il faut utiliser le pid associé à rc_tofino (dans CE)
          tofino_rc.remoteBuf() + to_remote_memories[2] + offset, false);
          //tofino_rc.remoteBuf() + to_remote_memories[2] + 0. , false);

      //std::cout << "pids " << connections[0].pid << " " << connections[1].pid << " offsets " << to_remote_memories[connections[0].pid]  << " " << to_remote_memories[connections[1].pid] 
        //       << " we used " << to_remote_memories[2] << " offset " << offset << "\n";
      if (!ok) {
        throw std::runtime_error("Posting to tofino_rc failed failed");
      }

    }
    else{
    //posting the WR to the QPs
    for (auto &c : connections) {
      //std::cout << "Posting to "<< c.pid << " by hand " << std::endl;
      auto ok = c.rc->postSendSingle(
          ReliableConnection::RdmaWrite,
          QuorumWaiter::packer(kind, c.pid, req_id), from_local_memory,
          static_cast<uint32_t>(size),
          c.rc->remoteBuf() + to_remote_memories[c.pid] + offset, false);  
      //std::cout << "pids " << connections[0].pid << " " << connections[1].pid << " offsets " << to_remote_memories[connections[0].pid]  << " " << to_remote_memories[connections[1].pid] 
      //     << " we used " << to_remote_memories[2] << " offset " << offset << "\n";
      
      if (!ok) {
        throw std::runtime_error("Posting to rc for fastWrite failed failed");
        return false;
      }

    }
    }

    //if we use the tofino, we don't need to pull as many WC to continue forward
    int expected_nr = use_tofino ? outstanding_req+1 : outstanding_req * replicas_size + quorum_size; 
    auto cq = ctx->cq.get();
    entries.resize(expected_nr);
    int num = 0;
    
    //if we can no longer send operations, than we process the WC received until the QW gives us a green light
    while (!qw.canContinueWithOutstanding(outstanding_req, next_req_id)) {
      num = ibv_poll_cq(cq, expected_nr, &entries[0]);
      
      if (num >= 0) {
        if (!qw.fastConsume(entries, num, expected_nr)) {
          std::cout << "Fast Consume failed "<< std::endl;
          return false;
        }
      } else {
        std::cout << "Polled negative value in fastWrite==> failed "<< std::endl;
        return false;
      }
    }

    //même problème que dans le op_with_leader_bail :
    //il peut y avoir eu un changement de leader pendant que l'on poll 
    //ce qui causerait la perte de certaines requêtes
    //donc on vérifie de temps en temps que le leader n'a pas changé
    //(par contre, ce n'est pas dans le while. Peut-être que comme on attend pas souvent,
    // c'est plus pertinent de regarder entre deux séries d'envoies )
    constexpr unsigned mask = (1 << 14) - 1;  // Must be power of 2 minus 1
    int loops = 0;
    
    loops = (loops + 1) & mask;
    if (loops == 0) {
      auto ldr = leader.load();
      if (ldr.requester != ctx->my_id) {
        return false;
      }
    }
    range_start = req_id;
    range_end = qw.reqID();
        
    return true;
  }

  std::unique_ptr<MaybeError> fastWriteError() {
    auto req_id = qw.reqID();
    return std::make_unique<ErrorType>(req_id);
  }



  std::vector<int> &successes() { return successful_ops; }

  uint64_t latestReplicatedID() { return uint64_t(qw.reqID()); }

 private:
  template <class T>  std::unique_ptr<MaybeError> op_with_leader_bail( ReliableConnection::RdmaReq rdma_req, 
                                                                        T const &local_memory,
                                                                        std::vector<size_t> &size, std::vector<uintptr_t> &remote_memory,
                                                                        std::atomic<Leader> &leader) {
    successful_ops.clear();

    auto req_id = qw.reqID();
    auto next_req_id = qw.nextReqID();


    //on envoie post toutes les opérations 
    for (auto &c : connections) {
      auto pid = c.pid;
      auto &rc = *(c.rc);
      if constexpr (std::is_same_v<T, std::vector<void *>>) { 
        auto ok = rc.postSendSingle(
            rdma_req, QuorumWaiter::packer(kind, pid, req_id),
            local_memory[pid], static_cast<uint32_t>(size[pid]),
            rc.remoteBuf() + remote_memory[pid]);
        if (!ok) {
          return std::make_unique<ErrorType>(req_id);
        }
      } else {
        auto ok =
            rc.postSendSingle(rdma_req, QuorumWaiter::packer(kind, pid, req_id),
                              local_memory, static_cast<uint32_t>(size[pid]),
                              rc.remoteBuf() + remote_memory[pid]);
        if (!ok) {
          return std::make_unique<ErrorType>(req_id);
        }
      }
    }


    //on attend leur wc, grâce au quorum waiter
    int expected_nr = static_cast<int>(connections.size());
    int loops = 0;
    while (!qw.canContinueWith(next_req_id)) {
      entries.resize(expected_nr);
      if (ctx->cb.pollCqIsOK(ctx->cq, entries)) { //cherche les wc dans cq et les enregistre dans entries
        if (!qw.consume(entries, successful_ops)) { //maj de qw 
          if (failed_majority.isUnrecoverable(entries)) { 
            return std::make_unique<ErrorType>(req_id);
          }
        }
      } else {
        std::cout << "Poll returned an error" << std::endl;
        return std::make_unique<ErrorType>(req_id);
      }

      // Workaround: When leader changes, some poll events may get lost
      // (most likely due to a bug on the driver) and we are stuck in an
      // infinite loop.
      // Pour empêcher ça : on vérifie de temps en temps que c'est bien 
      //toujours moi le leader
      loops += 1;
      if (loops % 1024 == 0) {
        loops = 0;
        auto ldr = leader.load();
        if (ldr.requester != ctx->my_id) {
          return std::make_unique<ErrorType>(req_id);
        }
      }
    }

    qw.setFastReqID(next_req_id);
    return std::make_unique<NoError>();
  }



  //c'est presque la même chose que précédemment, mais sans la vérification en cas de changement de leader 
  template <class T, class Poller> std::unique_ptr<MaybeError> op_without_leader(T const &local_memory, 
                                                                                  size_t size, 
                                                                                  std::vector<uintptr_t> &remote_memory,
                                                                                  Poller poller) {
    successful_ops.clear();

    auto req_id = qw.reqID();
    auto next_req_id = qw.nextReqID();


    //on envoie toutes nos requêtes
    for (auto &c : connections) {
      auto pid = c.pid;
      auto &rc = *(c.rc);
      if constexpr (std::is_same_v<T, std::vector<void *>>) { //si local_memory est un vecteur, c'est qu'on demande un READ
        auto ok = rc.postSendSingle(
            ReliableConnection::RdmaRead,
            QuorumWaiter::packer(kind, pid, req_id), local_memory[pid],
            static_cast<uint32_t>(size), rc.remoteBuf() + remote_memory[pid]);
        if (!ok) {
          return std::make_unique<ErrorType>(req_id);
        }
      } else { //sinon, c'est un WRITE
        auto ok = rc.postSendSingle(ReliableConnection::RdmaWrite,
                                    QuorumWaiter::packer(kind, pid, req_id),
                                    local_memory, static_cast<uint32_t>(size),
                                    rc.remoteBuf() + remote_memory[pid]);
        if (!ok) {
          return std::make_unique<ErrorType>(req_id);
        }
      }
    }


    //on vérifie  qu'elles on bien fonctionnées grâce à un quorum waiter
    int expected_nr = static_cast<int>(connections.size());
    while (!qw.canContinueWith(next_req_id)) {
      entries.resize(expected_nr);
      if (poller(ctx->cq, entries)) {
        if (!qw.consume(entries, successful_ops)) {
          if (failed_majority.isUnrecoverable(entries)) { //quand est-ce qu'il est mis à jour ?
            return std::make_unique<ErrorType>(req_id);
          }
        }
      } else {
        std::cout << "Poll returned an error" << std::endl;
        return std::make_unique<ErrorType>(req_id);
      }
    }

    qw.setFastReqID(next_req_id);
    return std::make_unique<NoError>();
  }

 private:
 //couple (pid, rc)
  struct Conn {
    Conn(int pid, dory::ReliableConnection *rc) : pid{pid}, rc{rc} {}
    int pid;
    dory::ReliableConnection *rc;
  };

 private:
  ConnectionContext *ctx;

  QuorumWaiter qw;
  quorum::Kind kind;

  int quorum_size, replicas_size; 

  FailureTracker failed_majority;

  std::vector<struct ibv_wc> entries;
  std::vector<int> successful_ops;
  std::vector<Conn> connections; 

 public:
  uint64_t range_start = 0, range_end = 0;
};
}  // namespace dory
