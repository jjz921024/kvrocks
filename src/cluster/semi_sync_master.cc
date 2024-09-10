#include "semi_sync_master.h"

#define SEMI_SYNC_WAIT_TIMEOUT 10

WaitingNodeManager::WaitingNodeManager() = default;

WaitingNodeManager::~WaitingNodeManager() {
  for (auto it : waiting_node_list_) {
    delete it;
  }
  waiting_node_list_.clear();
}

bool WaitingNodeManager::InsertWaitingNode(uint64_t log_file_pos) {
  auto* ins_node = new WaitingNode();
  ins_node->log_pos = log_file_pos;

  bool is_insert = false;
  if (waiting_node_list_.empty()) {
    is_insert = true;
  } else {
    auto& last_node = *(--waiting_node_list_.end());
    if (last_node->log_pos < log_file_pos) {
      is_insert = true;
    } else {
      auto iter = waiting_node_list_.find(ins_node);
      if (iter == waiting_node_list_.end()) is_insert = true;
    }
  }
  if (is_insert) {
    waiting_node_list_.emplace(ins_node);
    return true;
  } else {
    LOG(WARNING) << "[semisync] Unknown error to write the same sequence data (" << log_file_pos << ")";
    delete ins_node;
    return false;
  }
}

void WaitingNodeManager::ClearWaitingNodes(uint64_t ack_log_file_pos) {
  auto iter = waiting_node_list_.begin();
  while (iter != waiting_node_list_.end()) {
    auto& item = *iter;
    if (item->log_pos > ack_log_file_pos) break;

    if (item->waiters != 0) {
      iter++;
      continue;
    }
    waiting_node_list_.erase(iter++);
  }
}

// Return the first item whose sequence is greater then or equal to log_file_pos
WaitingNode* WaitingNodeManager::FindWaitingNode(uint64_t log_file_pos) {
  auto iter = waiting_node_list_.begin();
  while (iter != waiting_node_list_.end()) {
    auto& item = *iter;
    if (item->log_pos >= log_file_pos) return item;
    iter++;
  }

  if (waiting_node_list_.empty())
    return nullptr;
  else
    return *waiting_node_list_.begin();
}

int WaitingNodeManager::SignalWaitingNodesUpTo(uint64_t log_file_pos) {
  int ret_num = 0;
  auto iter = waiting_node_list_.begin();
  while (iter != waiting_node_list_.end()) {
    auto& item = *iter;
    if (item->log_pos > log_file_pos) break;

    item->cond.notify_all();
    iter++;
    ++ret_num;
  }

  return ret_num;
}

int WaitingNodeManager::SignalWaitingNodesAll() {
  int ret_num = 0;
  auto iter = waiting_node_list_.begin();
  while (iter != waiting_node_list_.end()) {
    (*iter)->cond.notify_all();
    iter++;
    ++ret_num;
  }

  return ret_num;
}

bool AckContainer::Resize(uint32_t size, const AckInfo** ackinfo) {
  if (size == 0) return false;
  if (size - 1 == ack_array_.size()) return true;

  std::vector<AckInfo> old_ack_array;
  old_ack_array.swap(ack_array_);
  ack_array_.resize(size - 1);
  for (auto& info : old_ack_array) {
    if (info.server_id == 0) continue;
    auto* ack = Insert(info.server_id, info.log_pos);
    if (ack) {
      *ackinfo = ack;
    }
  }

  return true;
}

void AckContainer::Clear() {
  for (auto& item : ack_array_) {
    item.Reset();
  }
}

void AckContainer::RemoveAll(uint64_t log_file_pos) {
  for (std::size_t i = 0; i < ack_array_.size(); i++) {
    auto& info = ack_array_[i];
    if (info.log_pos == log_file_pos) {
      info.Reset();
      empty_slot_ = i;
    }
  }
}

