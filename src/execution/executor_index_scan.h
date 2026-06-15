/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;

    SmManager *sm_manager_;

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names; 
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return scan_ == nullptr ? true : scan_->is_end(); }

    void beginTuple() override {
        // 加锁：S锁表
        if (context_ != nullptr && context_->txn_ != nullptr && context_->lock_mgr_ != nullptr) {
            context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
        }

        auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols);
        auto ih = sm_manager_->ihs_.at(ix_name).get();
        scan_ = std::make_unique<IxScan>(ih, ih->leaf_begin(), ih->leaf_end(), sm_manager_->get_bpm());

        auto matches = [&](const Rid &r) -> bool {
            auto rec = fh_->get_record(r, context_);
            for (const auto &cond : fed_conds_) {
                if (cond.lhs_col.tab_name != tab_name_) continue;
                const auto &lhs_meta = *get_col(cols_, cond.lhs_col);
                const char *lhs = rec->data + lhs_meta.offset;
                const char *rhs = nullptr;
                ColType type = lhs_meta.type;
                int len = lhs_meta.len;
                if (cond.is_rhs_val) {
                    Value v = cond.rhs_val;
                    if (!v.raw) v.init_raw(len);
                    rhs = v.raw->data;
                } else {
                    if (cond.rhs_col.tab_name != tab_name_) continue;
                    const auto &rhs_meta = *get_col(cols_, cond.rhs_col);
                    rhs = rec->data + rhs_meta.offset;
                }
                int cmp = ix_compare(lhs, rhs, type, len);
                bool ok = false;
                switch (cond.op) {
                    case OP_EQ: ok = (cmp == 0); break;
                    case OP_NE: ok = (cmp != 0); break;
                    case OP_LT: ok = (cmp < 0); break;
                    case OP_GT: ok = (cmp > 0); break;
                    case OP_LE: ok = (cmp <= 0); break;
                    case OP_GE: ok = (cmp >= 0); break;
                    default: ok = false; break;
                }
                if (!ok) return false;
            }
            return true;
        };

        for (; !scan_->is_end(); scan_->next()) {
            Rid r = scan_->rid();
            if (matches(r)) {
                rid_ = r;
                break;
            }
        }
    }

    void nextTuple() override {
        if (scan_ == nullptr || scan_->is_end()) return;

        auto matches = [&](const Rid &r) -> bool {
            auto rec = fh_->get_record(r, context_);
            for (const auto &cond : fed_conds_) {
                if (cond.lhs_col.tab_name != tab_name_) continue;
                const auto &lhs_meta = *get_col(cols_, cond.lhs_col);
                const char *lhs = rec->data + lhs_meta.offset;
                const char *rhs = nullptr;
                ColType type = lhs_meta.type;
                int len = lhs_meta.len;
                if (cond.is_rhs_val) {
                    Value v = cond.rhs_val;
                    if (!v.raw) v.init_raw(len);
                    rhs = v.raw->data;
                } else {
                    if (cond.rhs_col.tab_name != tab_name_) continue;
                    const auto &rhs_meta = *get_col(cols_, cond.rhs_col);
                    rhs = rec->data + rhs_meta.offset;
                }
                int cmp = ix_compare(lhs, rhs, type, len);
                bool ok = false;
                switch (cond.op) {
                    case OP_EQ: ok = (cmp == 0); break;
                    case OP_NE: ok = (cmp != 0); break;
                    case OP_LT: ok = (cmp < 0); break;
                    case OP_GT: ok = (cmp > 0); break;
                    case OP_LE: ok = (cmp <= 0); break;
                    case OP_GE: ok = (cmp >= 0); break;
                    default: ok = false; break;
                }
                if (!ok) return false;
            }
            return true;
        };

        for (scan_->next(); !scan_->is_end(); scan_->next()) {
            Rid r = scan_->rid();
            if (matches(r)) {
                rid_ = r;
                break;
            }
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (scan_ == nullptr || scan_->is_end()) return nullptr;
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }
};
