#include <cstring>
#include <stdexcept>

#include "rc.hpp"
#include "wr-builder.hpp"


namespace dory {

ReliableConnection::ReliableConnection(ControlBlock &cb)
    : cb{cb}, pd{nullptr}, LOGGER_INIT(logger, "RC") {
  memset(&create_attr, 0, sizeof(struct ibv_qp_init_attr));
  create_attr.qp_type = IBV_QPT_RC;
  create_attr.cap.max_send_wr = WRDepth;
  create_attr.cap.max_recv_wr = WRDepth;
  create_attr.cap.max_send_sge = SGEDepth;
  create_attr.cap.max_recv_sge = SGEDepth;
  create_attr.cap.max_inline_data = MaxInlining;
}

void ReliableConnection::bindToPD(std::string pd_name) {
  pd = cb.pd(pd_name).get();
}

void ReliableConnection::bindToMR(std::string mr_name) { mr = cb.mr(mr_name); }

// TODO(Kristian): creation of qp should be rather separated?
void ReliableConnection::associateWithCQ(std::string send_cp_name,
                                         std::string recv_cp_name) {
  create_attr.send_cq = cb.cq(send_cp_name).get();
  create_attr.recv_cq = cb.cq(recv_cp_name).get();

  auto qp = ibv_create_qp(pd, &create_attr);

  if (qp == nullptr) {
    throw std::runtime_error("Could not create the queue pair");
  }

  uniq_qp = deleted_unique_ptr<struct ibv_qp>(qp, [](struct ibv_qp *qp) {
    auto ret = ibv_destroy_qp(qp);
    if (ret != 0) {
      throw std::runtime_error("Could not query device: " +
                               std::string(std::strerror(errno)));
    }
  });
  //std :: cout << "QP created with ibv_create_qp() ==> Be careful about its state !" << std :: endl;
}

/*Pour créer la QP, il faut renseigner des infos dans le champ create_attr de
cette instance de ReliableConnection. Ici, on s'occupe des champs cq (send et
recv) Pour passer par CM, on finalisera la création de qp une fois le cm_id en
place*/
void ReliableConnection::associateWithCQ_for_cm_prel(std::string send_cp_name,
                                                     std::string recv_cp_name) {
  create_attr.send_cq = cb.cq(send_cp_name).get();
  create_attr.recv_cq = cb.cq(recv_cp_name).get();
}



/*Une fois le connection manager prêt, on peut enfin créer une qp
C'est comme ça qu'on évite de devoir nous même utiliser les ibv_modify_qp()*/
void ReliableConnection::associateWithCQ_for_cm() {
  //cm_id->verbs = pd->context; 

  int ret = rdma_create_qp(cm_id, pd, &create_attr);

  if (ret) {
    printf("Failed to create QP due to errno: %s\n", strerror(errno));
    throw std::runtime_error("Failed to create QP due to ...");
    return;
  }

  /*Copié-collé de associateWithCQ() pour renseigner uniq_qp
  Il est notamment utilisé dans les send et receive */
  auto qp = cm_id->qp;
  uniq_qp = deleted_unique_ptr<struct ibv_qp>(qp, [](struct ibv_qp *qp) {
    auto ret = ibv_destroy_qp(qp);
    if (ret != 0) {
      throw std::runtime_error("Could not query device: " + std::string(std::strerror(errno)));
    }
  });
  //LOGGER_INFO(logger, "QP successfully created (with cm)! ");

  // Copié-collé de la fin de la fonction connect()
  struct ibv_send_wr *wr_ = reinterpret_cast<ibv_send_wr *>( aligned_alloc(64, roundUp(sizeof(ibv_send_wr), 64) + sizeof(ibv_sge)));
  struct ibv_sge *sg_ = reinterpret_cast<ibv_sge *>( reinterpret_cast<char *>(wr_) + roundUp(sizeof(ibv_send_wr), 64));

  memset(sg_, 0, sizeof(*sg_));
  memset(wr_, 0, sizeof(*wr_));

  wr_->sg_list = sg_;
  wr_->num_sge = 1;
  wr_->send_flags |= IBV_SEND_SIGNALED;

  sg_->lkey = mr.lkey;
  wr_->wr.rdma.rkey = rconn.rci.rkey;

  wr_cached = deleted_unique_ptr<struct ibv_send_wr>(wr_, wr_deleter); 


}

void ReliableConnection::reset() {
  printf("ATTENTION appel d'une fonction de RC non implémentée reset()\n");
  /*struct ibv_qp_attr attr;
  memset(&attr, 0, sizeof(attr));

  attr.qp_state = IBV_QPS_RESET;

  auto ret = ibv_modify_qp(uniq_qp.get(), &attr, IBV_QP_STATE);

  if (ret != 0) {
    throw std::runtime_error("Could not modify QP to RESET: " +
                             std::string(std::strerror(errno)));
  }
*/

}


void ReliableConnection :: set_init_with_cm(ControlBlock :: MemoryRights rights){
    //std :: cout << "On initialise l'access right de la QP ! " << std :: endl;
    struct ibv_qp_attr init_attr;
    memset(&init_attr, 0, sizeof(struct ibv_qp_attr));
    init_attr.qp_access_flags = rights;

    auto ret = ibv_modify_qp(uniq_qp.get(), &init_attr, IBV_QP_ACCESS_FLAGS);

    if (ret != 0) {
      throw std::runtime_error("Failed to change the access flag of QP : " +
                               std::string(std::strerror(errno)));
    }

    init_rights = rights;
    //std:: cout <<"State de la QP : " << this->query_qp_state() << std::endl;
}

void ReliableConnection::init(ControlBlock::MemoryRights rights) {
  printf("ATTENTION appel d'une fonction de RC qui change l'état: init() ==> ne fait rien  \n");
}

void ReliableConnection::reinit() { 
  printf("ATTENTION appel d'une fonction de RC interdite: reinit() ==> ne fait rien\n");
}


void ReliableConnection::connect(RemoteConnection &rc) {
  printf("ATTENTION appel d'une fonction de RC interdite: connect() ==> ne fait rien\n");
}

bool ReliableConnection::needsReset() {
  
  printf("ATTENTION appel d'une fonction de RC interdite: needsReset()\n");
  struct ibv_qp_attr attr;
  struct ibv_qp_init_attr init_attr;

  if (ibv_query_qp(uniq_qp.get(), &attr, IBV_QP_STATE, &init_attr)) {
    throw std::runtime_error("Failed to query QP state: " +
                             std::string(std::strerror(errno)));
  }

  return attr.qp_state == IBV_QPS_RTS;
}

bool ReliableConnection::changeRights(ControlBlock::MemoryRights rights) {
  printf("ATTENTION : changeRight est appelé, ce qui peut faire planter la QP\n");
  struct ibv_qp_attr attr;
  memset(&attr, 0, sizeof(attr));

  //std::cout << "some infos about the QP before the modify() : " << std::endl;
  //this->print_all_infos();
    

  attr.qp_access_flags = static_cast<unsigned>(rights);

  auto ret = ibv_modify_qp(uniq_qp.get(), &attr, IBV_QP_ACCESS_FLAGS);

  if (ret == 0){
    std::cout << "changeRights() worked ! " << std::endl;
  }else{
    std::cout << "changeRights() seems to have failed, with ret = " << static_cast<int>(ret) << std::endl;
    //std::cout << "This QP's state : " <<  query_qp_state() <<std::endl;
    return false;
  }



  return ret == 0;
}

bool ReliableConnection::changeRightsIfNeeded(
    ControlBlock::MemoryRights rights) {
  //printf("ATTENTION appel d'une fonction de RC non travaillée: changeRightsIfNeeded()\n");
  auto converted_rights = static_cast<unsigned>(rights);

  struct ibv_qp_attr attr;
  struct ibv_qp_init_attr init_attr;

  if (ibv_query_qp(uniq_qp.get(), &attr, IBV_QP_STATE | IBV_QP_ACCESS_FLAGS,
                   &init_attr)) {
    throw std::runtime_error("Failed to query QP state: " + std::string(std::strerror(errno)));
  }

  if (attr.qp_state != IBV_QPS_RTS) {
    return false;
  }

  if (static_cast<unsigned>(attr.qp_access_flags) == converted_rights) {
    return true;
  }

  return changeRights(rights);
}

bool ReliableConnection::post_send(ibv_send_wr &wr, bool print) {
  //printf("ATTENTION : post_send() appelé\n");
  struct ibv_send_wr *bad_wr = nullptr;

  auto ret = ibv_post_send(uniq_qp.get(), &wr, &bad_wr);

  if (bad_wr != nullptr) {
    LOGGER_DEBUG(logger, "Got bad wr with id: {}", bad_wr->wr_id);
    return false;
    // throw std::runtime_error("Error encountered during posting in some work
    // request");
  }

  if (print){
    std::cout << "In post_send, we just sent to addr : "<< (uintptr_t)wr.wr.rdma.remote_addr << std::endl;
    std::cout << "While the remote buffr is  : "<< remoteBuf() << std::endl;
    uintptr_t diff = (uintptr_t)wr.wr.rdma.remote_addr - (uintptr_t)remoteBuf();
    std::cout << "If we substract the remoteAddr : "<<  diff << std::endl;
  }

  if (ret != 0) {
    throw std::runtime_error("Error due to driver misuse during posting: " +
                             std::string(std::strerror(errno)));
  }

  return true;
}

bool ReliableConnection::postSendSingleCached(RdmaReq req, uint64_t req_id,
                                              void *buf, uint32_t len,
                                              uintptr_t remote_addr,
                                              bool print) {
  //printf("ATTENTION : postSendSingleCached() appelé\n");
  wr_cached->sg_list->addr = reinterpret_cast<uintptr_t>(buf);
  wr_cached->sg_list->length = len;

  wr_cached->wr_id = req_id;
  wr_cached->opcode = static_cast<enum ibv_wr_opcode>(req);

  if (wr_cached->opcode == IBV_WR_RDMA_WRITE && len <= MaxInlining) {
    wr_cached->send_flags |= IBV_SEND_INLINE;
  } else {
    wr_cached->send_flags &= ~static_cast<unsigned int>(IBV_SEND_INLINE);
  }

  wr_cached->wr.rdma.remote_addr = remote_addr;
  wr_cached->wr.rdma.rkey = rconn.rci.rkey;

  struct ibv_send_wr *bad_wr = nullptr;
  
  auto ret = ibv_post_send(uniq_qp.get(), wr_cached.get(), &bad_wr);
  if (bad_wr != nullptr) {
    LOGGER_DEBUG(logger, "Got bad wr with id: {}", bad_wr->wr_id);
    return false;
    // throw std::runtime_error(
    //     "Error encountered during posting in some work request");
  }

  if (print){
    std::cout << "In postSendSingleCached, we just sent to addr : "<< wr_cached->wr.rdma.remote_addr << std::endl;
  }

  if (ret != 0) {
    throw std::runtime_error("Error due to driver misuse during posting: " +
                             std::string(std::strerror(errno)));
  }

  return true;
}

bool ReliableConnection::postSendSingle(RdmaReq req, uint64_t req_id, void *buf,
                                        uint32_t len, uintptr_t remote_addr,bool print) {
  //printf("ATTENTION : postSendSingle appelé\n");
  return postSendSingle(req, req_id, buf, len, mr.lkey, remote_addr, print);
}

bool ReliableConnection::postSendSingle(RdmaReq req, uint64_t req_id, void *buf,
                                        uint32_t len, uint32_t lkey,
                                        uintptr_t remote_addr,bool print) {
  // TODO(Kristian): if not used concurrently, we could reuse the same wr
  //printf("ATTENTION : postSendSingle() appelé\n");
  struct ibv_send_wr wr;
  struct ibv_sge sg;

  SendWrBuilder()
      .req(req)
      .signaled(true)
      .req_id(req_id)
      .buf(buf)
      .len(len)
      .lkey(lkey)
      .remote_addr(remote_addr)
      .rkey(rconn.rci.rkey)
      .build(wr, sg);

  return post_send(wr, print);
}

void ReliableConnection::reconnect() { 
  printf("ATTENTION appel d'une fonction de RC interdite: reconnect() ==> does nothing \n");
  //connect(rconn); 
}

bool ReliableConnection::pollCqIsOK(CQ cq,
                                    std::vector<struct ibv_wc> &entries) {
  int num = 0;

  switch (cq) {
    case RecvCQ:
      num = ibv_poll_cq(create_attr.recv_cq, static_cast<int>(entries.size()),
                        &entries[0]);
      break;
    case SendCQ:
      num = ibv_poll_cq(create_attr.send_cq, static_cast<int>(entries.size()),
                        &entries[0]);
      break;
    default:
      throw std::runtime_error("Invalid CQ");
  }

  if (num >= 0) {
    entries.erase(entries.begin() + num, entries.end());
    return true;
  } else {
    return false;
  }
}

RemoteConnection ReliableConnection::remoteInfo() const {
  printf("ATTENTION appel d'une fonction de RC dangereuse: remoteInfo()\n");
  
  RemoteConnection rc(static_cast<uint16_t>(cb.lid()), uniq_qp->qp_num, mr.addr,
                      mr.size, mr.rkey); //la preuve que le remote a un buffer de la même taille que nous 
  return rc;
}

void ReliableConnection::query_qp(ibv_qp_attr &qp_attr,
                                  ibv_qp_init_attr &init_attr,
                                  int attr_mask) const {
  ibv_query_qp(uniq_qp.get(), &qp_attr, attr_mask, &init_attr);
}

int ReliableConnection::query_qp_state(){
  struct ibv_qp_attr attr;
  struct ibv_qp_init_attr init_attr;
  
  if (ibv_query_qp(uniq_qp.get(), &attr, IBV_QP_STATE, &init_attr)) {
    fprintf(stderr, "Failed to query QP state\n");
    return -1;
  }
  enum ibv_qp_state state = attr.qp_state;
  return state;
}

/*Méthodes ajoutées pour pouvoir utiliser rdma_create_qp() dans exchanger.cpp*/
struct ibv_pd *ReliableConnection::get_pd() { return this->pd; }

struct ibv_qp_init_attr *ReliableConnection::get_init_attr() {
  return &(this->create_attr);
}

/*Méthode pour initialiser rdma event channel et rdma cm id*/
/*Un rdma event channel et rdma cm id par connexion*/
void ReliableConnection ::configure_cm_channel() {
  cm_event_channel = rdma_create_event_channel();
  if (!cm_event_channel) {
    throw std::runtime_error("Creating cm event channel failed");
    return;
  }
  //LOGGER_INFO(logger, "RDMA CM event channel is created successfully");

  int ret = rdma_create_id(cm_event_channel, &cm_listen_id, NULL, RDMA_PS_TCP);
  if (ret) {
    throw std::runtime_error("Creating cm id failed");
    return;
  }
  //LOGGER_INFO(logger, "A RDMA connection id for the node is created ");
}

struct rdma_cm_id *ReliableConnection ::get_cm_listen_id() { return cm_listen_id; }

struct rdma_cm_id *ReliableConnection ::get_cm_id() { return cm_id; }

struct rdma_event_channel *ReliableConnection ::get_event_channel() {
  return cm_event_channel;
}

void ReliableConnection :: set_cm_id(rdma_cm_id* id){
  cm_id = id;
  cm_id->verbs = pd->context;  
}

/* Dirty legacy code to communicate the RDMA buffer location */
void *ReliableConnection ::getLocalSetup() {
  // max (2) + direct_pmem (1) + dest_size (1) + Addr (8) + length (8) + key (4)
  // = 24
  void *privateData = malloc(24);
  memset(privateData, 0, 24);

  //uint64_t addr = static_cast<uint64_t>(mr.addr);
  uintptr_t addr = static_cast<uintptr_t>(mr.addr);
  uint32_t lkey = mr.lkey;
  /*
  printf("\n============ Local setup ===============\n");
  printf("===== LOCAL ADDRESS : %p\n", reinterpret_cast<void*>(addr));
  printf("===== LOCAL KEY : %p\n\n", reinterpret_cast<void*>(lkey));*/

  memcpy(static_cast<uint8_t*>(privateData) + 4, reinterpret_cast<void*>(&addr), 8);   // Address
  memcpy(static_cast<uint8_t*>(privateData) + 20, reinterpret_cast<void*>(&lkey), 4);  // Key

  return privateData;
}

void ReliableConnection ::setRemoteSetup(const void *network_data) {
  // 4 Bytes of offset to get the address
  memcpy(&rconn.rci.buf_addr, static_cast<const uint8_t*>(network_data) + 4, 8);

  rconn.rci.buf_size = mr.size; //car les buffers de tous les noeuds font la même taille 
  //on copie la taille de notre local buffer à nous 

  // 20 Bytes of offset to get KEY
  memcpy(&rconn.rci.rkey, static_cast<const uint8_t*>(network_data) + 20, 4);
  /*
  printf("\n============ (received) remote setup ===============\n");
  printf("===== ADDRESS : %p\n", reinterpret_cast<void*>(rconn.rci.buf_addr));
  printf("===== LOCAL KEY : %p\n\n", reinterpret_cast<void*>(rconn.rci.rkey));*/

}

void ReliableConnection :: print_all_infos(){
  //ajouter tes tests pour les null
  
  printf("======Informations about this rc ======\n");
  
  //printf("ControlBlock cb : %p \n", reinterpret_cast<void*>(&cb));
  
  printf("Protection Domain pd : %p \n", reinterpret_cast<void*>(pd));
  printf("\t pd-> context : %p \n", reinterpret_cast<void*>(pd->context));
  
  printf("MemoryRegion mr : %p \n", reinterpret_cast<void*>(&mr));
  printf("\t addr : %p \n",  reinterpret_cast<void*>(mr.addr) );
  printf("\t lkey : %d \n",  mr.lkey );
  
  printf("Queue pair uniq_qp : %p \n", reinterpret_cast<void*>(&uniq_qp));
  printf("\t qp-> context : %p \n", reinterpret_cast<void*>(uniq_qp->context));
  printf("\t qp-> pd : %p \n", reinterpret_cast<void*>(uniq_qp->pd));
  //printf("\t qp-> qp_num : %d \n",uniq_qp->qp_num);
    //??

  printf("Remote connection rconn: %p \n", reinterpret_cast<void*>(&rconn));
  //printf("\t qpn : %d \n", rconn.rci.qpn );
  printf("\t buff addr : %p \n",  reinterpret_cast<void*>(rconn.rci.buf_addr));
  //printf("\t buf size : %d \n",  rconn.rci.buf_size);
  printf("\t rkey : %d \n", rconn.rci.rkey);

  std:: cout <<"State de la QP : " << this->query_qp_state() << std::endl;
}

void ReliableConnection :: reset_with_cm(ControlBlock :: MemoryRights rights){
  std::cout << "calling reset with cm" << std::endl; 
  //destroy the qp without changing the cm_id
  /*int ret = rdma_destroy_qp(cm_id); 
  if (ret) {
    printf("Failed to destroy QP due to errno: %s\n", strerror(errno));
    throw std::runtime_error("Failed to create QP due to ...");
    return;
  }*/
  rdma_destroy_qp(cm_id);

  //creating the qp 
  auto ret = rdma_create_qp(cm_id, pd, &create_attr);

  if (ret) {
    printf("Failed to create QP due to errno: %s\n", strerror(errno));
    throw std::runtime_error("Failed to create QP due to ...");
    return;
  }

  auto qp = cm_id->qp;
  uniq_qp = deleted_unique_ptr<struct ibv_qp>(qp, [](struct ibv_qp *qp) {
    auto ret = ibv_destroy_qp(qp);
    if (ret != 0) {
      throw std::runtime_error("Could not query device: " + std::string(std::strerror(errno)));
    }
  });

  //toujours dans l'état init, on en profite pour changer les access flags de la qp
  set_init_with_cm(rights);

  return;
}

void ReliableConnection :: setRCWithTofino(bypass::connection *conn){
  cm_event_channel = conn->cm_event_channel;
  cm_id = conn->cm_id;
  pd = conn->pd;

  auto qp = conn->qp;
  std::cout << "qp copied : " << qp << std::endl;
  
  uniq_qp = deleted_unique_ptr<struct ibv_qp>(qp, [](struct ibv_qp *qp) {
    auto ret = ibv_destroy_qp(qp);
    if (ret != 0) {
      throw std::runtime_error("Could not query device: " + std::string(std::strerror(errno)));
    }
  });

  std::cout << "uniq_qp set up " << std::endl;

  mr.addr = (uintptr_t) conn->mr->addr;
  mr.size = (uint64_t) conn->mr->length;
  mr.lkey = conn->mr->lkey;
  mr.rkey = conn->mr->rkey;
  
  rconn.rci.buf_addr = (uintptr_t)conn->remote_buffer_info.address;
  rconn.rci.buf_size = conn->remote_buffer_info.length;
  rconn.rci.rkey = conn->remote_buffer_info.stag.local_stag;

  // Copié-collé de la fin de la fonction connect() du code source, pour pouvoir utiliser SendSingleCached
  struct ibv_send_wr *wr_ = reinterpret_cast<ibv_send_wr *>( aligned_alloc(64, roundUp(sizeof(ibv_send_wr), 64) + sizeof(ibv_sge)));
  struct ibv_sge *sg_ = reinterpret_cast<ibv_sge *>( reinterpret_cast<char *>(wr_) + roundUp(sizeof(ibv_send_wr), 64));

  memset(sg_, 0, sizeof(*sg_));
  memset(wr_, 0, sizeof(*wr_));

  wr_->sg_list = sg_;
  wr_->num_sge = 1;
  wr_->send_flags |= IBV_SEND_SIGNALED;

  sg_->lkey = mr.lkey;
  wr_->wr.rdma.rkey = rconn.rci.rkey;

  wr_cached = deleted_unique_ptr<struct ibv_send_wr>(wr_, wr_deleter); 

}

}  // namespace dory