const AckInfo* AckContainer::Insert(int server_id, uint64_t log_file_pos) {
  if (log_file_pos < greatest_return_ack_.log_pos) {
    return nullptr;
  }

  empty_slot_ = ack_array_.size();
  for (std::size_t i = 0; i < ack_array_.size(); i++) {
    auto& info = ack_array_[i];
    if (info.server_id == 0) {
      empty_slot_ = i;
    }
    if (info.server_id == server_id) {
      if (info.log_pos < log_file_pos) {
        info.log_pos = log_file_pos;
      }
      return nullptr;
    }
  }

  AckInfo* ret_ack = nullptr;
  bool to_insert = false;
  if (empty_slot_ == ack_array_.size()) {
    uint64_t min_seq = log_file_pos;
    for (auto& info : ack_array_) {
      if (info.server_id != 0 && info.log_pos < min_seq) {
        min_seq = info.log_pos;
        ret_ack = &info;
      }
    }
    if (ret_ack != nullptr) {
      greatest_return_ack_.Set(ret_ack->server_id, ret_ack->log_pos);
    } else {
      greatest_return_ack_.Set(server_id, log_file_pos);
    }
    ret_ack = &greatest_return_ack_;
    RemoveAll(greatest_return_ack_.log_pos);

    if (log_file_pos > greatest_return_ack_.log_pos) {
      to_insert = true;
    }
  } else {
    to_insert = true;
  }

  if (to_insert) ack_array_[empty_slot_].Set(server_id, log_file_pos);

  return ret_ack;
}

int ReplSemiSyncMaster::Initalize(Config* config) {
  if (init_done_) {
    return 1;
  }
  init_done_ = true;
  bool set_result = SetWaitSlaveCount(config->semi_sync_wait_for_slave_count);
  if (!set_result) {
    LOG(ERROR) << "[semisync] Failed to initialize the semi sync master";
  }
  
  return config->semi_sync_enable ? EnableMaster() : DisableMaster();
}

ReplSemiSyncMaster::~ReplSemiSyncMaster() {
  delete node_manager_;
  LOG(INFO) << "exec ReplSemiSyncMaster::~ReplSemiSyncMaster";
}

int ReplSemiSyncMaster::EnableMaster() {
  std::lock_guard<std::mutex> lock(lock_binlog_);

  int result = 0;
  if (!IsSemiSyncEnabled()) {
    if (node_manager_ == nullptr) node_manager_ = new WaitingNodeManager();
    if (node_manager_ != nullptr) {
      setSemiSyncEnabled(true);
    } else {
      result = -1;
    }
  }

  // initialize state
  state_.store(slave_threads_.size() >= semi_sync_wait_for_slave_count_);
  return result;
}

int ReplSemiSyncMaster::DisableMaster() {
  std::lock_guard<std::mutex> lock(lock_binlog_);

  if (IsSemiSyncEnabled()) {
    switchOff();

    if (node_manager_) {
      delete node_manager_;
      node_manager_ = nullptr;
    }

    setSemiSyncEnabled(false);
    ack_container_.Clear();
  }

  return 0;
}

void ReplSemiSyncMaster::AddSlave(FeedSlaveThread* slave_thread_ptr) {
  std::lock_guard<std::mutex> lock(lock_binlog_);
  if (slave_thread_ptr == nullptr && slave_thread_ptr->GetConn() == nullptr) {
    LOG(ERROR) << "[semisync] Failed to add slave as semi sync one";
    return;
  }
  slave_threads_.emplace_back(slave_thread_ptr);
}

void ReplSemiSyncMaster::RemoveSlave(FeedSlaveThread* slave_thread_ptr) {
  std::lock_guard<std::mutex> lock(lock_binlog_);
  if (slave_thread_ptr == nullptr && slave_thread_ptr->GetConn() == nullptr) {
    LOG(ERROR) << "[semisync] Failed to remove semi sync slave";
    return;
  }
  slave_threads_.remove(slave_thread_ptr);
  if (!IsSemiSyncEnabled() || !IsOn()) return;

  if (slave_threads_.size() < semi_sync_wait_for_slave_count_) {
    LOG(WARNING) << "[semisync] slave less setting count, switch off semi sync";
    switchOff();
  }
}

