#include <algorithm>
#include <array>
#include <atomic>
#include <sstream>

#include "exchanger.hpp"

namespace dory {

int ConnectionExchanger :: num_conn = 0;

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
  /*
  rcs.insert(
      std::pair<int, ReliableConnection>(proc_id, ReliableConnection(cb)));

  auto& rc = rcs.find(proc_id)->second;


  rc.bindToPD(pd);
  rc.bindToMR(mr);  
  rc.associateWithCQ(send_cq_name, recv_cq_name);*/
  //LOGGER_INFO(logger, "configure was called ==> redirecting to configure_with_cm");
  configure_with_cm(proc_id, pd, mr, send_cq_name, recv_cq_name);
}

void ConnectionExchanger::configure_all(std::string const& pd,
                                        std::string const& mr,
                                        std::string send_cq_name,
                                        std::string recv_cq_name) {
  /*for (auto const& id : remote_ids) {
    configure(id, pd, mr, send_cq_name, recv_cq_name);
  }*/
  //LOGGER_INFO(logger, "configure all was called ==> redirecting to configure_all_with_cm");
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
  /*loopback_ = std::make_unique<ReliableConnection>(cb);
  loopback_->bindToPD(pd);
  loopback_->bindToMR(mr);
  loopback_->associateWithCQ(send_cq_name, recv_cq_name);*/
  //LOGGER_INFO(logger, "Add Loopback  was called ==> redirecting to AddLoopback_with_cm()");
  addLoopback_with_cm(pd, mr, send_cq_name, recv_cq_name);
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
  remote_loopback_->associateWithCQ_for_cm_prel(send_cq_name, recv_cq_name);
  //LOGGER_INFO(logger, "Remote LoopBack added with_cm ");

  loopback_port = 80000;
}

void ConnectionExchanger::connectLoopback(ControlBlock::MemoryRights rights) {
  /*auto infoForRemoteParty = loopback_->remoteInfo();
  loopback_->init(rights);
  loopback_->connect(infoForRemoteParty);
  LOGGER_INFO(logger, "Loopback connection was established");*/
  //LOGGER_INFO(logger, "Loopback connect was called ==> redirecting to connectLoopBack_with_cm");
  connectLoopback_with_cm(rights);
}


void ConnectionExchanger::connectLoopback_with_cm(ControlBlock::MemoryRights rights) {
  std::thread client_thread(&ConnectionExchanger::threaded_client,this,rights);
  client_thread.detach();

  start_loopback_server(rights);
  //LOGGER_INFO(logger, "Loopback connected with cm !");
}



void ConnectionExchanger :: threaded_client(ControlBlock::MemoryRights rights){
  //-attendre quelques secondes
  //std :: cout << "Client thread sleeping" << std :: endl;
  std::this_thread::sleep_for(std::chrono::seconds(2));

  //se comporter comme un client ! 
  start_loopback_client(rights);
  /*
  while(1){
    std :: cout << "Client thread sleeping for 10s" << std :: endl;
    std::this_thread::sleep_for(std::chrono::seconds(10));
  } // just doing nothing, until the main thread makes it stop*/
}

int ConnectionExchanger :: start_loopback_server(ControlBlock::MemoryRights rights){
  struct sockaddr_in server_addr;
  struct rdma_cm_event *cm_event = NULL;
	int ret = -1;
  
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  
  std::string str_ip;
  switch(my_id){
    case 1:
      str_ip="10.30.2.1";
      break;
    case 2:
      str_ip="10.30.2.2";
      break;
    case 3:
      str_ip="10.30.2.3";
      break;
    default:
      std::cout << "IP of this node ?";
      std::cin >> str_ip;
      break;
  }

  char* char_ip = new char[str_ip.length() + 1];
  strcpy(char_ip, str_ip.c_str()); 
  ret = get_addr(char_ip, reinterpret_cast<struct sockaddr*>(&server_addr));
  if (ret) {
    throw std::runtime_error("Wrong input");
    return ret;
  }
  delete[] char_ip;

  server_addr.sin_port = htons(static_cast<uint16_t>(loopback_port));

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
      cm_params.private_data = loopback_->getLocalSetup(); 
      cm_params.private_data_len = 24;
      cm_params.retry_count = 1;
      rdma_accept(loopback_->get_cm_id(), &cm_params); 

    
      loopback_->setRemoteSetup(cm_event->param.conn.private_data); 
      loopback_->print_all_infos();

      /*Une fois que la connection est bien finie, on ack l'event du début*/
      ret = rdma_ack_cm_event(cm_event);
      if (ret) {
        throw std::runtime_error("Failed to acknowledge the cm event");
        return -1;
      }
      LOGGER_INFO(logger,"A new RDMA client connection is set up");      
      break; //on sort de là, car on n'attend qu'un client 
   } while(1);

	return ret;
}



