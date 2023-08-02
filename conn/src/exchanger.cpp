#include <algorithm>
#include <array>
#include <atomic>
#include <sstream>

#include "exchanger.hpp"

namespace dory {

ConnectionExchanger::ConnectionExchanger(int my_id, std::vector<int> remote_ids,
                                         ControlBlock& cb)
    : my_id{my_id}, remote_ids{remote_ids}, cb{cb}, LOGGER_INIT(logger, "CE") {
  auto [valid, maximum_id] = valid_ids();
  if (!valid) {
    throw std::runtime_error(
        "Ids are not natural numbers/reasonably contiguous");
  }

  max_id = maximum_id;

  std::string path_to_config = "/dory/config.txt"; //WARNING : change this if needed 
  ifs.open(path_to_config);
  if (!ifs.is_open()) {
      std::cerr << "Error opening configuration file." << std::endl;
  }

  std::string line;
  while (std::getline(ifs, line)) {
    size_t delimiterPos = line.find('=');
    if (delimiterPos != std::string::npos) {
        int key =std::stoi(line.substr(0, delimiterPos));
        std::string value = line.substr(delimiterPos + 1, line.length());
        ipAddresses.insert(std::pair<int, std::string>(key, value));
    }
  }

  ifs.close();

  std::cout << "Successfully built the ConnectionExchanger object" << std::endl;
  std::cout << "IPs read : ";
  for (int i = 1 ; i <= max_id; i++){
    std::cout << "Node " << i << ":" << ipAddresses[i] << "; ";
  }
  std::cout << std::endl;


}

void ConnectionExchanger::configure(int proc_id, std::string const& pd,
                                    std::string const& mr,
                                    std::string send_cq_name,
                                    std::string recv_cq_name) {
  configure_with_cm(proc_id, pd, mr, send_cq_name, recv_cq_name);
}

void ConnectionExchanger::configure_all(std::string const& pd,
                                        std::string const& mr,
                                        std::string send_cq_name,
                                        std::string recv_cq_name) {
  configure_all_with_cm(pd, mr, send_cq_name, recv_cq_name);
}


void ConnectionExchanger::configure_with_cm(int proc_id, std::string const& pd,
                                    std::string const& mr,
                                    std::string send_cq_name,
                                    std::string recv_cq_name) {
  rcs.insert(
      std::pair<int, ReliableConnection>(proc_id, ReliableConnection(cb)));

  auto& rc = rcs.find(proc_id)->second;

  rc.bindToPD(pd);
  rc.bindToMR(mr);
  /*Quand on utilise le CM, on doit créer la qp avec rdma_qp après avoir reçu l'event rdma
  Du coup, la QP de rc sera crée plus tard*/
  rc.associateWithCQ_for_cm_prel(send_cq_name, recv_cq_name);
  }

void ConnectionExchanger::configure_all_with_cm(std::string const& pd,
                                        std::string const& mr,
                                        std::string send_cq_name,
                                        std::string recv_cq_name) { 
  for (auto const& id : remote_ids) {
    configure_with_cm(id, pd, mr, send_cq_name, recv_cq_name);
  }
}

void ConnectionExchanger::addLoopback(std::string const& pd,
                                      std::string const& mr,
                                      std::string send_cq_name,
                                      std::string recv_cq_name) {

  addLoopback_with_cm(pd, mr, send_cq_name, recv_cq_name);
}

void ConnectionExchanger::connectLoopback(ControlBlock::MemoryRights rights) {
  connectLoopback_with_cm(rights);
}


void ConnectionExchanger::addLoopback_with_cm(std::string const& pd,
                                      std::string const& mr,
                                      std::string send_cq_name,
                                      std::string recv_cq_name) {
  loopback_ = std::make_unique<ReliableConnection>(cb);
  loopback_->bindToPD(pd);
  loopback_->bindToMR(mr);
  loopback_->associateWithCQ_for_cm_prel(send_cq_name, recv_cq_name);
  //LOGGER_INFO(logger, "LoopBack added with_cm ");
  
  //remote_loopback est identique à loopback
  remote_loopback_ = std::make_unique<ReliableConnection>(cb);
  remote_loopback_->bindToPD(pd);
  remote_loopback_->bindToMR(mr);
  remote_loopback_->associateWithCQ_for_cm_prel(send_cq_name, recv_cq_name); //pas sûr de ça, peut-être que ça double les wc pour rien
  //LOGGER_INFO(logger, "Remote LoopBack added with_cm ");

  loopback_port = 80000;
}

void ConnectionExchanger::connectLoopback_with_cm(ControlBlock::MemoryRights rights) {
  std::thread client_thread(&ConnectionExchanger::start_loopback_client,this,rights); //lancer le remote_loopback_, qui va jouer le rôle de client 
  client_thread.detach();
  start_loopback_server(rights); //loopback_ joue le rôle de serveur 
}



int ConnectionExchanger :: start_loopback_server(ControlBlock::MemoryRights rights){
  struct rdma_cm_event *cm_event = NULL;
	int ret = -1;
  
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  std::string str_ip = ipAddresses[my_id];
  ret = get_addr(str_ip, reinterpret_cast<struct sockaddr*>(&server_addr));
  if (ret) {
    throw std::runtime_error("Wrong input");
    return ret;
  }

  server_addr.sin_port = htons(static_cast<uint16_t>(loopback_port));
  std::cout << "loopback_server() called with my_port = "<< loopback_port <<std::endl;

  loopback_->configure_cm_channel();

	ret = rdma_bind_addr(loopback_->get_cm_listen_id(), reinterpret_cast<struct sockaddr*>(&server_addr));
	if (ret) {
    throw std::runtime_error("Failed to bind the channel to the addr");
		return -1;
	}

  ret = rdma_listen(loopback_->get_cm_listen_id(), 8); /* backlog = 8 clients, same as TCP*/
	if (ret) {
    throw std::runtime_error("rdma_listen failed to listen on server address");
		return -1;
	}
	printf("Loopback (server) is listening successfully at: %s , port: %d \n",inet_ntoa(server_addr.sin_addr),
          ntohs(server_addr.sin_port));

  
  do {
      ret = process_rdma_cm_event(loopback_->get_event_channel(),RDMA_CM_EVENT_CONNECT_REQUEST,&cm_event);
      if (ret) {continue;}
      
      loopback_->set_cm_id(cm_event->id);
      loopback_->associateWithCQ_for_cm();
      loopback_->set_init_with_cm(rights);

      struct rdma_conn_param cm_params;
      memset(&cm_params, 0, sizeof(cm_params));
      build_conn_param(&cm_params);
      cm_params.private_data_len = 24;
      cm_params.private_data = loopback_->getLocalSetup();       
      rdma_accept(loopback_->get_cm_id(), &cm_params); 

      loopback_->setRemoteSetup(cm_event->param.conn.private_data); 
      
      /*Une fois que la connection est bien finie, on ack l'event du début*/
      ret = rdma_ack_cm_event(cm_event);
      if (ret) {
        throw std::runtime_error("Failed to acknowledge the cm event");
        return -1;
      }
      LOGGER_INFO(logger,"A new RDMA client connection is set up");      
      break; 
   } while(1);

	return ret;
}

int ConnectionExchanger :: start_loopback_client(ControlBlock::MemoryRights rights){
  struct rdma_cm_event *cm_event = NULL;

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  std::string str_ip = ipAddresses[my_id];
  int ret = get_addr(str_ip, reinterpret_cast<struct sockaddr*>(&server_addr));
  if (ret) {
    throw std::runtime_error("Wrong input");
    return ret;
  }

  server_addr.sin_port = htons(static_cast<uint16_t>(loopback_port));
  std::cout << "loopback_client() called with dest_port = "<< loopback_port <<std::endl;
  
  remote_loopback_->configure_cm_channel();
  
  //resolve addr
  ret = rdma_resolve_addr(remote_loopback_->get_cm_listen_id(), NULL, reinterpret_cast<struct sockaddr*>(&server_addr), 2000);
  if (ret) {
		throw std::runtime_error("Failed to resolve address");
		exit(-1);
	}
  ret  = process_rdma_cm_event(remote_loopback_->get_event_channel(),RDMA_CM_EVENT_ADDR_RESOLVED,	&cm_event);
	if (ret) {
		throw std::runtime_error("Failed to receive a valid event");
		exit(-1);
	}
  ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		throw std::runtime_error("Failed to acknowledge the CM event");
		exit(-1);
	}

