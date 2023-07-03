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
}

void ConnectionExchanger::configure(int proc_id, std::string const& pd,
                                    std::string const& mr,
                                    std::string send_cq_name,
                                    std::string recv_cq_name) {
  rcs.insert(
      std::pair<int, ReliableConnection>(proc_id, ReliableConnection(cb)));

  auto& rc = rcs.find(proc_id)->second;


  rc.bindToPD(pd);
  rc.bindToMR(mr);  
  rc.associateWithCQ(send_cq_name, recv_cq_name);
}

void ConnectionExchanger::configure_all(std::string const& pd,
                                        std::string const& mr,
                                        std::string send_cq_name,
                                        std::string recv_cq_name) {
  for (auto const& id : remote_ids) {
    configure(id, pd, mr, send_cq_name, recv_cq_name);
  }
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
  /*Quand on utilise le CM, on doit créer la qp avec rdma_qp après avoir reçu l'event
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
  loopback_ = std::make_unique<ReliableConnection>(cb);
  loopback_->bindToPD(pd);
  loopback_->bindToMR(mr);
  loopback_->associateWithCQ(send_cq_name, recv_cq_name);

  LOGGER_INFO(logger, "Loopback connection was added");
}

void ConnectionExchanger::connectLoopback(ControlBlock::MemoryRights rights) {
  auto infoForRemoteParty = loopback_->remoteInfo();
  loopback_->init(rights);
  loopback_->connect(infoForRemoteParty);

  LOGGER_INFO(logger, "Loopback connection was established");
}

void ConnectionExchanger::announce(int proc_id, MemoryStore& store,
                                   std::string const& prefix) {
  
  LOGGER_INFO(logger, "Inside announce all");
  auto& rc = rcs.find(proc_id)->second;

  std::stringstream name;
  name << prefix << "-" << my_id << "-for-" << proc_id;
  auto infoForRemoteParty = rc.remoteInfo();
  store.set(name.str(), infoForRemoteParty.serialize());
  LOGGER_INFO(logger, "Publishing qp {}", name.str());

  /*
  std::string info_supp;
  info_supp = "key=(" + name.str() + "); value=(" + infoForRemoteParty.serialize() + ")";
  LOGGER_INFO(logger, "[GILLOU]remote info published : {}", info_supp); */
}

void ConnectionExchanger::announce_all(MemoryStore& store,
                                       std::string const& prefix) {
  for (int pid : remote_ids) {
    announce(pid, store, prefix);
  }
}

void ConnectionExchanger::announce_ready(MemoryStore& store,
                                         std::string const& prefix,
                                         std::string const& reason) {
  std::stringstream name;
  name << prefix << "-" << my_id << "-ready(" << reason << ")";
  store.set(name.str(), "ready(" + reason + ")");
}

void ConnectionExchanger::wait_ready(int proc_id, MemoryStore& store,
                                     std::string const& prefix,
                                     std::string const& reason) {
  auto packed_reason = "ready(" + reason + ")";
  std::stringstream name;
  name << prefix << "-" << proc_id << "-" << packed_reason;

  auto key = name.str();
  std::string value;

  while (!store.get(key, value)) {
    std::this_thread::sleep_for(retryTime);
  }

  if (value != packed_reason) {
    throw std::runtime_error("Ready announcement of message `" + key +
                             "` does not contain the value `" + packed_reason +
                             "`");
  }
}

void ConnectionExchanger::wait_ready_all(MemoryStore& store,
                                         std::string const& prefix,
                                         std::string const& reason) {
  for (int pid : remote_ids) {
    wait_ready(pid, store, prefix, reason);
  }
}

void ConnectionExchanger::connect(int proc_id, MemoryStore& store,
                                  std::string const& prefix,
                                  ControlBlock::MemoryRights rights) {
  auto& rc = rcs.find(proc_id)->second;

  std::stringstream name;
  name << prefix << "-" << proc_id << "-for-" << my_id;

  std::string ret_val;
  if (!store.get(name.str(), ret_val)) {
    LOGGER_DEBUG(logger, "Could not retrieve key {}", name.str());

    throw std::runtime_error("Cannot connect to remote qp " + name.str());
  }

  auto remoteRC = dory::RemoteConnection::fromStr(ret_val);

  rc.init(rights);
  rc.connect(remoteRC);
  LOGGER_INFO(logger, "Connected with {}", name.str());
}

void ConnectionExchanger:: connect_with_cm(int proc_id,
                                  std::string const& prefix,
                                  ControlBlock::MemoryRights rights){
  //fetching the RC associated with the remote id
  auto& rc = rcs.find(proc_id)->second;

  std::stringstream str_print;
  str_print << "Handling the connection of "<< my_id << "-to-"<< proc_id;
  LOGGER_INFO(logger, "[Gillou debug] {}", str_print.str());

  std::string rdma_mode; 
  std :: cout << "Which mode : client or server ? "; // Type a number and press enter
  std :: cin >> rdma_mode; // Get user input from the keyboard

  if (rdma_mode == "server"){
    start_server(proc_id);  //va initialiser toutes les ressources, attendre pour la connection, et faire tout le reste
    //ça fait un gros bloc qui fait tout (pas terrible)
  }
  else if (rdma_mode == "client"){
    start_client(proc_id);
  }
  else{ 
    throw std::runtime_error("Wrong input");
  }
}

