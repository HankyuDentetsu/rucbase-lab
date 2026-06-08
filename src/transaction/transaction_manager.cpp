/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include <algorithm>
#include <vector>

#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

namespace {

void append_index_key(std::vector<char> &key, const IndexMeta &index, const char *record_data) {
    int offset = 0;
    for (int i = 0; i < index.col_num; ++i) {
        memcpy(key.data() + offset, record_data + index.cols[i].offset, index.cols[i].len);
        offset += index.cols[i].len;
    }
}

void delete_indexes_for_record(SmManager *sm_manager, const std::string &tab_name, const RmRecord &record) {
    auto &tab = sm_manager->db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        auto ix_name = sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols);
        auto ih = sm_manager->ihs_.at(ix_name).get();
        std::vector<char> key(index.col_tot_len);
        append_index_key(key, index, record.data);
        ih->delete_entry(key.data(), nullptr);
    }
}

void insert_indexes_for_record(SmManager *sm_manager, const std::string &tab_name, const RmRecord &record, const Rid &rid) {
    auto &tab = sm_manager->db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        auto ix_name = sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols);
        auto ih = sm_manager->ihs_.at(ix_name).get();
        std::vector<char> key(index.col_tot_len);
        append_index_key(key, index, record.data);
        ih->insert_entry(key.data(), rid, nullptr);
    }
}

void release_all_locks(LockManager *lock_manager, Transaction *txn) {
    if (txn == nullptr || lock_manager == nullptr) {
        return;
    }
    auto lock_set = txn->get_lock_set();
    if (lock_set == nullptr || lock_set->empty()) {
        return;
    }
    std::vector<LockDataId> locks(lock_set->begin(), lock_set->end());
    std::sort(locks.begin(), locks.end(), [](const LockDataId &lhs, const LockDataId &rhs) { return lhs.Get() < rhs.Get(); });
    for (auto &lock_data_id : locks) {
        lock_manager->unlock(txn, lock_data_id);
    }
}

void clear_write_set(Transaction *txn) {
    if (txn == nullptr || txn->get_write_set() == nullptr) {
        return;
    }
    for (auto *record : *txn->get_write_set()) {
        delete record;
    }
    txn->get_write_set()->clear();
}

}  // namespace

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    (void)log_manager;

    if (txn == nullptr) {
        auto txn_id = next_txn_id_++;
        txn = new Transaction(txn_id);
    }
    txn->set_start_ts(next_timestamp_++);
    txn->set_state(TransactionState::GROWING);

    std::lock_guard<std::mutex> guard(latch_);
    txn_map[txn->get_transaction_id()] = txn;
    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        return;
    }
    if (txn->get_state() == TransactionState::COMMITTED || txn->get_state() == TransactionState::ABORTED) {
        return;
    }

    clear_write_set(txn);
    release_all_locks(lock_manager_, txn);

    if (log_manager != nullptr) {
        log_manager->flush_log_to_disk();
    }
    txn->set_state(TransactionState::COMMITTED);
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    if (txn == nullptr) {
        return;
    }
    if (txn->get_state() == TransactionState::ABORTED) {
        return;
    }

    auto write_set = txn->get_write_set();
    if (write_set != nullptr) {
        for (auto it = write_set->rbegin(); it != write_set->rend(); ++it) {
            auto *write_record = *it;
            auto &tab_name = write_record->GetTableName();
            auto fh = sm_manager_->fhs_.at(tab_name).get();
            auto &rid = write_record->GetRid();

            switch (write_record->GetWriteType()) {
                case WType::INSERT_TUPLE: {
                    auto current_record = fh->get_record(rid, nullptr);
                    delete_indexes_for_record(sm_manager_, tab_name, *current_record);
                    fh->delete_record(rid, nullptr);
                    break;
                }
                case WType::DELETE_TUPLE: {
                    fh->insert_record(rid, write_record->GetRecord().data);
                    insert_indexes_for_record(sm_manager_, tab_name, write_record->GetRecord(), rid);
                    break;
                }
                case WType::UPDATE_TUPLE: {
                    auto current_record = fh->get_record(rid, nullptr);
                    delete_indexes_for_record(sm_manager_, tab_name, *current_record);
                    fh->update_record(rid, write_record->GetRecord().data, nullptr);
                    insert_indexes_for_record(sm_manager_, tab_name, write_record->GetRecord(), rid);
                    break;
                }
                default:
                    break;
            }
        }
    }

    clear_write_set(txn);
    release_all_locks(lock_manager_, txn);

    if (log_manager != nullptr) {
        log_manager->flush_log_to_disk();
    }
    txn->set_state(TransactionState::ABORTED);
}