  //resolve route
  ret = rdma_resolve_route(remote_loopback_->get_cm_listen_id(), 2000);
	if (ret) {
		throw std::runtime_error("Failed to resolve route");
	   exit(-1);
	}  
  ret = process_rdma_cm_event(remote_loopback_->get_event_channel(),RDMA_CM_EVENT_ROUTE_RESOLVED,&cm_event);
	if (ret) {
		throw std::runtime_error("Failed to receive a valid event");
		exit(-1);
	}
	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		throw std::runtime_error("Failed to acknowledge the CM event");
		exit(-1);
	}
	
  
  printf("[LoopBack client] Trying to connect to server at : %s port: %d \n",inet_ntoa(server_addr.sin_addr),ntohs(server_addr.sin_port));
  /* Creating the QP */      
  remote_loopback_->set_cm_id(remote_loopback_->get_cm_listen_id()); //dans le cas du serveur, il n'y a plus de dinstinction entre cm_id et cm_listen_id
  remote_loopback_->associateWithCQ_for_cm();
  remote_loopback_->set_init_with_cm(rights);
  /*Connecting*/
  struct rdma_conn_param cm_params;
  memset(&cm_params, 0, sizeof(cm_params));
  build_conn_param(&cm_params);
  cm_params.private_data_len = 24;
  cm_params.private_data = remote_loopback_->getLocalSetup();
  rdma_connect(remote_loopback_->get_cm_id(), &cm_params);
  //LOGGER_INFO(logger, "waiting for cm event: RDMA_CM_EVENT_ESTABLISHED\n");
  ret = process_rdma_cm_event(remote_loopback_->get_event_channel(), RDMA_CM_EVENT_ESTABLISHED,&cm_event);
  if (ret) {
		throw std::runtime_error("Failed to receive a valid event");
    return ret;
  } 
  remote_loopback_->setRemoteSetup(cm_event->param.conn.private_data);
  ret = rdma_ack_cm_event(cm_event);
  if (ret) {
		throw std::runtime_error("Failed to acknowledge the CM event");
    return -errno;
  }
  LOGGER_INFO(logger, "The Loopback client is connected successfully \n");
 
  return 0 ;
}