/* Starts an RDMA server by allocating basic (CM) connection resources */
int ConnectionExchanger:: start_server(int proc_id) {
  struct sockaddr_in server_addr;
  struct rdma_cm_event *cm_event = NULL;
	int ret = -1;
  
  /*On donne les infos sur l'IP du server*/
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  
  std::string str_ip;
  std::cout << "What's the IP of this node ? (running as a server)";
  std::cin >> str_ip;

  //conversion du string en char pour utiliser get_addr (qui provient de rdma_common de Baptiste)
  char* char_ip = new char[str_ip.length() + 1];
  strcpy(char_ip, str_ip.c_str()); 
  ret = get_addr(char_ip, reinterpret_cast<struct sockaddr*>(&server_addr));
  if (ret) {
    throw std::runtime_error("Wrong input");
    return ret;
  }
  delete[] char_ip;
  
  server_addr.sin_port = htons(20886);
  
  /* Explicit binding of rdma cm id to the socket credentials */
	ret = rdma_bind_addr(cm_id, reinterpret_cast<struct sockaddr*>(&server_addr));
	if (ret) {
    throw std::runtime_error("Failed to bind the channel to the addr");
		return -1;
	}
	LOGGER_INFO(logger, "Server RDMA CM id is successfully binded ");
	

  /*Listening for incoming events*/
  ret = rdma_listen(cm_id, 8); /* backlog = 8 clients, same as TCP*/
	if (ret) {
    throw std::runtime_error("rdma_listen failed to listen on server address");
		return -1;
	}
	printf("Server is listening successfully at: %s , port: %d \n",inet_ntoa(server_addr.sin_addr),
          ntohs(server_addr.sin_port));

  /*Même si on ne s'attend qu'à une connexion, on met un while pour être persistant*/
  do {
      ret = process_rdma_cm_event(cm_event_channel,RDMA_CM_EVENT_CONNECT_REQUEST,&cm_event);
      //on veut un CONNECT_REQUEST
      if (ret) {
         continue; //en cas d'erreur, on recommence un coup dans la boucle 
      }
      //(à faire optionnel) vérifier que le contexte de l'event et celui du server sont identiques (le contexte est dans cb.resolved_port)
      
      /*On fetch la RC associée à proc_id*/
      auto& rc = rcs.find(proc_id)->second;
      
      
      show_rdma_cmid(cm_id);
      show_rdma_cmid(cm_event->id);

      rc.associateWithCQ_for_cm(cm_event->id);


      //TO DO :Poster quelques receive buffers 


      //Accepter la connexion
      struct rdma_conn_param cm_params;
      memset(&cm_params, 0, sizeof(cm_params));
      rdma_accept(cm_event->id, &cm_params); 


      /*Une fois que la connection est bien finie, on ack l'event du début*/
      ret = rdma_ack_cm_event(cm_event);
      if (ret) {
        throw std::runtime_error("Failed to acknowledge the cm event");
        return -1;
      }
      LOGGER_INFO(logger,"A new RDMA client connection id is stored");
      break; //on sort de là, car la connection est stored comme il faut 
   } while(1);

	return ret;
}


