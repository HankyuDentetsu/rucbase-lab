/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <fstream>

#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"

/**
 * @description: 判断是否为一个文件夹
 * @return {bool} 返回是否为一个文件夹
 * @param {string&} db_name 数据库文件名称，与文件夹同名
 */
bool SmManager::is_dir(const std::string& db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @description: 创建数据库，所有的数据库相关文件都放在数据库同名文件夹下
 * @param {string&} db_name 数据库名称
 */
void SmManager::create_db(const std::string& db_name) {
    if (is_dir(db_name)) {
        throw DatabaseExistsError(db_name);
    }
    //为数据库创建一个子目录
    std::string cmd = "mkdir " + db_name;
    if (system(cmd.c_str()) < 0) {  // 创建一个名为db_name的目录
        throw UnixError();
    }
    if (chdir(db_name.c_str()) < 0) {  // 进入名为db_name的目录
        throw UnixError();
    }
    //创建系统目录
    DbMeta *new_db = new DbMeta();
    new_db->name_ = db_name;

    // 注意，此处ofstream会在当前目录创建(如果没有此文件先创建)和打开一个名为DB_META_NAME的文件
    std::ofstream ofs(DB_META_NAME);

    // 将new_db中的信息，按照定义好的operator<<操作符，写入到ofs打开的DB_META_NAME文件中
    ofs << *new_db;  // 注意：此处重载了操作符<<

    delete new_db;

    // 创建日志文件
    disk_manager_->create_file(LOG_FILE_NAME);

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除数据库，同时需要清空相关文件以及数据库同名文件夹
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::drop_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    std::string cmd = "rm -r " + db_name;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 打开数据库，找到数据库对应的文件夹，并加载数据库元数据和相关文件
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::open_db(const std::string& db_name) {
    // 如果数据库目录不存在，则先创建
    if (!is_dir(db_name)) {
        create_db(db_name);
    }
    // 进入数据库目录
    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }
    // 读取数据库元数据
    {
        std::ifstream ifs(DB_META_NAME);
        if (!ifs.is_open()) {
            throw FileNotFoundError(DB_META_NAME);
        }
        ifs >> db_;
    }
    // 打开所有表的记录文件句柄
    for (auto &entry : db_.tabs_) {
        const std::string &tab_name = entry.first;
        fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));
    }
    // 打开所有索引文件句柄
    for (auto &entry : db_.tabs_) {
        const std::string &tab_name = entry.first;
        auto &tab = entry.second;
        for (auto &index : tab.indexes) {
            auto ih = ix_manager_->open_index(tab_name, index.cols);
            auto ix_name = ix_manager_->get_index_name(tab_name, index.cols);
            ihs_.emplace(ix_name, std::move(ih));
        }
    }
}

/**
 * @description: 把数据库相关的元数据刷入磁盘中
 */
void SmManager::flush_meta() {
    // 默认清空文件
    std::ofstream ofs(DB_META_NAME);
    ofs << db_;
}

IxIndexHandle* SmManager::get_ix_handle(const std::string& tab_name, const std::vector<ColMeta>& index_cols) {
    auto ix_name = ix_manager_->get_index_name(tab_name, index_cols);
    auto it = ihs_.find(ix_name);
    if (it == ihs_.end()) {
        auto ih = ix_manager_->open_index(tab_name, index_cols);
        it = ihs_.emplace(ix_name, std::move(ih)).first;
    }
    return it->second.get();
}

/**
 * @description: 关闭数据库并把数据落盘
 */
void SmManager::close_db() {
    // 将数据库元数据落盘
    flush_meta();
    // 关闭所有记录文件
    for (auto &entry : fhs_) {
        rm_manager_->close_file(entry.second.get());
    }
    fhs_.clear();
    // 关闭所有索引文件
    for (auto &entry : ihs_) {
        ix_manager_->close_index(entry.second.get());
    }
    ihs_.clear();
    // 清理当前数据库元数据
    db_.name_.clear();
    db_.tabs_.clear();
    // 退出数据库目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 显示所有的表,通过测试需要将其结果写入到output.txt,详情看题目文档
 * @param {Context*} context 
 */
void SmManager::show_tables(Context* context) {
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "| Tables |\n";
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        printer.print_record({tab.name}, context);
        outfile << "| " << tab.name << " |\n";
    }
    printer.print_separator(context);
    outfile.close();
}

/**
 * @description: 显示表的元数据
 * @param {string&} tab_name 表名称
 * @param {Context*} context 
 */
void SmManager::desc_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    // Print header
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    // Print fields
    for (auto &col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    // Print footer
    printer.print_separator(context);
}

/**
 * @description: 创建表
 * @param {string&} tab_name 表的名称
 * @param {vector<ColDef>&} col_defs 表的字段
 * @param {Context*} context 
 */