bool ReplSemiSyncMaster::CommitTrx(uint64_t trx_wait_binlog_pos) {
  std::unique_lock<std::mutex> lock(lock_binlog_);

  if (!IsSemiSyncEnabled() || !IsOn()) return false;

  if (trx_wait_binlog_pos <= wait_file_pos_) {
    return false;
  }

  bool insert_result = node_manager_->InsertWaitingNode(trx_wait_binlog_pos);
  if (!insert_result) {
    LOG(ERROR) << "[semisync] Failed to insert log sequence to wait list";
  }
  auto trx_node = node_manager_->FindWaitingNode(trx_wait_binlog_pos);
  if (trx_node == nullptr) {
    LOG(ERROR) << "[semisync] Data in wait list is lost";
    return false;
  }

  // TODO: 不阻塞线程
  trx_node->waiters++;
  auto s = trx_node->cond.wait_for(lock, std::chrono::seconds(SEMI_SYNC_WAIT_TIMEOUT));
  trx_node->waiters--;
  if (std::cv_status::timeout == s) {
    LOG(ERROR) << "[semisync] Semi sync waits 10s, switch all the slaves to async";
    switchOff();
  }

  if (max_handle_sequence_.load() < trx_wait_binlog_pos) {
    max_handle_sequence_.store(trx_wait_binlog_pos);
  }

  if (trx_node->waiters == 0) {
    node_manager_->ClearWaitingNodes(trx_wait_binlog_pos);
  }

  return true;
}

void ReplSemiSyncMaster::HandleAck(int server_id, uint64_t log_file_pos) {
  std::lock_guard<std::mutex> lock(lock_binlog_);
  // TODO: 重构
  if (semi_sync_wait_for_slave_count_ == 1) {
    reportReplyBinlog(log_file_pos);
  } else {
    auto* ack = ack_container_.Insert(server_id, log_file_pos);
    if (ack != nullptr) {
      reportReplyBinlog(ack->log_pos);
    }
  }
}

bool ReplSemiSyncMaster::SetWaitSlaveCount(uint new_value) {  
  std::lock_guard<std::mutex> lock(lock_binlog_);
  std::ostringstream log;
  log << "[semisync] Try to set slave count " << new_value;
  if (new_value == 0) {
    new_value = slave_threads_.size() / 2 + 1;
    log << ", quorum is: " << new_value;
  }
  LOG(INFO) << log.str();

  const AckInfo* ackinfo = nullptr;
  bool resize_result = ack_container_.Resize(new_value, &ackinfo);
  if (resize_result) {
    if (ackinfo != nullptr) {
      reportReplyBinlog(ackinfo->log_pos);
    }
    semi_sync_wait_for_slave_count_ = new_value;
  }

  LOG(INFO) << "[semisync] Finish setting slave count";
  return resize_result;
}

void ReplSemiSyncMaster::SetAutoFallBack(bool new_value) {
  std::lock_guard<std::mutex> lock(lock_binlog_);
  LOG(INFO) << "[semisync] set auto fall back " << new_value;
  semi_sync_auto_fall_back_.store(new_value);
}

void ReplSemiSyncMaster::reportReplyBinlog(uint64_t log_file_pos) {
  if (!IsSemiSyncEnabled()) return;

  // 若主从同步状态为off时,
  // 每次从节点ack后，根据seq判断是否可以恢复
  if (!IsOn()) {
    LOG(INFO) << "[semisync] try to switch on semi sync";
    trySwitchOn(log_file_pos);
  }

  node_manager_->SignalWaitingNodesUpTo(log_file_pos);
  if (log_file_pos > wait_file_pos_) wait_file_pos_ = log_file_pos;
}

void ReplSemiSyncMaster::trySwitchOn(uint64_t log_file_pos) {
  if (semi_sync_enabled_) {
    if (log_file_pos > max_handle_sequence_) {
      state_.store(true);
    }
  }
}

void ReplSemiSyncMaster::switchOff() {
  state_.store(false);
  wait_file_pos_ = 0;
  max_handle_sequence_ = 0;

  node_manager_->SignalWaitingNodesAll();
}

// semisync_master.cc