int ConnectionExchanger:: start_client(int proc_id){
  //destination à renseigner 
  struct sockaddr_in server_addr;
  struct rdma_cm_event *cm_event = NULL;
  struct rdma_cm_event event_copy;

  /*On donne les infos sur l'IP du server qu'on cherche à atteindre*/
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  
  std::string str_ip;
  std::cout << "What's the IP of the server ? (this node is running as a client)";
  std::cin >> str_ip;

  //conversion du string en char pour utiliser get_addr (qui provient de rdma_common de Baptiste)
  char* char_ip = new char[str_ip.length() + 1];
  strcpy(char_ip, str_ip.c_str()); 
  int ret = get_addr(char_ip, reinterpret_cast<struct sockaddr*>(&server_addr));
  if (ret) {
    throw std::runtime_error("Wrong input");
    return ret;
  }
  delete[] char_ip;
  
  int port_serv;
  std::cout << "What's the port number of the server ? (this node is running as a client)";
  std::cin >> port_serv;
  server_addr.sin_port = htons(port_serv);
  

  ret = rdma_resolve_addr(cm_id, NULL, reinterpret_cast<struct sockaddr*>(&server_addr), 2000);
	if (ret) {
		throw std::runtime_error("Failed to resolve address");
		exit(-1);
	}
  LOGGER_INFO(logger, "waiting for cm event: RDMA_CM_EVENT_ADDR_RESOLVED\n");
  ret  = process_rdma_cm_event(cm_event_channel,RDMA_CM_EVENT_ADDR_RESOLVED,	&cm_event);
	if (ret) {
		throw std::runtime_error("Failed to receive a valid event");
		exit(-1);
	}
	/* we ack the event */
	
	memcpy(&event_copy, cm_event, sizeof(*cm_event));      
  ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		throw std::runtime_error("Failed to acknowledge the CM event");
		exit(-1);
	}
  LOGGER_INFO(logger, "RDMA address is resolved \n");

	 /* Resolves an RDMA route to the destination address in order to
	  * establish a connection */
	ret = rdma_resolve_route(event_copy.id, 2000);
	if (ret) {
		throw std::runtime_error("Failed to resolve route");
	   exit(-1);
	}
  LOGGER_INFO(logger, "waiting for cm event: RDMA_CM_EVENT_ROUTE_RESOLVED\n");

	ret = process_rdma_cm_event(cm_event_channel,RDMA_CM_EVENT_ROUTE_RESOLVED,&cm_event);
	if (ret) {
		throw std::runtime_error("Failed to receive a valid event");
		exit(-1);
	}

	/* we ack the event */
	memcpy(&event_copy, cm_event, sizeof(*cm_event));      
  ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		throw std::runtime_error("Failed to acknowledge the CM event");
		exit(-1);
	}


	printf("Trying to connect to server at : %s port: %d \n",inet_ntoa(server_addr.sin_addr),
			ntohs(server_addr.sin_port));

  auto& rc = rcs.find(proc_id)->second;

  /* Creating the QP */
        
  show_rdma_cmid(cm_id);
  show_rdma_cmid(event_copy.id);


  rc.associateWithCQ_for_cm(event_copy.id);

  /*Connecting*/
  struct rdma_conn_param cm_params;
  memset(&cm_params, 0, sizeof(cm_params));
  rdma_connect(event_copy.id, &cm_params);

  LOGGER_INFO(logger, "waiting for cm event: RDMA_CM_EVENT_ESTABLISHED\n");
  ret = process_rdma_cm_event(cm_event_channel,RDMA_CM_EVENT_ESTABLISHED,&cm_event);
  if (ret) {
		throw std::runtime_error("Failed to receive a valid event");
    return ret;
  }ret = rdma_ack_cm_event(cm_event);
  if (ret) {
		throw std::runtime_error("Failed to acknowledge the CM event");
    return -errno;
  }

  LOGGER_INFO(logger, "The client is connected successfully \n");
 
  return 0 ;
}

void ConnectionExchanger::connect_all(MemoryStore& store,
                                      std::string const& prefix,
                                      ControlBlock::MemoryRights rights) {
  for (int pid : remote_ids) {
    connect(pid, store, prefix, rights);
  }
}

void ConnectionExchanger::connect_all_with_cm(MemoryStore& store,
                                      std::string const& prefix,
                                      ControlBlock::MemoryRights rights){
  /*Le CM event channel sera commun à toutes les connexions
  On le crée et on lui donne un id une seule fois, peu importe notre nombre de connexion
  */
  cm_event_channel = rdma_create_event_channel();
	if (!cm_event_channel) {
    throw std::runtime_error("Creating cm event channel failed");
		return;
	}
  LOGGER_INFO(logger, "RDMA CM event channel is created successfully");

	int ret = rdma_create_id(cm_event_channel, &cm_id, NULL, RDMA_PS_TCP);
	if (ret) {
    throw std::runtime_error("Creating cm id failed");
		return;
	}
  LOGGER_INFO(logger, "A RDMA connection id for the server is created ");
	
  for (int pid : remote_ids) {
    connect_with_cm(pid, prefix, rights);
  }
}

int ConnectionExchanger :: get_addr(char *dst, struct sockaddr *addr){
	struct addrinfo *res;
	int ret = -1;
	ret = getaddrinfo(dst, NULL, NULL, &res);
	if (ret) {
		throw std::runtime_error("Error when fetching getaddrinfo ");
		return ret;
	}
	memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
	freeaddrinfo(res);
	return ret;
}

int ConnectionExchanger :: process_rdma_cm_event(struct rdma_event_channel *echannel,
		enum rdma_cm_event_type expected_event,
		struct rdma_cm_event **cm_event){
	
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
	printf("RDMA cm id at %p \n", id);
	if(id->verbs && id->verbs->device)
		printf("dev_ctx: %p (device name: %s) \n", id->verbs,
				id->verbs->device->name);
	if(id->channel)
		printf("cm event channel %p\n", id->channel);
	printf("QP: %p, port_space %x, port_num %u \n", id->qp,
			id->ps,
			id->port_num);
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
}  // namespace dory
