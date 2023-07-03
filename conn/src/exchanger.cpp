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
  Du coup, la QP de rc sera crée plus tard
  Pour l'instant, on configure juste les attributs, pour plus tard */
  rc.associateWithCQ_for_cm(send_cq_name, recv_cq_name);
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
  
  /*
  std::string str_ip;
  std::cout << "What's the IP of this node ? (running as a server)";
  std::cin >> str_ip;
  ret = get_addr(str_ip, (struct sockaddr*)server_addr);
  if (ret) {
    throw std::runtime_error("Wrong input");
    return ret;
  }
  */
	
  
  /* Explicit binding of rdma cm id to the socket credentials */
	ret = rdma_bind_addr(cm_id, (struct sockaddr*)&server_addr);
	if (ret) {
    throw std::runtime_error("Failed to bind the channel to the addr");
		return -1;
	}
	LOGGER_INFO(logger, "Server RDMA CM id is successfully binded \n");
	
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
      if (ret) {
         continue; //en cas d'erreur, on recommence un coup dans la boucle 
      }
      
      /*METTRE LE RESTE DE LA CONNECTION ICI*/
      
      //(optionnel) vérifier que le contexte de l'event et celui du server sont identiques (le contexte est en privé dans cb.resolved_port, c'est pas urgent)
      
      /*On fetch la RC associée à proc_id*/
      auto& rc = rcs.find(proc_id)->second;

      ret = rdma_create_qp(cm_event->id,rc.get_pd(), rc.get_init_attr() );
      if (ret) {
        throw std::runtime_error("Failed to create QP due to errno");
        return -1;
      }
      
      //Poster quelques receive buffers 


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
