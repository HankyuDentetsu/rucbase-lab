/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

bool LockManager::is_compatible(LockMode existing, LockMode requested) const {
    switch (existing) {
        case LockMode::SHARED:
            return requested == LockMode::SHARED || requested == LockMode::INTENTION_SHARED;
        case LockMode::EXLUCSIVE:
            return false;
        case LockMode::INTENTION_SHARED:
            return requested != LockMode::EXLUCSIVE;
        case LockMode::INTENTION_EXCLUSIVE:
            return requested == LockMode::INTENTION_SHARED || requested == LockMode::INTENTION_EXCLUSIVE;
        case LockMode::S_IX:
            return requested == LockMode::INTENTION_SHARED;
        default:
            return false;
    }
}

bool LockManager::is_compatible_with_queue(const LockRequestQueue &queue, txn_id_t txn_id, LockMode requested) const {
    for (const auto &request : queue.request_queue_) {
        if (!request.granted_ || request.txn_id_ == txn_id) {
            continue;
        }
        if (!is_compatible(request.lock_mode_, requested) || !is_compatible(requested, request.lock_mode_)) {
            return false;
        }
    }
    return true;
}

LockManager::LockMode LockManager::upgrade_lock_mode(LockMode current, LockMode requested) const {
    if (current == requested) {
        return current;
    }
    if (current == LockMode::EXLUCSIVE || requested == LockMode::EXLUCSIVE) {
        return LockMode::EXLUCSIVE;
    }
    if (current == LockMode::S_IX || requested == LockMode::S_IX) {
        return LockMode::S_IX;
    }
    if ((current == LockMode::SHARED && requested == LockMode::INTENTION_EXCLUSIVE) ||
        (current == LockMode::INTENTION_EXCLUSIVE && requested == LockMode::SHARED)) {
        return LockMode::S_IX;
    }
    if (current == LockMode::SHARED || requested == LockMode::SHARED) {
        return LockMode::SHARED;
    }
    if (current == LockMode::INTENTION_EXCLUSIVE || requested == LockMode::INTENTION_EXCLUSIVE) {
        return LockMode::INTENTION_EXCLUSIVE;
    }
    return LockMode::INTENTION_SHARED;
}

LockManager::GroupLockMode LockManager::lock_mode_to_group_mode(LockMode lock_mode) const {
    switch (lock_mode) {
        case LockMode::SHARED:
            return GroupLockMode::S;
        case LockMode::EXLUCSIVE:
            return GroupLockMode::X;
        case LockMode::INTENTION_SHARED:
            return GroupLockMode::IS;
        case LockMode::INTENTION_EXCLUSIVE:
            return GroupLockMode::IX;
        case LockMode::S_IX:
            return GroupLockMode::SIX;
        default:
            return GroupLockMode::NON_LOCK;
    }
}

void LockManager::update_group_lock_mode(LockRequestQueue *queue) {
    queue->group_lock_mode_ = GroupLockMode::NON_LOCK;
    for (const auto &request : queue->request_queue_) {
        if (!request.granted_) {
            continue;
        }
        auto mode = lock_mode_to_group_mode(request.lock_mode_);
        if (mode == GroupLockMode::X) {
            queue->group_lock_mode_ = GroupLockMode::X;
            return;
        }
        if (mode == GroupLockMode::SIX) {
            queue->group_lock_mode_ = GroupLockMode::SIX;
            continue;
        }
        if (mode == GroupLockMode::S && queue->group_lock_mode_ != GroupLockMode::SIX) {
            queue->group_lock_mode_ = GroupLockMode::S;
            continue;
        }
        if (mode == GroupLockMode::IX &&
            queue->group_lock_mode_ != GroupLockMode::S && queue->group_lock_mode_ != GroupLockMode::SIX) {
            queue->group_lock_mode_ = GroupLockMode::IX;
            continue;
        }
        if (mode == GroupLockMode::IS && queue->group_lock_mode_ == GroupLockMode::NON_LOCK) {
            queue->group_lock_mode_ = GroupLockMode::IS;
        }
    }
}

bool LockManager::lock_impl(Transaction *txn, const LockDataId &lock_data_id, LockMode lock_mode) {
    if (txn == nullptr) {
        return true;
    }
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }
    if (txn->get_state() == TransactionState::DEFAULT) {
        txn->set_state(TransactionState::GROWING);
    }

    std::lock_guard<std::mutex> guard(latch_);
    auto &queue = lock_table_[lock_data_id];
    for (auto it = queue.request_queue_.begin(); it != queue.request_queue_.end(); ++it) {
        if (it->txn_id_ != txn->get_transaction_id()) {
            continue;
        }
        auto target_mode = upgrade_lock_mode(it->lock_mode_, lock_mode);
        if (target_mode == it->lock_mode_) {
            return true;
        }
        if (!is_compatible_with_queue(queue, txn->get_transaction_id(), target_mode)) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
        it->lock_mode_ = target_mode;
        it->granted_ = true;
        txn->get_lock_set()->insert(lock_data_id);
        update_group_lock_mode(&queue);
        return true;
    }

    if (!is_compatible_with_queue(queue, txn->get_transaction_id(), lock_mode)) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }

    queue.request_queue_.emplace_back(txn->get_transaction_id(), lock_mode);
    queue.request_queue_.back().granted_ = true;
    txn->get_lock_set()->insert(lock_data_id);
    update_group_lock_mode(&queue);
    return true;
}

/**
 * @description: 申请行级共享锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID 记录所在的表的fd
 * @param {int} tab_fd
 */
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    return lock_impl(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::SHARED);
}

/**
 * @description: 申请行级排他锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID
 * @param {int} tab_fd 记录所在的表的fd
 */
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    return lock_impl(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::EXLUCSIVE);
}

/**
 * @description: 申请表级读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    return lock_impl(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::SHARED);
}

/**
 * @description: 申请表级写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    return lock_impl(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::EXLUCSIVE);
}

/**
 * @description: 申请表级意向读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    return lock_impl(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_SHARED);
}

/**
 * @description: 申请表级意向写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    return lock_impl(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_EXCLUSIVE);
}

/**
 * @description: 释放锁
 * @return {bool} 返回解锁是否成功
 * @param {Transaction*} txn 要释放锁的事务对象指针
 * @param {LockDataId} lock_data_id 要释放的锁ID
 */
bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    if (txn == nullptr) {
        return true;
    }

    std::lock_guard<std::mutex> guard(latch_);
    auto table_it = lock_table_.find(lock_data_id);
    if (table_it == lock_table_.end()) {
        return false;
    }

    auto &queue = table_it->second;
    for (auto it = queue.request_queue_.begin(); it != queue.request_queue_.end(); ++it) {
        if (it->txn_id_ != txn->get_transaction_id()) {
            continue;
        }
        queue.request_queue_.erase(it);
        txn->get_lock_set()->erase(lock_data_id);
        update_group_lock_mode(&queue);
        if (queue.request_queue_.empty()) {
            lock_table_.erase(table_it);
        }
        if (txn->get_state() == TransactionState::GROWING) {
            txn->set_state(TransactionState::SHRINKING);
        }
        return true;
    }

    return false;
}