int ConnectionExchanger :: start_loopback_client(ControlBlock::MemoryRights rights){
  struct sockaddr_in server_addr;
  struct rdma_cm_event *cm_event = NULL;

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  
  
  std::string str_ip;
  switch(my_id){
    case 1:
      str_ip="10.30.2.1";
      break;
    case 2:
      str_ip="10.30.2.2";
      break;
    case 3:
      str_ip="10.30.2.3";
      break;
    default:
      std::cout << "IP of the server ?";
      std::cin >> str_ip;
      break;
  }

  //conversion du string en char pour utiliser get_addr (qui provient de rdma_common de Baptiste)
  char* char_ip = new char[str_ip.length() + 1];
  strcpy(char_ip, str_ip.c_str()); 
  int ret = get_addr(char_ip, reinterpret_cast<struct sockaddr*>(&server_addr));
  if (ret) {
    throw std::runtime_error("Wrong input");
    return ret;
  }
  delete[] char_ip;
  server_addr.sin_port = htons(static_cast<uint16_t>(loopback_port));
  
  remote_loopback_->configure_cm_channel();

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
  cm_params.private_data = remote_loopback_->getLocalSetup();
  cm_params.private_data_len = 24;
  cm_params.retry_count = 1;
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
    
  remote_loopback_->print_all_infos();


  LOGGER_INFO(logger, "The Loopback client is connected successfully \n");
 
  return 0 ;
}



void ConnectionExchanger::announce(int proc_id, MemoryStore& store,
                                   std::string const& prefix) {
  
  /*
  auto& rc = rcs.find(proc_id)->second;

  std::stringstream name;
  name << prefix << "-" << my_id << "-for-" << proc_id;
  auto infoForRemoteParty = rc.remoteInfo();
  store.set(name.str(), infoForRemoteParty.serialize());
  LOGGER_INFO(logger, "Publishing qp {}", name.str());*/
  LOGGER_INFO(logger, "announce() was called ==> does nothing");
}

void ConnectionExchanger::announce_all(MemoryStore& store,
                                       std::string const& prefix) {
  /*for (int pid : remote_ids) {
    announce(pid, store, prefix);
  }*/
  //LOGGER_INFO(logger, "announce_all() was called ==> does nothing");
}

void ConnectionExchanger::announce_ready(MemoryStore& store,
                                         std::string const& prefix,
                                         std::string const& reason) {
  /*std::stringstream name;
  name << prefix << "-" << my_id << "-ready(" << reason << ")";
  store.set(name.str(), "ready(" + reason + ")");*/
  //LOGGER_INFO(logger, "announce_ready() was called ==> does nothing");
}

void ConnectionExchanger::wait_ready(int proc_id, MemoryStore& store,
                                     std::string const& prefix,
                                     std::string const& reason) {
  /*auto packed_reason = "ready(" + reason + ")";
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
  }*/
  //LOGGER_INFO(logger, "wait_ready() was called ==> does nothing");
}

void ConnectionExchanger::wait_ready_all(MemoryStore& store,
                                         std::string const& prefix,
                                         std::string const& reason) {
  /*for (int pid : remote_ids) {
    wait_ready(pid, store, prefix, reason);
  }*/
  //LOGGER_INFO(logger, "wait_ready_all() was called ==> does nothing");
}

void ConnectionExchanger::connect(int proc_id, MemoryStore& store,
                                  std::string const& prefix,
                                  ControlBlock::MemoryRights rights) {
  /*auto& rc = rcs.find(proc_id)->second;

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
  LOGGER_INFO(logger, "Connected with {}", name.str());*/
  //LOGGER_INFO(logger, "connect() was called ==> redirecting to connect_with_cm()");
  connect_with_cm(proc_id, prefix, rights);

}

void ConnectionExchanger:: connect_with_cm(int proc_id,
                                  std::string const& prefix,
                                  ControlBlock::MemoryRights rights){
  //fetching the RC associated with the remote id
  auto& rc = rcs.find(proc_id)->second;

  std::stringstream print_conn;
  print_conn << "[NEW CONNECTION] Handling the connection of "<< my_id << "-to-"<< proc_id;
  LOGGER_INFO(logger, "{}", print_conn.str());
  
  /*Manual input */
  /*
  std::string rdma_mode; 
  std :: cout << "Client or server ?   "; // Type a number and press enter
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
  }*/

  /*Dans le cas où il y a trois replica : qui fait quoi (serveur ou client) déterminé arbitrairement
      node 1 toujours le serveur 
      node 3 toujours le client 
      node 2 une fois le client (avec node 1) et une fois le serveur (avec node 2) 
      */
  if (my_id == 1 || (my_id ==3 && proc_id ==2)){
    start_server(proc_id, rights);  //va initialiser toutes les ressources, attendre pour la connection, et faire tout le reste
    //ça fait un gros bloc qui fait tout (pas terrible)
  }
  else {
    start_client(proc_id, rights);
  }
}