void ConnectionExchanger::announce(int proc_id, MemoryStore& store,
                                   std::string const& prefix) {
}

void ConnectionExchanger::announce_all(MemoryStore& store,
                                       std::string const& prefix) {
}

void ConnectionExchanger::announce_ready(MemoryStore& store,
                                         std::string const& prefix,
                                         std::string const& reason) {

}

void ConnectionExchanger::wait_ready(int proc_id, MemoryStore& store,
                                     std::string const& prefix,
                                     std::string const& reason) {
}

void ConnectionExchanger::wait_ready_all(MemoryStore& store,
                                         std::string const& prefix,
                                         std::string const& reason) {
}

void ConnectionExchanger::connect(int proc_id, MemoryStore& store,
                                  std::string const& prefix,
                                  ControlBlock::MemoryRights rights) {
  LOGGER_INFO(logger, "connect() was called ==> does nothing");
}


void ConnectionExchanger:: start_server(int proc_id, int my_port, ControlBlock::MemoryRights rights) {
  struct rdma_cm_event *cm_event = NULL;
	int ret = -1;
  
  auto& rc = rcs.find(proc_id)->second;
  rc.configure_cm_channel();

  //setting up the server 
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;

  std::string str_ip = ipAddresses[my_id];
  ret = get_addr(str_ip, reinterpret_cast<struct sockaddr*>(&server_addr));
  if (ret) {
    throw std::runtime_error("Wrong input");
    return;
  }
  std::cout << "start_server() called with my_port = "<< my_port <<std::endl;
  server_addr.sin_port = htons(static_cast<uint16_t>(my_port));

  // Explicit binding of rdma cm id to the socket credentials 
	ret = rdma_bind_addr(rc.get_cm_listen_id(), reinterpret_cast<struct sockaddr*>(&server_addr));
	if (ret) {
    throw std::runtime_error("Failed to bind the channel to the addr");
		return;
	}
	LOGGER_INFO(logger, "Server RDMA CM id is successfully binded ");


  /*Listening for incoming events*/
  ret = rdma_listen(rc.get_cm_listen_id(), 8); /* backlog = 8 clients, same as TCP*/
	if (ret) {
    throw std::runtime_error("rdma_listen failed to listen on server address");
		return;
	}
	printf("Server is listening successfully at: %s , port: %d \n",inet_ntoa(server_addr.sin_addr), ntohs(server_addr.sin_port));
  
  while(1){
    std::cout << "Waiting of an event" << std::endl;
    ret = process_rdma_cm_event(rc.get_event_channel(),RDMA_CM_EVENT_CONNECT_REQUEST,&cm_event);
    std::cout << "got something" << std::endl;
    if (ret) {continue;}
    std::cout << "received connect request" << std::endl;
    
    rc.set_cm_id(cm_event->id);
    rc.associateWithCQ_for_cm();
    rc.set_init_with_cm(rights);

    std::cout << "rc set" << std::endl;

    //Accepter la connexion
    struct rdma_conn_param cm_params;
    memset(&cm_params, 0, sizeof(cm_params));
    build_conn_param(&cm_params);
    cm_params.private_data_len = 24;
    cm_params.private_data = rc.getLocalSetup();


    std::cout << "cm_params ready" << std::endl;

    rdma_accept(rc.get_cm_id(), &cm_params); 
    
    std::cout << "accepted ! " << std::endl;

    rc.setRemoteSetup(cm_event->param.conn.private_data); //dirty hack : on récupère les info (addr et rkey) de la remote qp.
    
    
    /*Une fois que la connection est bien finie, on ack l'event du début*/
    ret = rdma_ack_cm_event(cm_event);
    if (ret) {
      throw std::runtime_error("Failed to acknowledge the cm event");
      return;
    }
    LOGGER_INFO(logger,"A new RDMA client connection is set up");      
    break; //on sort de là, car on n'attend qu'un client 
  }

	return;
}


