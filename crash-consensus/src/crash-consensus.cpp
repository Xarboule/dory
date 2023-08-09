#include <stdexcept>

#include "consensus.hpp"
#include "crash-consensus.hpp"

namespace dory {
Consensus::Consensus(int my_id, std::vector<int> &remote_ids,
                     int outstanding_req, bool want_tofino=false,  ThreadBank threadBank) {
  ConsensusConfig::ThreadConfig config;
  switch (threadBank) {
    case ThreadBank::A:
      std::cout << "RdmaConsensus object created with default ThreadBank settings (A)" << std::endl;
      impl = std::make_unique<RdmaConsensus>(my_id, remote_ids, outstanding_req);
      break;
    case ThreadBank::B:
      std::cout << "RdmaConsensus object created with ThreadBank settings" << std::endl;
      config.consensusThreadCoreID = ConsensusConfig::consensusThreadBankB_ID;
      config.switcherThreadCoreID = ConsensusConfig::switcherThreadBankB_ID;
      config.heartbeatThreadCoreID = ConsensusConfig::heartbeatThreadBankB_ID;
      config.followerThreadCoreID = ConsensusConfig::followerThreadBankB_ID;
      config.prefix = "Secondary-";
      impl = std::make_unique<RdmaConsensus>(my_id, remote_ids, outstanding_req, want_tofino, config);
      break;
    default:
      throw std::runtime_error("Unreachable, software bug");
  }
}

Consensus::~Consensus() {}

/*commitHandler prend en argument une fonction (a callable object, of type std::function), qui return void et prend 3 paramètres : 
bool leader, uint8_t buf, et size_t commiter
Cette fonction sera le commitHandler des followers de impl*/
void Consensus::commitHandler( std::function<void(bool leader, uint8_t *buf, size_t len)> committer) {
  impl->commitHandler(committer);
}

/*wrapper autour du propose() de RdmaConsensus*/
ProposeError Consensus::propose(uint8_t *buf, size_t len) {
  int ret = impl->propose(buf, len);
  return static_cast<ProposeError>(ret);
}

int Consensus::potentialLeader() { return impl->potentialLeader(); }
bool Consensus::blockedResponse() { return impl->response_blocked->load(); }

std::pair<uint64_t, uint64_t> Consensus::proposedReplicatedRange() {
  return impl->proposedReplicatedRange();
}
}  // namespace dory