/* Starts an RDMA server by allocating basic (CM) connection resources */
int ConnectionExchanger:: start_server(int proc_id,ControlBlock::MemoryRights rights) {
  struct sockaddr_in server_addr;
  struct rdma_cm_event *cm_event = NULL;
	int ret = -1;
  
  /*Initialisation cm channel */
  auto& rc = rcs.find(proc_id)->second;
  rc.configure_cm_channel();

  /*On donne les infos sur l'IP du server*/
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  
  /*Manual input of the IP*/
  /*
  std::string str_ip;
  std::cout << "IP of this node ?";
  std::cin >> str_ip;*/
  
  std::string str_ip;
  switch(my_id){
    case 1:
      str_ip="10.30.2.1";
      break;
    case 2:
      str_ip="10.30.2.2";
      break;
    case 3:
      str_ip="10.30.2.3";
      break;
    default:
      std::cout << "IP of this node ?";
      std::cin >> str_ip;
      break;
  }

  //conversion du string en char pour utiliser get_addr (qui provient de rdma_common de Baptiste)
  char* char_ip = new char[str_ip.length() + 1];
  strcpy(char_ip, str_ip.c_str()); 
  ret = get_addr(char_ip, reinterpret_cast<struct sockaddr*>(&server_addr));
  if (ret) {
    throw std::runtime_error("Wrong input");
    return ret;
  }
  delete[] char_ip;

  server_addr.sin_port = htons(static_cast<uint16_t>((20886 + get_num_conn())));
  incr_num_conn();

  /* Explicit binding of rdma cm id to the socket credentials */
	ret = rdma_bind_addr(rc.get_cm_listen_id(), reinterpret_cast<struct sockaddr*>(&server_addr));
	if (ret) {
    throw std::runtime_error("Failed to bind the channel to the addr");
		return -1;
	}
	//LOGGER_INFO(logger, "Server RDMA CM id is successfully binded ");


  /*Listening for incoming events*/
  ret = rdma_listen(rc.get_cm_listen_id(), 8); /* backlog = 8 clients, same as TCP*/
	if (ret) {
    throw std::runtime_error("rdma_listen failed to listen on server address");
		return -1;
	}
	printf("Server is listening successfully at: %s , port: %d \n",inet_ntoa(server_addr.sin_addr),
          ntohs(server_addr.sin_port));


  
  do {
      ret = process_rdma_cm_event(rc.get_event_channel(),RDMA_CM_EVENT_CONNECT_REQUEST,&cm_event);
      //printf("Caught something ! ");
      if (ret) {continue;}
      
      rc.set_cm_id(cm_event->id);
      rc.associateWithCQ_for_cm();
      rc.set_init_with_cm(rights);

      //Accepter la connexion
      struct rdma_conn_param cm_params;
      memset(&cm_params, 0, sizeof(cm_params));
      cm_params.private_data = rc.getLocalSetup(); //dirty hack, vient mettre les infos de la mr de rc dans cm_params
      cm_params.private_data_len = 24;
      cm_params.retry_count = 1;
      rdma_accept(rc.get_cm_id(), &cm_params); 

    
      rc.setRemoteSetup(cm_event->param.conn.private_data); //dirty hack : on récupère les info (addr et rkey) de la remote qp.
      rc.print_all_infos();

      /*Une fois que la connection est bien finie, on ack l'event du début*/
      ret = rdma_ack_cm_event(cm_event);
      if (ret) {
        throw std::runtime_error("Failed to acknowledge the cm event");
        return -1;
      }
      LOGGER_INFO(logger,"A new RDMA client connection is set up");      
      break; //on sort de là, car on n'attend qu'un client 
   } while(1);

	return ret;
}


