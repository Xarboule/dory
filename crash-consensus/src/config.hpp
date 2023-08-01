#pragma once

namespace dory {
namespace ConsensusConfig {

static constexpr bool nameThreads = true;
static const char handoverThreadName[] = "thd_handover";
static const char consensusThreadName[] = "thd_consensus";
static const char switcherThreadName[] = "thd_switcher";
static const char heartbeatThreadName[] = "thd_heartbeat";
static const char followerThreadName[] = "thd_follower";
static const char fileWatcherThreadName[] = "thd_filewatcher";

static constexpr int handoverThreadBankAB_ID = 0; //sibling 1
static constexpr int fileWatcherThreadBankAB_ID = 10; //sibling 3

static constexpr int consensusThreadBankA_ID = 15; //sibling 1 
static constexpr int consensusThreadBankB_ID = -1;

static constexpr int switcherThreadBankA_ID = 4; //sibling 2
static constexpr int switcherThreadBankB_ID = -1;

static constexpr int heartbeatThreadBankA_ID = 19; //sibling 2
static constexpr int heartbeatThreadBankB_ID = -1;

static constexpr int followerThreadBankA_ID = 25; //sibling 3
static constexpr int followerThreadBankB_ID = -1;

struct ThreadConfig {
  ThreadConfig()
      : //pinThreads{true}, //le vrai code de mu
        pinThreads{false}, 
        handoverThreadCoreID{handoverThreadBankAB_ID},
        consensusThreadCoreID{consensusThreadBankA_ID},
        switcherThreadCoreID{switcherThreadBankA_ID},
        heartbeatThreadCoreID{heartbeatThreadBankA_ID},
        followerThreadCoreID{followerThreadBankA_ID},
        fileWatcherThreadCoreID{fileWatcherThreadBankAB_ID},
        prefix{""} {}

  bool pinThreads;

  // If pinThreads is true, make sure to set the core id appropriately.
  // You can use `numatcl -H` to see which cores each numa domain has.
  // In hyperthreaded CPUs, numactl first lists the non-hyperthreaded cores
  // and then the hyperthreaded ones.

  int handoverThreadCoreID;
  int consensusThreadCoreID;
  int switcherThreadCoreID;
  int heartbeatThreadCoreID;
  int followerThreadCoreID;
  int fileWatcherThreadCoreID;
  std::string prefix;
};

}  // namespace ConsensusConfig
}  // namespace dory