int ConnectionExchanger:: start_client(int proc_id, int dest_port, ControlBlock::MemoryRights rights){
  //destination à renseigner 
  struct sockaddr_in server_addr;
  struct rdma_cm_event *cm_event = NULL;

  /*On donne les infos sur l'IP du server qu'on cherche à atteindre*/
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  
  std::string str_ip = ipAddresses[proc_id];
  int ret = get_addr(str_ip, reinterpret_cast<struct sockaddr*>(&server_addr));
  if (ret) {
    throw std::runtime_error("Wrong input");
    return ret;
  }

  server_addr.sin_port = htons(static_cast<uint16_t>(dest_port));
  std::cout << "start_client() called with dest_port = "<< dest_port <<std::endl;


  auto& rc = rcs.find(proc_id)->second;
  rc.configure_cm_channel();

  ret = rdma_resolve_addr(rc.get_cm_listen_id(), NULL, reinterpret_cast<struct sockaddr*>(&server_addr), 2000);
  if (ret) {
		throw std::runtime_error("Failed to resolve address");
		exit(-1);
	}
  ret  = process_rdma_cm_event(rc.get_event_channel(),RDMA_CM_EVENT_ADDR_RESOLVED,	&cm_event);
	if (ret) {
		throw std::runtime_error("Failed to receive a valid event");
		exit(-1);
	}
  ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		throw std::runtime_error("Failed to acknowledge the CM event");
		exit(-1);
	}
  LOGGER_INFO(logger, "RDMA address is resolved \n");

	ret = rdma_resolve_route(rc.get_cm_listen_id(), 2000);
	if (ret) {
		throw std::runtime_error("Failed to resolve route");
	   exit(-1);
	}
 	ret = process_rdma_cm_event(rc.get_event_channel(),RDMA_CM_EVENT_ROUTE_RESOLVED,&cm_event);
	if (ret) {
		throw std::runtime_error("Failed to receive a valid event");
		exit(-1);
	}
	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		throw std::runtime_error("Failed to acknowledge the CM event");
		exit(-1);
	}
  LOGGER_INFO(logger, "RDMA route is resolved \n");
  printf("Trying to connect to server at : %s port: %d \n",inet_ntoa(server_addr.sin_addr),ntohs(server_addr.sin_port));
  
  /* Creating the QP */      
  rc.set_cm_id(rc.get_cm_listen_id()); //dans le cas du serveur, il n'y a plus de dinstinction entre cm_id et cm_listen_id
  rc.associateWithCQ_for_cm();
  rc.set_init_with_cm(rights);
  
  /*Connecting*/
  struct rdma_conn_param cm_params;
  memset(&cm_params, 0, sizeof(cm_params));
  build_conn_param(&cm_params);
  cm_params.private_data_len = 24;
  cm_params.private_data = rc.getLocalSetup();
  rdma_connect(rc.get_cm_id(), &cm_params);

  LOGGER_INFO(logger, "waiting for cm event: RDMA_CM_EVENT_ESTABLISHED\n");
  ret = process_rdma_cm_event(rc.get_event_channel(), RDMA_CM_EVENT_ESTABLISHED,&cm_event);
  if (ret) {
		throw std::runtime_error("Failed to receive a valid event");
    return ret;
  } 
  rc.setRemoteSetup(cm_event->param.conn.private_data);
  ret = rdma_ack_cm_event(cm_event);
  if (ret) {
		throw std::runtime_error("Failed to acknowledge the CM event");
    return -errno;
  }
  LOGGER_INFO(logger, "The client is connected successfully \n");
 
  return 0 ;
}


