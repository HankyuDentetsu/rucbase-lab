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

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);

    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    void beginTuple() override {
        left_->beginTuple();
        if (left_->is_end()) { isend = true; return; }
        right_->beginTuple();
        auto match_current = [&]() -> bool {
            if (left_->is_end() || right_->is_end()) return false;
            auto lrec = left_->Next();
            auto rrec = right_->Next();
            // 评估所有join条件
            for (const auto &cond : fed_conds_) {
                const char *lhs = nullptr; const char *rhs = nullptr;
                int len = 0; ColType type;
                // 查找lhs元信息
                bool lhs_in_left = false, lhs_in_right = false;
                for (auto &c : left_->cols()) if (c.tab_name == cond.lhs_col.tab_name && c.name == cond.lhs_col.col_name) { lhs = lrec->data + c.offset; len = c.len; type = c.type; lhs_in_left = true; break; }
                if (!lhs_in_left) {
                    for (auto &c : right_->cols()) if (c.tab_name == cond.lhs_col.tab_name && c.name == cond.lhs_col.col_name) { lhs = rrec->data + c.offset; len = c.len; type = c.type; lhs_in_right = true; break; }
                }
                if (!lhs_in_left && !lhs_in_right) return false;
                if (cond.is_rhs_val) {
                    Value v = cond.rhs_val; if (!v.raw) v.init_raw(len); rhs = v.raw->data;
                } else {
                    bool rhs_in_left = false, rhs_in_right = false;
                    for (auto &c : left_->cols()) if (c.tab_name == cond.rhs_col.tab_name && c.name == cond.rhs_col.col_name) { rhs = lrec->data + c.offset; rhs_in_left = true; break; }
                    if (!rhs_in_left) {
                        for (auto &c : right_->cols()) if (c.tab_name == cond.rhs_col.tab_name && c.name == cond.rhs_col.col_name) { rhs = rrec->data + c.offset; rhs_in_right = true; break; }
                    }
                    if (!rhs_in_left && !rhs_in_right) return false;
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
        // 定位到第一条满足条件的连接元组
        auto advance = [&]() -> bool {
            while (!left_->is_end()) {
                while (!right_->is_end()) {
                    if (match_current()) return true;
                    right_->nextTuple();
                }
                left_->nextTuple();
                if (left_->is_end()) break;
                right_->beginTuple();
            }
            return false;
        };
        isend = !advance();
    }

    void nextTuple() override {
        if (isend) return;
        // 从当前(right)后续开始寻找下一个满足的配对
        right_->nextTuple();
        auto match_current = [&]() -> bool {
            if (left_->is_end() || right_->is_end()) return false;
            auto lrec = left_->Next();
            auto rrec = right_->Next();
            for (const auto &cond : fed_conds_) {
                const char *lhs = nullptr; const char *rhs = nullptr; int len = 0; ColType type;
                bool lhs_in_left = false, lhs_in_right = false;
                for (auto &c : left_->cols()) if (c.tab_name == cond.lhs_col.tab_name && c.name == cond.lhs_col.col_name) { lhs = lrec->data + c.offset; len = c.len; type = c.type; lhs_in_left = true; break; }
                if (!lhs_in_left) {
                    for (auto &c : right_->cols()) if (c.tab_name == cond.lhs_col.tab_name && c.name == cond.lhs_col.col_name) { lhs = rrec->data + c.offset; len = c.len; type = c.type; lhs_in_right = true; break; }
                }
                if (!lhs_in_left && !lhs_in_right) return false;
                if (cond.is_rhs_val) {
                    Value v = cond.rhs_val; if (!v.raw) v.init_raw(len); rhs = v.raw->data;
                } else {
                    bool rhs_in_left = false, rhs_in_right = false;
                    for (auto &c : left_->cols()) if (c.tab_name == cond.rhs_col.tab_name && c.name == cond.rhs_col.col_name) { rhs = lrec->data + c.offset; rhs_in_left = true; break; }
                    if (!rhs_in_left) {
                        for (auto &c : right_->cols()) if (c.tab_name == cond.rhs_col.tab_name && c.name == cond.rhs_col.col_name) { rhs = rrec->data + c.offset; rhs_in_right = true; break; }
                    }
                    if (!rhs_in_left && !rhs_in_right) return false;
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
        auto advance = [&]() -> bool {
            while (!left_->is_end()) {
                while (!right_->is_end()) {
                    if (match_current()) return true;
                    right_->nextTuple();
                }
                left_->nextTuple();
                if (left_->is_end()) break;
                right_->beginTuple();
            }
            return false;
        };
        isend = !advance();
    }

    bool is_end() const override { return isend; }

    std::unique_ptr<RmRecord> Next() override {
        if (isend) return nullptr;
        auto lrec = left_->Next();
        auto rrec = right_->Next();
        auto out = std::make_unique<RmRecord>(len_);
        size_t l_len = left_->tupleLen();
        memcpy(out->data, lrec->data, l_len);
        memcpy(out->data + l_len, rrec->data, right_->tupleLen());
        return out;
    }

    Rid &rid() override { return _abstract_rid; }
};