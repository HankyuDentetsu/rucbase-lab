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

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;     // table_iterator

    SmManager *sm_manager_;

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return scan_ == nullptr ? true : scan_->is_end(); }

    /**
     * @brief 构建表迭代器scan_,并开始迭代扫描,直到扫描到第一个满足谓词条件的元组停止,并赋值给rid_
     *
     */
    void beginTuple() override {
        if (context_ != nullptr && context_->txn_ != nullptr && context_->lock_mgr_ != nullptr) {
            context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
        }
        scan_ = std::make_unique<RmScan>(fh_);
        auto matches = [&](const Rid &r) -> bool {
            auto rec = fh_->get_record(r, context_);
            for (const auto &cond : fed_conds_) {
                // 只处理本表上的条件；连接条件交由上层算子
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
                    if (cond.rhs_col.tab_name != tab_name_) continue; // 连接条件，跳过
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
            if (matches(r)) { rid_ = r; break; }
        }
    }

    /**
     * @brief 从当前scan_指向的记录开始迭代扫描,直到扫描到第一个满足谓词条件的元组停止,并赋值给rid_
     *
     */
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
            if (matches(r)) { rid_ = r; break; }
        }
    }

    /**
     * @brief 返回下一个满足扫描条件的记录
     *
     * @return std::unique_ptr<RmRecord>
     */
    std::unique_ptr<RmRecord> Next() override {
        if (scan_ == nullptr || scan_->is_end()) return nullptr;
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }
};