/*
  There are max_id (=N) nodes. We want to connect all of them to each other.
  That's a total of (N-1) + ... + 1 connexions. 
  We proceed in N-1 rounds :
    -at round 1, node 1 acts as a server for all the (N-1) connexions it expects
      So node 1 is connected with everyone, and is done ! 
    -at round 2, node 2 acts as a server for all the (N-2) connexions it expects (since it's already connected to node 1),
      So node 1 is connected with everyone, and is done !
    ...
    -at round i, node i acts as a server for all the (N-i) connexions it expects, 
      So node i is connected with everyone, and is done ! 
    ...
    -at round N-1, node N-1 acts as a server for the 1 connexion it expects
      So everybody is connected to each other ! 

  So, when a node isn't acting as a server, it's either :
    -acting as a client who's job is to connect to the server (if round_number > the node's id)
    -doing nothing, because it's done (if round_number < the node's id)

  As an arbitrary rule : 
    If node X is acting as a server, and is expecting node Y to connect to it as a client, 
    node X will listen to Y's message on the port (p + Y), with p an arbitrary int given
  

*/
void ConnectionExchanger::connect_all(MemoryStore& store,
                                      std::string const& prefix, //this remains from mu's original code, but we don't use it
                                      int base_port,
                                      ControlBlock::MemoryRights rights) {
  std::cout << "max_id " << max_id <<std::endl;  

  for (int round = 1; round < max_id; round++){
    std::cout << "round "<< round << std::endl;
    if (my_id < round){
      return; //this node is done ! 
    }
    else if (my_id == round){  //it's this node's turn to act as a server 
      int num_threads = max_id - my_id;
      std::vector<std::thread> threads;
      for (int j=0; j < num_threads; j++){
        threads.emplace_back(&ConnectionExchanger::start_server, 
                              this, 
                              my_id+j+1, //the remote node I'm expecting 
                              base_port + my_id+j+1,       //the port number where I'm listening for that node
                              rights);
      }    
      for (auto& thread : threads) {
        thread.join(); //the node waits for all its connexions to be done before continuing 
      }
    }
    else if (my_id > round){ //this node must act as a client 
      std::this_thread::sleep_for(std::chrono::seconds(2)); //wait for the server to set-up 
      start_client(round, base_port + my_id, rights);
    }
  }
}



