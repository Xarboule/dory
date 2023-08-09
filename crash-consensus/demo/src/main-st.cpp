#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <dory/crash-consensus.hpp>

#include <cassert>
#include <condition_variable>

#include "helpers.hpp"
#include "timers.h"

void benchmark(int id, std::vector<int> remote_ids, int times, int payload_size,
               int outstanding_req, dory::ThreadBank threadBank);

int main(int argc, char* argv[]) {
  if (argc < 4) {
    throw std::runtime_error("Provide the id of the process as argument");
  }

  constexpr int nr_procs = 3;
  constexpr int minimum_id = 1;
  int id = 0;
  switch (argv[1][0]) {
    case '1':
      id = 1;
      break;
    case '2':
      id = 2;
      break;
    case '3':
      id = 3;
      break;
    default:
      throw std::runtime_error("Invalid id");
  }

  std::cout << "USING ID = " << id << std :: endl;

  int payload_size = atoi(argv[2]);
  std::cout << "USING PAYLOAD SIZE = " << payload_size << std::endl;

  int outstanding_req = atoi(argv[3]);
  std::cout << "USING OUTSTANDING_REQ = " << outstanding_req << std::endl;

  // Build the list of remote ids
  std::vector<int> remote_ids;
  for (int i = 0, min_id = minimum_id; i < nr_procs; i++, min_id++) {
    if (min_id == id) {
      continue;
    } else {
      remote_ids.push_back(min_id);
    }
  }

  const int times = static_cast<int>(1.5 * 1024) * 1024 * 1024 / (payload_size + 64); //pk ? 
  
  benchmark(id, remote_ids, times, payload_size, outstanding_req, dory::ThreadBank::A);

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(60));
  }

  return 0;
}

void benchmark(int id, std::vector<int> remote_ids, int times, int payload_size, int outstanding_req, dory::ThreadBank threadBank) {
  dory::Consensus consensus(id, remote_ids, outstanding_req,false,  threadBank);
  
  consensus.commitHandler([]([[maybe_unused]] bool leader,
                             [[maybe_unused]] uint8_t* buf,
                             [[maybe_unused]] size_t len) {});

  // Wait enough time for the consensus to become ready
  std::cout << "Wait some time (" << (5+id) << "seconds)"<< std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(5+id));

  if (id == 1) {
    TIMESTAMP_INIT;

    std::vector<uint8_t> payload_buffer(payload_size + 2);
    uint8_t* payload = &payload_buffer[0];

    std::vector<TIMESTAMP_T> timestamps_start(times);
    std::vector<TIMESTAMP_T> timestamps_end(times);
    std::vector<std::pair<int, TIMESTAMP_T>> timestamps_ranges(times);
    TIMESTAMP_T loop_time;

    mkrndstr_ipa(payload_size, payload);
    auto ret = consensus.propose(payload, payload_size);


    if (ret != dory::ProposeError::NoError){
      std::cout << "Error in the first propose"<<std::endl;
    }
    

    int offset = 2;

    std::vector<std::vector<uint8_t>> payloads(8192); //all the payloads, jsp pk 8192
    for (size_t i = 0; i < payloads.size(); i++) {
      payloads[i].resize(payload_size);   //chaque payload doit bien avoir la taille demandée
      mkrndstr_ipa(payload_size, &(payloads[i][0])); // on génère aléatoirement le contenu de chaque payload 
    }

    std::cout << "Started "<< std::endl;  
    
    TIMESTAMP_T start_meas, end_meas;

    GET_TIMESTAMP(start_meas);
    for (int i = 0; i < times; i++) {
      // GET_TIMESTAMP(timestamps_start[i]);
      // Encode process doing the proposal
      dory::ProposeError err;
      //std::cout << "Proposing " << i << "th" <<std::endl;
      
      //on parcourt les payloads, en proposant chacun 
      if ((err = consensus.propose(&(payloads[i % 8192][0]), payload_size)) != dory::ProposeError::NoError) {
        std::cout << "Proposal failed at index " << i << std::endl;
        i -= 1;
        switch (err) {
          case dory::ProposeError::FastPath:
          case dory::ProposeError::FastPathRecyclingTriggered:
          case dory::ProposeError::SlowPathCatchFUO:
          case dory::ProposeError::SlowPathUpdateFollowers:
          case dory::ProposeError::SlowPathCatchProposal:
          case dory::ProposeError::SlowPathUpdateProposal:
          case dory::ProposeError::SlowPathReadRemoteLogs:
          case dory::ProposeError::SlowPathWriteAdoptedValue:
          case dory::ProposeError::SlowPathWriteNewValue:
            std::cout << "Error: in leader mode. Code: "<< static_cast<int>(err) << std::endl;
            break;
          case dory::ProposeError::SlowPathLogRecycled:
            std::cout << "Log recycled, waiting a bit..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            break;
          case dory::ProposeError::MutexUnavailable:
          case dory::ProposeError::FollowerMode:
            std::cout << "Error: in follower mode. Potential leader: "<< consensus.potentialLeader() << std::endl;
            break;
          default:
            std::cout << "Bug in code. You should only handle errors here" << std::endl;
        }
        break;
      }
    }
    GET_TIMESTAMP(end_meas);
    
    double n_commands = static_cast<double>(times);
    double elapsed_time = static_cast<double>(ELAPSED_NSEC(start_meas, end_meas));
    double time_per_op = elapsed_time / n_commands;
    double throughput = n_commands*payload_size/elapsed_time  * 1000 *1000 *1000 /1024 /1024/1024; 
    std::cout << "Replicated " << times << " commands of size " << payload_size
              << " bytes in " << elapsed_time << " ns"
              << std::endl;

    std::cout << "Throughput = " << throughput << "Go/s"<< std::endl;
    std::cout << "Average latency of one op = " << time_per_op/1000<< "µs"<<std::endl;

    std::ofstream dump;
    dump.open("dump-throughput-" + std::to_string(payload_size) + "-" +
              std::to_string(outstanding_req) + ".txt");
    
    dump << "Replicated " << times << " commands of size " << payload_size
              << " bytes in " << elapsed_time << " ns"
              << "\n";

    dump << "Throughput = " << throughput << "Go/s"<< "\n";
    dump << "Average latency of one op = " << time_per_op/1000<< "µs"<<"\n";

    dump.close();

    exit(0);
  }
}
