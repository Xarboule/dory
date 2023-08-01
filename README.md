# Microsecond Consensus for Microsecond Applications - Adapted for RoCE v2.0

This repository provides the source code of Mu (https://github.com/LPD-EPFL/mu) modified to work on an RDMA network (IB, ROCE v1.0 and v2.0). 


## Running with docker 
Our deployment comprises of 4 machines that run over an RDMA network. We will refer to these machines as `node1, node2, node3, node4` (or `node{1,2,3,4}` for brevity).
In each one of these machines, we deploy a docker container with the necessary dependencies for building the software stack. 


### Running the docker container
In each machine :
   * make sure to be in a directory that contains the Dockerfile and the script `sleep.sh`. 
   * inside that directory, build the image from the Dockerfile. 
   * run the container. It will use the network of the host (network == host), in detached mode (-d), and with privileges (--privileged, so that the container can have access to the NIC).
   * run a bash inside the container.

For example, in node-1, the commands are the following : 
```sh
$ docker build -t mu_img .
$ docker run -d --privileged --name mu-node-1 --hostname node-1 --network host mu_img
$ docker exec -it mu-node-1 bash
```

### Building the code in the container 
Once inside the running container, first download the source code : 
```sh
$ git clone https://github.com/GillesHOP/dory.git
$ cd dory/
```

In the file config.txt, enter the IP addresses of node-1, node-2 and node-3 :
```text
1=10.30.2.1
2=10.30.2.2
2=10.30.2.3
```

Then, simply execute one of the following scripts : 
* Either compilation.sh
* Or compile_debug.sh

If you get the error "Error opening configuration file", then change the variable path_to_config in dory/conn/src/exchanger.cpp, in the constructor of ConnectionExchanger.

### Running without docker 
(Better performance expected, however some bug ==> need ubuntu 18.04) 
Same steps as with docker : see the content of the dockerfile for the dependencies.

   
## Execution of the various experiments
First, make sure that you are at the `~/dory` directory.

### Setting up the connections during a run 
For all the experiments, some inputs are required during the connection set-up. 
When node-X and node-Y establish an RDMA connection, one of those nodes will act as a server, and the other as a client. 
On both nodes, a print in the terminal will indicate "Handling of connection X-to-Y". 
The server will print "listening successfully at: ..., port : ...."
On the client side, the user will have to manually input the port of the server.

Since there are 3 nodes involved (the fourth being a client connected to the leader by TCP), and 2 connections between each paire of nodes (background plane and replication plane), the user will have to prompt 6 times.

(TO DO : automate the set-up) 

A commun error when a node acts as a server is "Failed to bind the channel to the addr". It  means that another process is already occupying the port. Usually, it is another run of Mu that wasn't killed proporly. So to solve this issue, just kill that other instance. 


### MAIN-ST (Standalone throughput)
On `node{1,2,3}` run:
```sh
$ export DORY_REGISTRY_IP=10.30.2.4:9999
```

On `node1` run:
```sh
$ ./crash-consensus/demo/using_conan_fully/build/bin/main-st 1 4096 1
```

On `node2` run:
```sh
$ ./crash-consensus/demo/using_conan_fully/build/bin/main-st 2 4096 1
```

On `node3` run:
```sh
$ ./crash-consensus/demo/using_conan_fully/build/bin/main-st 3 4096 1
```

**Notes**:
* The argument list is `<process_id> <payload_size> <nr_of_outstanding_reqs>`
* Make sure to spawn all instances of `main-st` at (*almost*) the same time.
* When the execution of `main-st` finishes on node 1, the terminal will print `Replicated X commands of size Y bytes in Z ns`.
* After the experiment completes on node 1, make sure to kill (e.g., using `Ctrl-C`) the experiment on nodes 2 and 3.

### MAIN-ST-LAT (Standalone latency)
On `node{1,2,3}` run:
```sh
$ export DORY_REGISTRY_IP=10.30.2.4:9999
```

On `node1` run:
```sh
$ ./crash-consensus/demo/using_conan_fully/build/bin/main-st-lat 1 4096 1
```

On `node2` run:
```sh
$ ./crash-consensus/demo/using_conan_fully/build/bin/main-st-lat 2 4096 1
```

On `node3` run:
```sh
$ ./crash-consensus/demo/using_conan_fully/build/bin/main-st-lat 3 4096 1
```

**Notes**:
* The argument list is `<process_id> <payload_size> <nr_of_outstanding_reqs>`
* Make sure to spawn all instances of `main-st-lat` at (*almost*) the same time.
* When execution of `main-st-lat` in node1 finishes, a file named `dump-st-4096-1.txt` is created that stores latency measurements in *ns*.
* After the experiment completes on node 1, make sure to kill (e.g., using `Ctrl-C`) the experiment on nodes 2 and 3.

### REDIS (Replicated Redis)
On `node{1,2,3}` run:
```sh
$ export DORY_REGISTRY_IP=10.30.2.4:9999
$ export IDS=1,2,3
```

On `node1` run:
```sh
$ export SID=1
$ ./crash-consensus/experiments/redis/bin/redis-server-replicated --port 6379
```

On `node2` run:
```sh
$ export SID=2
$ ./crash-consensus/experiments/redis/bin/redis-server-replicated --port 6379
```

On `node3` run:
```sh
$ export SID=3
$ ./crash-consensus/experiments/redis/bin/redis-server-replicated --port 6379
```

On `node4`:
```sh
$ # Wait enough time for the pumps to start. You will see the message `Reading pump started` in nodes 2 and 3.
$ ./crash-consensus/experiments/redis/bin/redis-puts-only 16 32 osdi-node-1 6379
```

Notes:
* Syntax: `./redis-puts-only <key_size> <value_size> <redis_leader_server_host> <redis_leader_server_port>`
* After execution of `redis-puts-only` finishes, a file named `redis-cli.txt` appears in node4. This files contains end2end latency from client's perspective. You can get the latency that the leader (server) spend in replicating the request by ssh-ing into node1, and sending the command `kill -SIGUSR1 <pid>`. The `<pid>` corresponds to the PID of the redis process and it gets printed when `redis-server-replicated` starts. A file name `dump-1.txt` will appear in the filesystem after sending the signal.
* Apart from `redis-puts-only`, the client can also run `redis-gets-only` or `redis-puts-gets`.
* To get the baseline measurements (original redis without replication), you can run `numactl --membind 0 -- ./crash-consensus/experiments/redis/bin/redis-server --port 6379` on `node1`, don't run anything on `node2`, `node3` and execute the previously shown command on `node4`.
* At the end of the experiment, make sure to kill (e.g., using `Ctrl-C`) the `redis-server-replicated` processes on nodes 1, 2, and 3.

### REDIS (Replicated Redis) --- INTERACTIVE
On `node{1,2,3}` run:
```sh
$ export DORY_REGISTRY_IP=10.30.2.4:9999
$ export IDS=1,2,3
```

On `node1` run:
```sh
$ export SID=1
$ numactl --membind 0 -- ./crash-consensus/experiments/redis/bin/redis-server-replicated --port 6379
```

On `node2` run:
```sh
$ export SID=2
$ numactl --membind 0 -- ./crash-consensus/experiments/redis/bin/redis-server-replicated --port 6379
```

On `node3` run:
```sh
$ export SID=3
$ numactl --membind 0 -- ./crash-consensus/experiments/redis/bin/redis-server-replicated --port 6379
```

Wait until the message `Reading pump started` appears in nodes 2 and 3. You can now start a Redis client on node 4, issue SET commands to node 1, and verify that these commands are replicated to nodes 2 and 3. For example, on `node4`:
```sh
$ # Wait enough time for the pumps to start. You will see the message `Reading pump started` in nodes 2 and 3.
$ ./crash-consensus/experiments/redis/bin/redis-cli -h 10.30.2.1 -p 6379
osdi-node-1:6379> SET CH Bern
OK
osdi-node-1:6379> SET IT Rome
OK
$ ./crash-consensus/experiments/redis/bin/redis-cli -h 10.30.2.2 -p 6379
osdi-node-2:6379> GET CH
"Bern"
```

Notes:
* The last command is replicated but not committed: in the example above, if you run `GET IT` when the client is connected to node 2, it will return `(nil)`. This is because of the piggybacking mechanism; as explained in the paper, the commit indication for a command is transmitted when the next command is replicated. 
* At the end of the experiment, make sure to kill (e.g., using `Ctrl-C`) the `redis-server-replicated` processes on nodes 1, 2, and 3, as well as the client on node 4.


### MEMCACHED (replicated memcached):
On `node{1,2,3}` run:
```sh
$ export DORY_REGISTRY_IP=10.30.2.4:9999
$ export IDS=1,2,3
```

On `node1` run:
```sh
$ export SID=1
$ numactl --membind 0 -- ./crash-consensus/experiments/memcached/bin/memcached-replicated -p 6379
```

On `node2` run:
```sh
$ export SID=2
$ numactl --membind 0 -- ./crash-consensus/experiments/memcached/bin/memcached-replicated -p 6379
```

On `node3` run:
```sh
$ export SID=3
$ numactl --membind 0 -- ./crash-consensus/experiments/memcached/bin/memcached-replicated -p 6379
```

On `node4`:
```sh
$ # Wait enough time for the pumps to start. You will see the message `Reading pump started` in nodes 2 and 3.
$ ./crash-consensus/experiments/memcached/bin/memcached-puts-only 16 32 osdi-node-1 6379
```

**Notes**:
* Syntax: `./memcached-puts-only <key_size> <value_size> <memcached_leader_server_host> <memcacched_leader_server_port>`
* After execution of `memcached-puts-only` finishes, a file named `memcached-cli.txt` appears in node4. This files contains end2end latency from client's perspective. You can get the latency that the leader (server) spend in replicating the request by ssh-ing into node1, and sending the command `kill -SIGUSR1 <pid>`. The `<pid>` corresponds to the PID of the memcached process and it gets printed when `memcached-replicated` starts. A file name `dump-1.txt` will appear in the filesystem after sending the signal.
* Apart from `memcached-puts-only`, the client can also run `memcached-gets-only` or `memcached-puts-gets`.
* To get the baseline measurements (original memcached without replication), you can run `numactl --membind 0 -- ./crash-consensus/experiments/memcached/bin/memcached -p 6379` on `node1`, don't run anything on `node2`, `node3` and execute the previously shown command on `node4`.
* At the end of the experiment, make sure to kill (e.g., using `Ctrl-C`) the `memcached-replicated` processes on nodes 1, 2, and 3.


### LIQUIBOOK (Stock trading application)
On `node{1,2,3,4}` run:
```sh
$ export SERVER_URIS=1=1030.2.1:31850,2=10.30.2.2:31850,3=10.30.2.3:31850
$ export TRADERS_NUM=1
```
On `node{1,2,3}` run:
```sh
$ export DORY_REGISTRY_IP=10.30.2.4:9999
$ export MODE=server
```

On `node1` run:
```sh
$ export URI=10.30.2.1:31850
$ ./crash-consensus/experiments/liquibook/eRPC/build/liquibook --test_ms 120000 --sm_verbose 0 --num_processes 4 --numa_0_ports 0 --process_id 0 --numa_node 0
```

On `node2` run:
```sh
$ export URI=10.30.2.2:31850
$ ./crash-consensus/experiments/liquibook/eRPC/build/liquibook --test_ms 120000 --sm_verbose 0 --num_processes 4 --numa_0_ports 0 --process_id 1 --numa_node 0
```

On `node3` run:
```sh
$ export URI=10.30.2.3:31850
$ ./crash-consensus/experiments/liquibook/eRPC/build/liquibook --test_ms 120000 --sm_verbose 0 --num_processes 4 --numa_0_ports 0 --process_id 2 --numa_node 0
```

Make sure to run `liquibook` on `nodes{1,2,3}` at (*almost*) the same time. Wait for `liquibook` to append the stock **AAPL** at its book and then do the following:

On `node4` run:
```sh
$ export MODE=client
$ export URI=10.30.2.4:31850
$ export TRADER_ID=0
$ ./crash-consensus/experiments/liquibook/eRPC/build/liquibook --test_ms 120000 --sm_verbose 0 --num_processes 4 --numa_0_ports 0 --process_id 3 --numa_node 0
```

On `node1`, in a separte ssh connection (under directory `~/dory`) run:
```sh
$ ./crash-consensus/demo/using_conan_fully/build/bin/fifo /tmp/fifo-1
```
This provides a way to pause the first leader and force a leader switch. To do this, type `p` and hit `Enter`. Cause the hiccup only after the client has been connected.



## Starting over
In case you want recompile and execute the software stack in a system with all the system-wide dependencies installed, the do the following in `nodes{1,2,3,4}`.
```sh
$ # Run `ps aux`, then find all the processes that are related to the software stack and kill them
$ rm -rf ~/dory
$ conan remove --force "*"
```
Optionally, exit the shell and login again in order to unload the exported environment variables set by the various experiments.
Subsequently, re-follow the document starting from section *Complication of the sofware stack*.


## Known limitations 
* Since we use rmda_cm to handle the state of the QPs, we cannot easily change these states with ibv_modify(). That can cause problems because Mu might change the state of a QP to change its permissions. The way Mu changes its permissions when changing a leader is quite straightforward : it changes the QP's access flag in an optimistic attempt, and if it fails (which might happen), it uses the slower but more robust method of changing directly the QP's state. So this second method is node yet implemented here. (With rdma_cm, instead of change the QP's state, we would more likely delete it than (re)create it by calling rdma_create_qp id, while hoping that it's allowed. Since rdma_cm_id is already set up, we might avoid to have to synchronize each QP connected to each other.)
