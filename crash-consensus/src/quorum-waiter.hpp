#pragma once

#include <functional>
#include <iostream>
#include <vector>

#include <dory/extern/ibverbs.hpp>

#include "message-identifier.hpp"

namespace dory {

/*
  Dans un scoreboard, on garde une trace de la situation actuelle de chaque noeud
  (pour un kind fixé) en marquant indiquant le "seq" actuelle
  L'objectif, c'est que tous les noeuds atteignent le "seq" voulu : next_id. 
  'left', c'est le nombre de noeuds qui n'ont pas encore atteint ce next_id

  Modulo, c'est l'écart entre deux valeurs possible de "seq". 

*/
template <class ID> class SerialQuorumWaiter {
 public:
  using ReqIDType = ID;
  SerialQuorumWaiter() = default;

  SerialQuorumWaiter(quorum::Kind kind, std::vector<int>& remote_ids, size_t quorum_size, ID next_id, ID modulo);

  inline ID reqID() const { return next_id; }
  inline ID nextReqID() const { return next_id + modulo; }
  inline void setFastReqID(ID id) { fast_id = id; }

  inline ID fetchAndIncFastID() {
    auto ret = fast_id;
    fast_id += modulo;
    return ret;
  }

  inline ID nextFastReqID() const { return fast_id; }

  
  /*Tu lui donnes des work completions (wc)
  Va "unpack" le wr_id, et si ça matche avec ce qu'on attend (bon kind et 
  seq qui matche avec next_id), alors on mets à jour le scoreboard*/
  bool consume(std::vector<struct ibv_wc>& entries, std::vector<int>& successful_ops);


  /*Comme consume, mais tu indiques le nombre de wc à traiter (int num)
  et le nombre de noeuds qui n'ont pas encore atteint next_id est renseigné dans 
  ret_left*/
  bool fastConsume(std::vector<struct ibv_wc>& entries, int num, int& ret_left);


  bool canContinueWith(ID expected) const;
  bool canContinueWithOutstanding(int outstanding, ID expected) const;

  int maximumResponses() const;

  //reset the scoreboard for next 
  void reset(ID next);

  void changeQuorum(int size) { quorum_size = size; }

  using PackerPID = int;
  using PackerSeq = ID;

  static inline constexpr uint64_t packer(quorum::Kind k, PackerPID pid,
                                          PackerSeq seq) {
    return quorum::pack(k, pid, seq);
  }

  inline quorum::Kind kindOfOp() { return kind; }

 private:
  quorum::Kind kind; //le genre d'opération, renseigné dans la wr_id
  std::vector<ID> scoreboard;
  int quorum_size;
  ID next_id;     
  ID fast_id;
  int left; 
  ID modulo;    //écart entre les ids à atteindre ? 

};
}  // namespace dory

namespace dory {
class SequentialQuorumWaiter : public SerialQuorumWaiter<uint64_t> {
 public:
  SequentialQuorumWaiter() : SerialQuorumWaiter<uint64_t>() {}
  SequentialQuorumWaiter(quorum::Kind kind, std::vector<int>& remote_ids,
                         size_t quorum_size, uint64_t next_id)
      : SerialQuorumWaiter<uint64_t>(kind, remote_ids, quorum_size, next_id,
                                     1) {}
};


//jamais utilisé ! 
class ModuloQuorumWaiter : public SerialQuorumWaiter<int64_t> {
 public:
  ModuloQuorumWaiter() : SerialQuorumWaiter<int64_t>() {}
  ModuloQuorumWaiter(quorum::Kind kind, std::vector<int>& remote_ids,
                     size_t quorum_size, int64_t next_id, int modulo)
      : SerialQuorumWaiter<int64_t>(kind, remote_ids, quorum_size, next_id,
                                    modulo) {}
};
}  // namespace dory

namespace dory {
/*Un moyen de traquer si, pour un 'kind' fixé, les opérations (après un id donné) ont échoué ou non 
Ce qui nous intéresse, c'est si le nombre d'échec dépasse un certain seuil
*/
class FailureTracker {
 public:
  FailureTracker() {}

  FailureTracker(quorum::Kind kind, std::vector<int>& remote_ids,
                 int tolerated_failures)
      : kind{kind}, tolerated_failures{tolerated_failures}, track_id{0} {
    auto max_elem = Identifiers::maxID(remote_ids);
    failures.resize(max_elem + 1);

    reset();
  }

  void reset() {
    track_id = 0;
    failed = 0;
    std::fill(failures.begin(), failures.end(), false);
  }

  void track(uint64_t id) {
    if (track_id != 0) {
      reset();
      track_id = id;
    }
  }

  bool isUnrecoverable(std::vector<struct ibv_wc>& entries) {
    for (auto const& entry : entries) {
      if (entry.status != IBV_WC_SUCCESS) {
        auto [k, pid, seq] = quorum::unpackAll<uint64_t, uint64_t>(entry.wr_id);

        if (k == kind && seq >= track_id && !failures[pid]) {
          failures[pid] = true;
          failed += 1;
        }
#ifndef NDEBUG
        else {
          std::cout << "Found unrelated remnants in the polled responses"  << std::endl;
        }
#endif
      }

      if (failed > tolerated_failures) {
        return true;
      }
    }

    return false;
  }

  

 private:
  quorum::Kind kind;
  int tolerated_failures;

  std::vector<uint64_t> failures;
  uint64_t track_id;
  int failed;

  static quorum::kind tof_kind;
};
}  // namespace dory