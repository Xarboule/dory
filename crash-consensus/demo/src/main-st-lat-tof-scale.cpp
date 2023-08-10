
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <dory/crash-consensus.hpp>
#include "timers.h"

#include <cassert>
#include <condition_variable>

#include "helpers.hpp"
#include "timers.h"

void benchmark(int id, std::vector<int> remote_ids, int times, int payload_size,
               int outstanding_req, dory::ThreadBank threadBank, bool want_tofino);

int main(int argc, char* argv[]) {
  if (argc < 6) {
    throw std::runtime_error("Provide the id of the process as argument");
  }


  int payload_size = atoi(argv[2]);
  std::cout << "USING PAYLOAD SIZE = " << payload_size << std::endl;

  int outstanding_req = atoi(argv[3]);
  std::cout << "USING OUTSTANDING_REQ = " << outstanding_req << std::endl;

  bool want_tofino = (bool)atoi(argv[4]);
  std::cout << "USING WANT_TOFINO = " << want_tofino << std::endl;

  int nr_procs = atoi(argv[5]);
  std::cout << "USING N = " << nr_procs << std::endl;
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
    case '4':
      id = 4;
      break;
    case '5':
      id = 5;
      break;
    case '6':
      id = 6;
      break;
    case '7':
      id = 7;
      break;
    case '8':
      id = 8;
      break;
    default:
      throw std::runtime_error("Invalid id");
  }

  // Build the list of remote ids
  std::vector<int> remote_ids;
  for (int i = 0, min_id = minimum_id; i < nr_procs; i++, min_id++) {
    if (min_id == id) {
      continue;
    } else {
      remote_ids.push_back(min_id);
    }
  }

  std::cout << "My id =" << id << "Remote IDs : ";
  for (auto i : remote_ids){
    std::cout << i;
  }
  std::cout << std::endl;

  const int times =  static_cast<int>(1.5 * 1024) * 1024 * 1024 / (payload_size + 64);
  benchmark(id, remote_ids, times, payload_size, outstanding_req,dory::ThreadBank::A, want_tofino);

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(60));
  }

  return 0;
}

void benchmark(int id, std::vector<int> remote_ids, int times, int payload_size,
               int outstanding_req, dory::ThreadBank threadBank, bool want_tofino) {
  std::cout << "calling consensus() with want_tofino : " << want_tofino << std::endl;
  dory::Consensus consensus(id, remote_ids, outstanding_req, want_tofino, threadBank);
  consensus.commitHandler([]([[maybe_unused]] bool leader,
                             [[maybe_unused]] uint8_t* buf,
                             [[maybe_unused]] size_t len) {});


  // Wait enough time for the consensus to become ready
  std::cout << "Wait some time (" << (5) << "seconds)"<< std::endl;
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
    consensus.propose(payload, payload_size);

    int offset = 2;

    std::vector<std::vector<uint8_t>> payloads(100);
    for (size_t i = 0; i < payloads.size(); i++) {
      payloads[i].resize(payload_size+2);
      mkrndstr_ipa(payload_size, &(payloads[i][0]));
    }

    std::cout << "Started" << std::endl;

    std::string foo;
    for (int i = 0; i < times; i++) {
      //std::cout << "Next propose ? ";
      //std::cin >> foo;
      
      //std::cout << "Proposing " << i << std::endl;
      GET_TIMESTAMP(timestamps_start[i]);

      // Encode process doing the proposal
      dory::ProposeError err;
      // std::cout << "Proposing " << i << std::endl;
      if ((err = consensus.propose(&(payloads[i % 100][0]), payload_size)) != dory::ProposeError::NoError) {
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
            std::cout << "Error: in leader mode. Code: "
                      << static_cast<int>(err) << std::endl;
            break;

          case dory::ProposeError::SlowPathLogRecycled:
            std::cout << "Log recycled, waiting a bit..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            break;

          case dory::ProposeError::MutexUnavailable:
          case dory::ProposeError::FollowerMode:
            std::cout << "Error: in follower mode. Potential leader: "
                      << consensus.potentialLeader() << std::endl;
            break;

          default:
            std::cout << "Bug in code. You should only handle errors here"
                      << std::endl;
        }
        throw std::runtime_error(" failure in proposal ==> stop !");
        
      }

      GET_TIMESTAMP(loop_time);
      auto [id_posted, id_replicated] = consensus.proposedReplicatedRange();
      (void)id_posted;

      //std::cout << "id_posted : " << id_posted << "; id_replicated :" << id_replicated << std::endl;

      timestamps_ranges[i] = std::make_pair(int(id_replicated - offset), loop_time);
      /*
      if (i == 300){
        std::cout << "First 300 propose done : check ";
        std::cin >> foo;
      }

      if (i%10000==0){
        std::cout << "Check ";
        std::cin >> foo;
      }*/

    }

    std::ofstream dump;
    dump.open("dump-st-" + std::to_string(payload_size) + "-" +std::to_string(outstanding_req) + ".txt");

    int start_range = 0;
    TIMESTAMP_T last_received;
    GET_TIMESTAMP(last_received);

    for (size_t i = 0; i < timestamps_ranges.size(); i++) {
      auto [last_id, timestamp] = timestamps_ranges[i];

      for (int j = start_range; j < last_id; j++) {
        last_received = timestamp;
        dump << ELAPSED_NSEC(timestamps_start[j], timestamp) << "\n";
      }

      if (start_range < last_id) {
        start_range = last_id;
      }
    }

    dump.close();

    double n_commands = static_cast<double>(times);
    double elapsed_time = static_cast<double>(ELAPSED_NSEC(timestamps_start[0], last_received));
    double time_per_op = elapsed_time / n_commands;
    double throughput = n_commands*payload_size/elapsed_time  * 1000 *1000 *1000 /1024 /1024/1024; 
    std::cout << "Replicated " << times << " commands of size " << payload_size
              << " bytes in " << elapsed_time << " ns"
              << std::endl;

    std::cout << "Throughput = " << throughput << "Go/s"<< std::endl;
    std::cout << "Average latency of one op = " << time_per_op/1000<< "µs"<<std::endl;


    exit(0);
  }
}
