
#include "quorum-waiter.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

#include "branching.hpp"
#include "message-identifier.hpp"

namespace dory {

template <class ID> SerialQuorumWaiter<ID>::SerialQuorumWaiter(quorum::Kind kind,
                                           std::vector<int>& remote_ids,
                                           size_t quorum_size, ID next_id,
                                           ID modulo)
    : kind{kind},
      quorum_size(static_cast<int>(quorum_size)),
      next_id{next_id},
      left(static_cast<int>(quorum_size)),
      modulo(modulo) {
  auto max_elem = Identifiers::maxID(remote_ids);
  scoreboard.resize(max_elem + 1);

  if (next_id == 0) {
    throw std::runtime_error("`next_id` must be positive");
  }

  for (auto& elem : scoreboard) {
    elem = next_id - modulo;
  }
}

template <class ID> void SerialQuorumWaiter<ID>::reset(ID next) {
  if (next == 0) {
    throw std::runtime_error("`next_id` must be positive");
  }

  for (auto& elem : scoreboard) {
    elem = next - modulo;
  }
  left = quorum_size; //added. (forgotten in source code ? )
  next_id = next;
}

template <class ID> bool SerialQuorumWaiter<ID>::consume(std::vector<struct ibv_wc>& entries,
                                     std::vector<int>& successful_ops) {
  auto ret = true;
  for (auto const& entry : entries) {
    if (entry.status != IBV_WC_SUCCESS) {
      ret = false;
    } else {
      auto [k, pid, seq] = quorum::unpackAll<int, ID>(entry.wr_id);

      if (k != kind) {
/*#ifndef NDEBUG
        std::cout << "Received unexpected (" << quorum::type_str(k)
                  << " instead of " << quorum::type_str(kind) << ")"
                  << " message from the completion queue, concerning process "
                  << pid << std::endl;
#endif*/

        continue;
      }
/*
#ifndef NDEBUG
      if (seq != next_id) {
        std::cout << "Received remnant (" << seq << " instead of " << next_id
                  << ") message from the completion queue, concerning process "
                  << pid << std::endl;
      }
#endif*/
      
      //on vérifie que le seq reçu est celui qui suit immédiatement celui enregistré 
      //(pour être sûr de ne pas en sauter un)
      auto current_seq = scoreboard[pid];
      scoreboard[pid] = current_seq + modulo == seq ? seq : 0;  //modulo c'est l'écart entre deux seq 
      

      if (scoreboard[pid] == next_id) { 
        left -= 1;
        successful_ops.push_back(pid);
      }

      //si on a fini d'attendre tous les noeuds pour cette étape, on passe à la suivante
      if (left == 0) { 
        left = quorum_size;
        next_id += modulo;    
      }
    }
  }

  return ret;
}

template <class ID> bool SerialQuorumWaiter<ID>::fastConsume(std::vector<struct ibv_wc>& entries,
                                         int num, int& ret_left) {
  for (int i = 0; i < num; i++) {
    auto& entry = entries[i];

    //std::cout << "The status is " << ibv_wc_status_str(entry.status)  << std::endl;
    if (entry.status != IBV_WC_SUCCESS) {
      std::cout << "In fastConsume, not IBV_WC_SUCCESS for the entry, instead :  " << ibv_wc_status_str(entry.status) << std::endl;
      return false;
    } else {
      auto [k, pid, seq] = quorum::unpackAll<int, ID>(entry.wr_id);

      /*std::cout << "In fastConsume(), we just processed : "
          <<"[" << quorum::type_str(k)    << "," << pid << "," << seq <<"] and we expected" 
          <<"[" << quorum::type_str(kind) << "," << pid << "," << next_id << "]." << std::endl; 
       */   
      if (k != kind && k == quorum::TofinoWr){
        //such a WC means that the Write worked for all the nodes, so we update all 
        // Question : is using reset() ok ? 
        reset(next_id + modulo);              

        continue;
      }
      else if (k != kind) { 
        continue;
      }

      auto current_seq = scoreboard[pid];
      scoreboard[pid] = current_seq + modulo == seq ? seq : 0;

      if (scoreboard[pid] == next_id) {
        left -= 1;
        ret_left = left; 
      }

      if (left == 0) {
        left = quorum_size;
        next_id += modulo;
      }
    }
  }

  return true;
}

template <class ID> inline bool SerialQuorumWaiter<ID>::canContinueWith(ID expected) const {
  return next_id >= expected;
}

/*On vérifie si tout le monde est arrivé à (expected - outstanding),
ce qui permet de moins attendre*/
template <class ID> inline bool SerialQuorumWaiter<ID>::canContinueWithOutstanding(
    int outstanding, ID expected) const {
  return next_id + outstanding >= expected;
}

template <class ID> int SerialQuorumWaiter<ID>::maximumResponses() const {
  // The number of processes that can go to the next round:
  return static_cast<int>(
      std::count_if(scoreboard.begin(), scoreboard.end(),
                    [this](ID i) { return i + modulo == next_id; }));
}
}  // namespace dory

// Instantiations
namespace dory {
template class SerialQuorumWaiter<uint64_t>;
template class SerialQuorumWaiter<int64_t>;
}  // namespace dory