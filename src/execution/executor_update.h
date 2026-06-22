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

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }
    std::unique_ptr<RmRecord> Next() override {
        // 对所有rids_对应的记录执行更新，同时维护索引
        auto record_size = fh_->get_file_hdr().record_size;
        for (const auto &rid : rids_) {
            auto old_rec = fh_->get_record(rid, context_);
            std::vector<char> new_buf(record_size);
            memcpy(new_buf.data(), old_rec->data, record_size);
            // 应用SET子句
            for (auto &clause : set_clauses_) {
                auto col_it = tab_.get_col(clause.lhs.col_name);
                auto &col = *col_it;
                Value v = clause.rhs;
                if (!v.raw) v.init_raw(col.len);
                memcpy(new_buf.data() + col.offset, v.raw->data, col.len);
            }
            // 维护索引：若键发生变化则删除旧键并插入新键
            for (auto &index : tab_.indexes) {
                auto ih = sm_manager_->get_ix_handle(tab_name_, index.cols);
                std::vector<char> old_key(index.col_tot_len), new_key(index.col_tot_len);
                int offset = 0;
                for (size_t i = 0; i < index.col_num; ++i) {
                    memcpy(old_key.data() + offset, old_rec->data + index.cols[i].offset, index.cols[i].len);
                    memcpy(new_key.data() + offset, new_buf.data() + index.cols[i].offset, index.cols[i].len);
                    offset += index.cols[i].len;
                }
                if (memcmp(old_key.data(), new_key.data(), index.col_tot_len) != 0) {
                    ih->delete_entry(old_key.data(), context_->txn_);
                    ih->insert_entry(new_key.data(), rid, context_->txn_);
                }
            }
            // 更新记录文件
            fh_->update_record(rid, new_buf.data(), context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};