int ConnectionExchanger :: get_addr(std::string dst, struct sockaddr *addr){
	char* char_ip = new char[dst.length() + 1];
  strcpy(char_ip, dst.c_str()); 
  
  struct addrinfo *res;
	int ret = -1;
	ret = getaddrinfo(char_ip, NULL, NULL, &res);
	if (ret) {
		throw std::runtime_error("Error when fetching getaddrinfo ");
		return ret;
	}
	memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
	freeaddrinfo(res);
  delete char_ip;
	return ret;
}

int ConnectionExchanger :: process_rdma_cm_event(struct rdma_event_channel *echannel,enum rdma_cm_event_type expected_event,struct rdma_cm_event **cm_event){

  int ret = 1;
	ret = rdma_get_cm_event(echannel, cm_event); //blocking call
	if (ret) {
		LOGGER_INFO(logger,"Failed to retrieve a cm event");
		return -1;
	}
	
  /* lets see, if it was a good event */
	if(0 != (*cm_event)->status){
		LOGGER_INFO(logger,"CM event has non zero status");
		ret = -((*cm_event)->status);
		/* important, we acknowledge the event */
		rdma_ack_cm_event(*cm_event);
		return ret;
	}
	
  /* if it was a good event, was it of the expected type */
	if ((*cm_event)->event != expected_event) {
		LOGGER_INFO(logger,"Received event {}, TODO: handle!\n",
				rdma_event_str((*cm_event)->event));
		/* important, we acknowledge the event */
		rdma_ack_cm_event(*cm_event);
		return -1; // unexpected event :(
	}
	LOGGER_INFO(logger,"A new {} type event is received \n", rdma_event_str((*cm_event)->event));
	/* The caller must acknowledge the event */
	return ret;
}

void ConnectionExchanger :: show_rdma_cmid(struct rdma_cm_id *id){
	if(!id){
		throw std::runtime_error("Passed pointer is null ");
		return;
	}
	printf("RDMA cm id at %p \n", reinterpret_cast<void*>(id));
	if(id->verbs && id->verbs->device)
		printf("dev_ctx: %p (device name: %s) \n", reinterpret_cast<void*>(id->verbs),
				id->verbs->device->name);
	if(id->channel)
		printf("cm event channel %p\n", reinterpret_cast<void*>(id->channel));
	printf("QP: %p, port_space %x, port_num %u \n", reinterpret_cast<void*>(id->qp),
			id->ps,
			id->port_num);
}


void ConnectionExchanger :: check_all_qp_states(){
  for ( auto &[pid, rc] : rcs){
    std::cout << "The QP connected to " << pid << "is in state :" << rc.query_qp_state() << std::endl;
  }
}

std::pair<bool, int> ConnectionExchanger::valid_ids() const {
  auto min_max_remote =
      std::minmax_element(remote_ids.begin(), remote_ids.end());
  auto min = std::min(*min_max_remote.first, my_id);
  auto max = std::max(*min_max_remote.second, my_id);

  if (min < 1) {
    return std::make_pair(false, 0);
  }

  if (double(max) > gapFactor * static_cast<double>((remote_ids.size() + 1))) {
    return std::make_pair(false, 0);
  }

  return std::make_pair(true, max);
}


void ConnectionExchanger::build_conn_param(rdma_conn_param *cm_params){
  cm_params->retry_count = 1;
  cm_params->responder_resources = 14;
  cm_params->initiator_depth = 14;
  cm_params->rnr_retry_count = 12;
}
}  // namespace dory