void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }
    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto &col_def : col_defs) {
        ColMeta col = {.tab_name = tab_name,
                       .name = col_def.name,
                       .type = col_def.type,
                       .len = col_def.len,
                       .offset = curr_offset,
                       .index = false};
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }
    // Create & open record file
    int record_size = curr_offset;  // record_size就是col meta所占的大小（表的元数据也是以记录的形式进行存储的）
    rm_manager_->create_file(tab_name, record_size);
    fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));
    // 加锁：X锁表
    if (context != nullptr && context->txn_ != nullptr && context->lock_mgr_ != nullptr) {
        context->lock_mgr_->lock_exclusive_on_table(context->txn_, fhs_.at(tab_name)->GetFd());
    }
    db_.tabs_[tab_name] = tab;

    flush_meta();
}

/**
 * @description: 删除表
 * @param {string&} tab_name 表的名称
 * @param {Context*} context
 */
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    // 检查表是否存在
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    // 加锁：X锁表
    if (context != nullptr && context->txn_ != nullptr && context->lock_mgr_ != nullptr) {
        context->lock_mgr_->lock_exclusive_on_table(context->txn_, fhs_.at(tab_name)->GetFd());
    }
    // 获取表元数据
    TabMeta &tab = db_.get_table(tab_name);
    // 关闭并删除索引文件
    for (auto &index : tab.indexes) {
        auto ix_name = ix_manager_->get_index_name(tab_name, index.cols);
        // 如果索引句柄已打开，先关闭
        auto it = ihs_.find(ix_name);
        if (it != ihs_.end()) {
            ix_manager_->close_index(it->second.get());
            ihs_.erase(it);
        }
        // 删除索引文件
        ix_manager_->destroy_index(tab_name, index.cols);
    }
    // 关闭并删除记录文件
    {
        auto fh_it = fhs_.find(tab_name);
        if (fh_it != fhs_.end()) {
            rm_manager_->close_file(fh_it->second.get());
            fhs_.erase(fh_it);
        }
        rm_manager_->destroy_file(tab_name);
    }
    // 从数据库元数据中移除该表
    db_.tabs_.erase(tab_name);
    // 刷新元数据到磁盘
    flush_meta();
}

/**
 * @description: 创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    // 检查表是否存在
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    // 加锁：S锁表
    if (context != nullptr && context->txn_ != nullptr && context->lock_mgr_ != nullptr) {
        context->lock_mgr_->lock_shared_on_table(context->txn_, fhs_.at(tab_name)->GetFd());
    }
    TabMeta &tab = db_.get_table(tab_name);
    // 检查索引是否已存在（同名列组合）
    if (tab.is_index(col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }
    // 构造索引元数据
    IndexMeta index_meta;
    index_meta.tab_name = tab_name;
    index_meta.col_num = static_cast<int>(col_names.size());
    index_meta.col_tot_len = 0;
    for (const auto &col_name : col_names) {
        auto col_it = tab.get_col(col_name);
        index_meta.cols.push_back(*col_it);
        index_meta.col_tot_len += col_it->len;
        // 标记列存在索引
        col_it->index = true;
    }
    // 创建索引文件
    ix_manager_->create_index(tab_name, index_meta.cols);
    auto ih = ix_manager_->open_index(tab_name, index_meta.cols);
    auto fh = fhs_.at(tab_name).get();
    for (RmScan scan(fh); !scan.is_end(); scan.next()) {
        auto rid = scan.rid();
        auto rec = fh->get_record(rid, nullptr);
        std::vector<char> key(index_meta.col_tot_len);
        int offset = 0;
        for (auto &col : index_meta.cols) {
            memcpy(key.data() + offset, rec->data + col.offset, col.len);
            offset += col.len;
        }
        ih->insert_entry(key.data(), rid, nullptr);
    }
    ix_manager_->close_index(ih.get());
    // 更新表的索引元数据
    tab.indexes.push_back(index_meta);
    // 刷新数据库元数据
    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    // 检查表是否存在
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    // 加锁：S锁表
    if (context != nullptr && context->txn_ != nullptr && context->lock_mgr_ != nullptr) {
        context->lock_mgr_->lock_shared_on_table(context->txn_, fhs_.at(tab_name)->GetFd());
    }
    TabMeta &tab = db_.get_table(tab_name);
    // 检查索引是否存在
    if (!tab.is_index(col_names)) {
        throw IndexNotFoundError(tab_name, col_names);
    }
    // 获取索引元数据并删除
    auto it = tab.get_index_meta(col_names);
    IndexMeta index_meta = *it;
    auto ix_name = ix_manager_->get_index_name(tab_name, index_meta.cols);
    // 关闭索引句柄
    auto ih_it = ihs_.find(ix_name);
    if (ih_it != ihs_.end()) {
        ix_manager_->close_index(ih_it->second.get());
        ihs_.erase(ih_it);
    }
    // 删除索引文件
    ix_manager_->destroy_index(tab_name, index_meta.cols);
    // 更新列索引标记（简单处理：将相关列标记为无索引）
    for (auto &col : tab.cols) {
        for (auto &idx_col : index_meta.cols) {
            if (col.name == idx_col.name) {
                col.index = false;
            }
        }
    }
    // 从表元数据中移除该索引
    tab.indexes.erase(it);
    // 刷新数据库元数据
    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} 索引包含的字段元数据
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    std::vector<std::string> col_names;
    col_names.reserve(cols.size());
    for (const auto &c : cols) col_names.push_back(c.name);
    drop_index(tab_name, col_names, context);
}
