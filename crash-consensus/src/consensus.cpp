#include "consensus.hpp"

#include <iostream>
// #include <algorithm>
// #include <functional>

namespace dory {
RdmaConsensus::RdmaConsensus(int my_id, std::vector<int>& remote_ids,
                             int outstanding_req,
                             bool want_tofino,
                             ConsensusConfig::ThreadConfig threadConfig)
    : my_id{my_id},
      remote_ids{remote_ids},
      am_I_leader{false},
      ask_reset{false},
      outstanding_req{outstanding_req},
      threadConfig{threadConfig},
      store(threadConfig.prefix),
      LOGGER_INIT(logger, ConsensusConfig::logger_prefix),
      use_tofino(want_tofino) {
  using namespace units;

  allocated_size = 2_GiB;
  alignment = 64;

  run();

  // TODO (Check that): Both iterators are implicitly protected by the mutex,
  // since they are shared between two threads.
  iter = re_ctx->log.blockingIterator();
  commit_iter = re_ctx->log.liveIterator();

  follower = Follower(re_ctx.get(), leader_election->context(), &iter,
                      &commit_iter, threadConfig);
  follower.waitForPoller();
}

RdmaConsensus::~RdmaConsensus() { consensus_thd.join(); }


/*thread qui va demander en boucle les permissions*/
void RdmaConsensus::spawn_follower() {
 
  consensus_thd = std::thread([this]() {
    
    follower.spawn();

    bool force_permission_request = false;
    am_I_leader.store(false);

    while (true) {
      bool asked_reset = ask_reset.load();
      if (asked_reset) {
        LOGGER_WARN(logger, "Hard-reset requested.");
        //throw std::runtime_error("changeRights failed or Hard set ==> stop !");
        force_permission_request = true;
        am_I_leader.store(false);
      }
   
      // It should internally block/unblock the follower
      auto apply_ok = leader_election->checkAndApplyConnectionPermissionsOK(follower, am_I_leader, force_permission_request);
    
      // If apply is not ok, means that I tried to become a leader, but failed
      // because somebody else tried to become as well.
      if (unlikely(!apply_ok)) {
        LOGGER_WARN(logger, "Permission request interrupted. Trying again.");
        force_permission_request = true;
        am_I_leader.store(false);
      }

      if (unlikely(asked_reset)) {
        ask_reset.store(false);
      }
    }
  });

  if (threadConfig.pinThreads) {
    pinThreadToCore(consensus_thd, threadConfig.consensusThreadCoreID);
  }

  if (ConsensusConfig::nameThreads) {
    setThreadName(consensus_thd, ConsensusConfig::consensusThreadName);
  }
}


/*C'est là que tout s'initialise : connexions, contextes etc. */
void RdmaConsensus::run() {
  std::vector<int> ids(remote_ids);
  ids.push_back(my_id);

  // Get the last device
  //Dans notre cas, comme on n'a qu'une seule device, ça va
  {
    for (auto& dev : d.list()) {
      od = std::move(dev);
      break;
    }
  }

  LOGGER_INFO(logger,
              "Device name: {}, Device verbs name: {}, Extra info: {} {}",
              od.name(), od.dev_name(), OpenDevice::type_str(od.node_type()),
              OpenDevice::type_str(od.transport_type()));

  //std::cout << "Max_qp_rd_atom : " << od.device_attributes().max_qp_rd_atom << std::endl;


  rp = std::make_unique<ResolvedPort>(od);
  auto binded = rp->bindTo(0);
  LOGGER_INFO(logger, "Binding to the first port of the device... {}",
              binded ? "OK" : "FAILED");
  LOGGER_INFO(logger, "Binded on (port_id, port_lid) = ({:d}, {:d})",
              rp->portID(), rp->portLID());

  // Configure the control block
  cb = std::make_unique<ControlBlock>(*rp.get());
  cb->registerPD("primary");
  cb->allocateBuffer("shared-buf", allocated_size, alignment);
  cb->registerMR("shared-mr", "primary", "shared-buf",
                 ControlBlock::LOCAL_READ | ControlBlock::LOCAL_WRITE |
                     ControlBlock::REMOTE_READ | ControlBlock::REMOTE_WRITE);
  cb->registerCQ("cq-replication");
  cb->registerCQ("cq-leader-election");

  // Configure the connection exchanger for the replication plane
  ce_replication = std::make_unique<ConnectionExchanger>(my_id, remote_ids, *cb.get());
  ce_replication->configure_all("primary", "shared-mr", "cq-replication",
                                "cq-replication");
  
  // Configure the connection exchanger for background plane
  ce_leader_election = std::make_unique<ConnectionExchanger>(my_id, remote_ids, *cb.get()); //utiliser make_unique avec ipAddresses dans ConnectionExchanger produit des erreurs de mémoire
  ce_leader_election->configure_all("primary", "shared-mr",
                                    "cq-leader-election", "cq-leader-election");
  ce_leader_election->addLoopback("primary", "shared-mr", "cq-leader-election",
                                  "cq-leader-election"); //loopback sert pour incrémenter le heartbeat
  ce_leader_election->connectLoopback(
      ControlBlock::LOCAL_READ | ControlBlock::LOCAL_WRITE |
      ControlBlock::REMOTE_READ | ControlBlock::REMOTE_WRITE);
  


  // Configure the log 
  auto shared_memory_addr = reinterpret_cast<uint8_t*>(cb->mr("shared-mr").addr);
  overlay = std::make_unique<OverlayAllocator>(shared_memory_addr, allocated_size);
  scratchpad =  std::make_unique<ScratchpadMemory>(ids, *overlay.get(), alignment);
  auto [logmem_ok, logmem, logmem_size] = overlay->allocateRemaining(alignment);

  LOGGER_INFO(logger, "Log allocation... {}", logmem_ok ? "OK" : "FAILED");
  LOGGER_INFO(logger, "Log (address: 0x{:x}, size: {} bytes)",
              uintptr_t(logmem), logmem_size);

  auto log_offset = logmem - shared_memory_addr;

  replication_log = std::make_unique<Log>(logmem, logmem_size);

  std::cout << "connecting all" << std::endl;  
  //connecting everything 

  int port = 20886;
  ce_replication->connect_all(
      store, "qp-replication",
      port,
      ControlBlock::LOCAL_READ | ControlBlock::LOCAL_WRITE);
  
  //make sure to give the right port number
  ce_leader_election->connect_all(
      store, "qp-leader-election",
      port + 1000, //here, we just shift by a big number 
      ControlBlock::LOCAL_READ | ControlBlock::LOCAL_WRITE |ControlBlock::REMOTE_READ | ControlBlock::REMOTE_WRITE); 
  

  /*std :: string foo;
  std::cout <<"EVERYTHING IS CONNECTED WELL\n";
  std::cout <<"Press anything to continue once all connections are ok\n";
  std :: cin >> foo;*/

  //Un "context", c'est rien d'autre qu'un struct qui contient tout ce qui est pertinent, pour faciliter l'accès

  // Initialize the context of replication plane (ConnectionContext and ReplicationContext) 
  auto& cq_replication = cb->cq("cq-replication");
  re_conn_ctx = std::make_unique<ConnectionContext>(
      *cb.get(), *ce_replication.get(), cq_replication, remote_ids, my_id);
  re_ctx = std::make_unique<ReplicationContext>(
      *re_conn_ctx.get(), *replication_log.get(), log_offset);


  // Initialize the context of background plane : 
  auto& cq_leader_election = cb->cq("cq-leader-election");
  le_conn_ctx = std::make_unique<ConnectionContext>(
      *cb.get(), *ce_leader_election.get(), cq_leader_election, remote_ids,
      my_id);


  // Initialize Leader election
  leader_election = std::make_unique<LeaderElection>(*le_conn_ctx.get(), *scratchpad.get(), threadConfig);
  leader_election->attachReplicatorContext(re_ctx.get());
  response_blocked = &(leader_election->response_blocked);


  // Initialize replication
  auto quorum_size = quorum::majority(remote_ids.size() + 1) - 1;
  auto next_log_entry_offset = re_ctx->log.headerFirstUndecidedOffset();
  LOGGER_TRACE(logger, "My first undecided offset is {}",next_log_entry_offset);

  catchup =  std::make_unique<CatchUpWithFollowers>(re_ctx.get(), *scratchpad.get());
  lsr = std::make_unique<LogSlotReader>(re_ctx.get(), *scratchpad.get(),
                                        next_log_entry_offset);

  follower.attach(&lsr, scratchpad.get());

  log_recycling =  std::make_unique<LogRecycling>(re_ctx.get(), *scratchpad.get());

  sqw = std::make_unique<SequentialQuorumWaiter>(  quorum::EntryWr, re_ctx->cc.remote_ids, quorum_size, 1);
  majW = std::make_unique<FixedSizeMajorityOperation<SequentialQuorumWaiter,
                                                     WriteLogMajorityError>>(
      &re_ctx->cc, *sqw.get(), re_ctx->cc.remote_ids);

  to_remote_memory.resize(Identifiers::maxID(remote_ids) + 1);
  std::fill(to_remote_memory.begin(), to_remote_memory.end(), log_offset);
  dest = to_remote_memory;

  LOGGER_INFO(logger, "Waiting (5 sec) for all threads to start");
  std::this_thread::sleep_for(std::chrono::seconds(5));

  std::cout << "Use tofino in consensus : " << use_tofino << std::endl;
  if (use_tofino){
    std::cout << "Seting up tofino" << std::endl;
    ce_replication->setup_tofino();
  }
}

int RdmaConsensus::propose(uint8_t* buf, size_t buf_len) {
  //std::cout << "================================About to propose================================ " << std::endl;
  //std::cout <<"Checking the state of the replication qp" << std::endl;
  //re_ctx->cc.ce.check_all_qp_states();
  
  std::unique_lock<std::mutex> lock(follower.lock(), std::defer_lock);
  
  if (!lock.try_lock()) {
    auto& leader = leader_election->leaderSignal();
    potential_leader = leader.load().requester;
    return ret_error(lock, ProposeError::MutexUnavailable);
  }

  // Make fast-path slightly faster
  // TODO eliminate duplicate code from down below
  //(c'est parce qu'il y a 2 fois le first path)
  
  if (likely(fast_path) && likely(am_I_leader.load())) {
    auto& leader = leader_election->leaderSignal();

    //on enregistre la valeur dans notre lof
    auto local_fuo = re_ctx->log.headerFirstUndecidedOffset();
    Slot slot(re_ctx->log, proposal_nr, local_fuo, buf, buf_len);
    auto [address, offset, size] = slot.location();

    //on écrit dans celui des autres
    auto ok = majW->fastWrite(address, size, to_remote_memory, offset, leader,
                              outstanding_req, use_tofino);

      
    if (likely(ok)) {
      //on avance le fuo
      auto fuo = LogConfig::round_up_powerof2(offset + size);
      re_ctx->log.updateHeaderFirstUndecidedOffset(fuo);
      auto has_next = iter.sampleNext();
      if (has_next) {
        ParsedSlot pslot(iter.location());

        LOGGER_TRACE(logger, "Accepted proposal: {}, FUO: {}",
                     pslot.acceptedProposal(), pslot.firstUndecidedOffset());

        // Now that I got something, I will use the commit iterator
        //(pk c'est pas le follower thread qui s'en occupe ?)
        while (commit_iter.hasNext(fuo)) {
          commit_iter.next();

          ParsedSlot pslot(commit_iter.location());
          auto [buf, len] = pslot.payload();
          commit(true, buf, len);
        }
      }
    } else {

      LOGGER_ERROR(logger,
                   "Error in fast-path: occurred when writing the new "
                   "value to a majority");
      auto err = majW->fastWriteError();
      majW->recoverFromError(err);

      return ret_error(lock, ProposeError::FastPath, true);
    }

    return ret_no_error();
  }

  
  if (am_I_leader.load()) {  // Leader (slow and fast-path)
    //si je viens de devenir le leader, pas de fast path, et on reconstruit le log
    if (unlikely(became_leader)) {
      fast_path = false;
      became_leader = false;
      LOGGER_TRACE(logger, "Rebuilding log");
      re_ctx->log.rebuildLog();
    }

    // Hanging workaround
    auto& leader = leader_election->leaderSignal();

    if (likely(fast_path)) {  // Fast-path
      //std :: cout << "in the fast path "<< std :: endl;
      if (unlikely(re_ctx->log.spaceLeftCritical())) {
        return ret_error(lock, ProposeError::FastPathRecyclingTriggered);
      }

      auto local_fuo = re_ctx->log.headerFirstUndecidedOffset();
      Slot slot(re_ctx->log, proposal_nr, local_fuo, buf, buf_len);
      auto [address, offset, size] = slot.location();

      auto ok = majW->fastWrite(address, size, to_remote_memory, offset, leader,
                                outstanding_req, use_tofino);

      if (likely(ok)) {
        auto fuo = LogConfig::round_up_powerof2(offset + size);
        re_ctx->log.updateHeaderFirstUndecidedOffset(fuo);
        auto has_next = iter.sampleNext();
        if (has_next) {
          ParsedSlot pslot(iter.location());

          LOGGER_TRACE(logger, "Accepted proposal: {}, FUO: {}",
                       pslot.acceptedProposal(), pslot.firstUndecidedOffset());
          

          // Now that I got something, I will use the commit iterator
          while (commit_iter.hasNext(fuo)) {
            commit_iter.next();

            ParsedSlot pslot(commit_iter.location());
            auto [buf, len] = pslot.payload();
            commit(true, buf, len);
          }
          
        }
      } else {
        LOGGER_TRACE(logger,
                     "Error in fast-path: occurred when writing the new "
                     "value to a majority");

        auto err = majW->fastWriteError();
        majW->recoverFromError(err);

        return ret_error(lock, ProposeError::FastPath, true);
      }
      }else {  // Slow-path
      auto update_followers_fuo_err = catchup->catchFUO(leader);
      if (!update_followers_fuo_err->ok()) {
        LOGGER_TRACE(logger,
                     "Error in slow-path: occurred when getting the FUO of "
                     "remote logs ({})",
                     MaybeError::type_str(update_followers_fuo_err->type()));
        catchup->recoverFromError(update_followers_fuo_err);

        return ret_error(lock, ProposeError::SlowPathCatchFUO, true);
      }

      auto update_followers_err = catchup->updateFollowers(leader);
      if (!update_followers_err->ok()) {
        LOGGER_TRACE(logger,
                     "Error in slow-path: occurred when updating the remote "
                     "logs of followers ({})",
                     MaybeError::type_str(update_followers_err->type()));
        catchup->recoverFromError(update_followers_err);

        return ret_error(lock, ProposeError::SlowPathUpdateFollowers, true);
      }

      auto catchup_proposal_err = catchup->catchProposal(leader);

      bool encountered_error = false;
      while (!catchup_proposal_err->ok()) {
        if (catchup_proposal_err->type() ==
            MaybeError::CatchProposalRetryError) {
          catchup_proposal_err = catchup->catchProposal(leader);
        } else {
          LOGGER_TRACE(logger,
                       "Error in slow-path: occurred when catching-up ({})",
                       MaybeError::type_str(catchup_proposal_err->type()));
          catchup->recoverFromError(catchup_proposal_err);
          encountered_error = true;
          break;
        }
      }

      if (encountered_error) {
        return ret_error(lock, ProposeError::SlowPathCatchProposal, true);
      }
      LOGGER_TRACE(logger, "Passed catchup.catchProposal()");

      auto catchup_update_proposal_err = catchup->updateWithCurrentProposal(leader);
      if (!catchup_update_proposal_err->ok()) {
        LOGGER_TRACE(
            logger,
            "Error in slow-path: occurred when updating the proposal ({})",
            MaybeError::type_str(catchup_update_proposal_err->type()));
        catchup->recoverFromError(catchup_update_proposal_err);
        encountered_error = true;
      }

      if (encountered_error) {
        return ret_error(lock, ProposeError::SlowPathUpdateProposal, true);
      }
      LOGGER_TRACE(logger, "Passed catchup.updateWithCurrentProposal()");

      proposal_nr = catchup->proposal();
      LOGGER_TRACE(logger, "Attempt writes with proposal numer = {}",
                   proposal_nr);

      // Trying to get the freshest value from the remote logs on the same
      // IndexedIterator index
      auto local_fuo = re_ctx->log.headerFirstUndecidedOffset();
      auto local_fuo_entry = re_ctx->log.headerPtr() + local_fuo;

      LOGGER_TRACE(logger, "My local first undecided offset = {}", local_fuo);

      // Find the freshest among the remote responses
      uint8_t* freshest = nullptr;
      uint64_t max_accepted_proposal = 0;

      auto resp = lsr->readSlotAt(local_fuo, leader);
      if (resp->ok()) {
        auto& successes = lsr->successes();

        ParsedSlot local_pslot(local_fuo_entry);
        if (local_pslot.isPopulated()) {
          freshest = local_fuo_entry;
          max_accepted_proposal = local_pslot.acceptedProposal();

          LOGGER_TRACE(logger, "Accepted proposal: {}, FUO: {}",
                       local_pslot.acceptedProposal(),
                       local_pslot.firstUndecidedOffset());
          // auto [buf, len] = local_pslot.payload();
        }

        for (auto pid : successes) {
          if (pid < 0) {
            LOGGER_TRACE(logger, "Nothing to read from {}", -pid);
          } else {
            LOGGER_TRACE(logger, "Something to read from {}", pid);
            auto store_addr = scratchpad->readLogEntrySlots()[pid];
            ParsedSlot pslot(store_addr);

            LOGGER_TRACE(logger, "Accepted proposal: {}, FUO: {}",
                         pslot.acceptedProposal(),
                         pslot.firstUndecidedOffset());
            // auto [buf, len] = pslot.payload();

            if (max_accepted_proposal < pslot.acceptedProposal()) {
              max_accepted_proposal = pslot.acceptedProposal();
              freshest = store_addr;
            }
          }
        }
      } else {
        LOGGER_TRACE(
            logger,
            "Error in slow-path: occured when reading the remote slots ({})",
            resp->type_str(resp->type()));
        lsr->recoverFromError(resp);

        return ret_error(lock, ProposeError::SlowPathReadRemoteLogs, true);
      }

      LOGGER_TRACE(logger, "Got the freshest value");

      if (freshest != nullptr) {
        LOGGER_TRACE(logger, "Proposing the freshest value in the slow-path");

        auto size = ParsedSlot::copy(local_fuo_entry, freshest);
        // TODO (Check that): These lines are necessary
        ParsedSlot fresh_pslot(local_fuo_entry);
        fresh_pslot.setAcceptedProposal(proposal_nr);

        LOGGER_TRACE(logger, "Accepted proposal: {}, FUO: {}",
                     fresh_pslot.acceptedProposal(),
                     fresh_pslot.firstUndecidedOffset());
        // auto [buf, len] = fresh_pslot.payload();

        std::transform(to_remote_memory.begin(), to_remote_memory.end(),
                       dest.begin(),
                       bind2nd(std::plus<uintptr_t>(), local_fuo));
        auto err = majW->write(local_fuo_entry, size, dest, leader);

        if (!err->ok()) {
          majW->recoverFromError(err);
          return ret_error(lock, ProposeError::SlowPathWriteAdoptedValue, true);
        } else {
          auto fuo = LogConfig::round_up_powerof2(local_fuo + size);
          re_ctx->log.updateHeaderFirstUndecidedOffset(fuo);
          auto has_next = iter.sampleNext();
          if (has_next) {
            ParsedSlot pslot(iter.location());

            LOGGER_TRACE(logger, "Accepted proposal: {}, FUO: {}",
                         pslot.acceptedProposal(),
                         pslot.firstUndecidedOffset());
            // auto [buf, len] = pslot.payload();

            // Now that I got something, I will use the commit iterator
            while (commit_iter.hasNext(fuo)) {
              commit_iter.next();

              ParsedSlot pslot(commit_iter.location());
              auto [buf, len] = pslot.payload();
              commit(true, buf, len);
            }
          }
        }

      } else {
        fast_path = true;
        LOGGER_TRACE(logger, "Proposing the new value in the slow-path");

        // std::cout << "Space left " << re_ctx->log.spaceLeft() << std::endl;
        if (unlikely(re_ctx->log.spaceLeftCritical())) {
          // Send log recycle request in the form:
          Slot slot(re_ctx->log);
          slot.storeAcceptedProposal(proposal_nr);
          slot.storeFirstUndecidedOffset(0);
          // std::cout << "Sending recycling request with fuo:" << local_fuo <<
          // std::endl;
          auto recycling_req = log_recycling->generateRequest(local_fuo);
          slot.storePayload(reinterpret_cast<uint8_t*>(&recycling_req),
                            sizeof(recycling_req));

          auto [address, offset, size] = slot.location();
          std::transform(to_remote_memory.begin(), to_remote_memory.end(),
                         dest.begin(), bind2nd(std::plus<uintptr_t>(), offset));
          auto err = majW->write(address, size, dest, leader);

          if (!err->ok()) {
            majW->recoverFromError(err);
            return ret_error(lock, ProposeError::SlowPathWriteNewValue, true);
          } else {
            re_ctx->log.resetFUO();
            re_ctx->log.rebuildLog();

            iter = re_ctx->log.blockingIterator();
            commit_iter = re_ctx->log.liveIterator();

            auto next_log_entry_offset =
                re_ctx->log.headerFirstUndecidedOffset();
            lsr = std::make_unique<LogSlotReader>(
                re_ctx.get(), *scratchpad.get(), next_log_entry_offset);

            // Wait for all to clean-up their log
            log_recycling->waitForReplies();

            re_ctx->log.bzero();

            // std::cout << "Sleeping..." << std::endl;
            // std::this_thread::sleep_for(std::chrono::seconds(2));

            return ret_error(lock, ProposeError::SlowPathLogRecycled);
          }
        }

        Slot slot(re_ctx->log);

        // TODO: Are these values correct?
        slot.storeAcceptedProposal(proposal_nr);
        slot.storeFirstUndecidedOffset(local_fuo);
        slot.storePayload(buf, buf_len);

        auto [address, offset, size] = slot.location();

        std::transform(to_remote_memory.begin(), to_remote_memory.end(),
                       dest.begin(), bind2nd(std::plus<uintptr_t>(), offset));
        auto err = majW->write(address, size, dest, leader);

        if (!err->ok()) {
          majW->recoverFromError(err);
          return ret_error(lock, ProposeError::SlowPathWriteNewValue, true);
        } else {
          auto fuo = LogConfig::round_up_powerof2(offset + size);
          re_ctx->log.updateHeaderFirstUndecidedOffset(fuo);
          auto has_next = iter.sampleNext();
          if (has_next) {
            ParsedSlot pslot(iter.location());

            LOGGER_TRACE(logger, "Accepted proposal: {}, FUO: {}",
                         pslot.acceptedProposal(),
                         pslot.firstUndecidedOffset());
            // auto [buf, len] = pslot.payload();

            // Now that I got something, I will use the commit iterator
            while (commit_iter.hasNext(fuo)) {
              commit_iter.next();

              ParsedSlot pslot(commit_iter.location());
              auto [buf, len] = pslot.payload();
              commit(true, buf, len);
            }
          }
        }
      }
    }
  } else {
    LOGGER_TRACE(logger, "Reject proposal request because I am a follower");
    became_leader = true;
    fast_path = false;
    auto& leader = leader_election->leaderSignal();
    potential_leader = leader.load().requester;
    return ret_error(lock, ProposeError::FollowerMode);
  }

  return ret_no_error();
}

}  // namespace dory