int ConnectionExchanger:: start_client(int proc_id, ControlBlock::MemoryRights rights){
  //destination à renseigner 
  struct sockaddr_in server_addr;
  struct rdma_cm_event *cm_event = NULL;

  /*On donne les infos sur l'IP du server qu'on cherche à atteindre*/
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  
  
  std::string str_ip;
  /*
  std::cout << "IP of the server ?";
  std::cin >> str_ip;*/
  
  switch(proc_id){
    case 1:
      str_ip="10.30.2.1";
      break;
    case 2:
      str_ip="10.30.2.2";
      break;
    case 3:
      str_ip="10.30.2.3";
      break;
    default:
      std::cout << "IP of the server ?";
      std::cin >> str_ip;
      break;
  }

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
  std::cout << "Port number of the server ? ";
  std::cin >> port_serv;
  server_addr.sin_port = htons(static_cast<uint16_t>(port_serv));
  
  auto& rc = rcs.find(proc_id)->second;

  rc.configure_cm_channel();

  ret = rdma_resolve_addr(rc.get_cm_listen_id(), NULL, reinterpret_cast<struct sockaddr*>(&server_addr), 2000);
  if (ret) {
		throw std::runtime_error("Failed to resolve address");
		exit(-1);
	}
  
  //LOGGER_INFO(logger, "waiting for cm event: RDMA_CM_EVENT_ADDR_RESOLVED\n");
  //printf("cm_id's verbs : %p \n,", reinterpret_cast<void*>(cm_id->verbs) );
  ret  = process_rdma_cm_event(rc.get_event_channel(),RDMA_CM_EVENT_ADDR_RESOLVED,	&cm_event);
	if (ret) {
		throw std::runtime_error("Failed to receive a valid event");
		exit(-1);
	}
	
  /* we ack the event */
	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		throw std::runtime_error("Failed to acknowledge the CM event");
		exit(-1);
	}
  //LOGGER_INFO(logger, "RDMA address is resolved \n");

	 /* Resolves an RDMA route to the destination address in order to
	  * establish a connection */
	ret = rdma_resolve_route(rc.get_cm_listen_id(), 2000);
	if (ret) {
		throw std::runtime_error("Failed to resolve route");
	   exit(-1);
	}
  //LOGGER_INFO(logger, "waiting for cm event: RDMA_CM_EVENT_ROUTE_RESOLVED\n");
  //printf("cm_id's verbs : %p \n,", reinterpret_cast<void*>(cm_id->verbs) );
	ret = process_rdma_cm_event(rc.get_event_channel(),RDMA_CM_EVENT_ROUTE_RESOLVED,&cm_event);
	if (ret) {
		throw std::runtime_error("Failed to receive a valid event");
		exit(-1);
	}

	/* we ack the event */
	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		throw std::runtime_error("Failed to acknowledge the CM event");
		exit(-1);
	}

	//printf("Trying to connect to server at : %s port: %d \n",inet_ntoa(server_addr.sin_addr),ntohs(server_addr.sin_port));
  
  /* Creating the QP */      
  rc.set_cm_id(rc.get_cm_listen_id()); //dans le cas du serveur, il n'y a plus de dinstinction entre cm_id et cm_listen_id
  rc.associateWithCQ_for_cm();
  rc.set_init_with_cm(rights);
  /*Connecting*/
  struct rdma_conn_param cm_params;
  memset(&cm_params, 0, sizeof(cm_params));
  cm_params.private_data = rc.getLocalSetup();
  cm_params.private_data_len = 24;
  cm_params.retry_count = 1;
  rdma_connect(rc.get_cm_id(), &cm_params);

  //LOGGER_INFO(logger, "waiting for cm event: RDMA_CM_EVENT_ESTABLISHED\n");
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
    
  rc.print_all_infos();


  LOGGER_INFO(logger, "The client is connected successfully \n");
 
  return 0 ;
}

void ConnectionExchanger::connect_all(MemoryStore& store,
                                      std::string const& prefix,
                                      ControlBlock::MemoryRights rights) {
  /*
  for (int pid : remote_ids) {
    connect(pid, store, prefix, rights);
  }*/
  //LOGGER_INFO(logger, "connect_all() called...redirecting to connect_all_with_cm() \n");
  connect_all_with_cm(store, prefix, rights);
}

void ConnectionExchanger::connect_all_with_cm(MemoryStore& store,
                                      std::string const& prefix,
                                      ControlBlock::MemoryRights rights){	
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
		//LOGGER_INFO(logger,"CM event has non zero status");
		ret = -((*cm_event)->status);
		/* important, we acknowledge the event */
		rdma_ack_cm_event(*cm_event);
		return ret;
	}
	
  /* if it was a good event, was it of the expected type */
	if ((*cm_event)->event != expected_event) {
		//LOGGER_INFO(logger,"Received event {}, TODO: handle!\n",
		//		rdma_event_str((*cm_event)->event));
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

/*Simple counter, for the ports */
int ConnectionExchanger :: get_num_conn(){return num_conn;}

void ConnectionExchanger :: incr_num_conn(){num_conn++;}

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
