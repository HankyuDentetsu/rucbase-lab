/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_file_handle.h"

/**
 * @description: 获取当前表中记录号为rid的记录
 * @param {Rid&} rid 记录号，指定记录的位置
 * @param {Context*} context
 * @return {unique_ptr<RmRecord>} rid对应的记录对象指针
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    // 加锁：IS锁表 + S锁记录
    if (context != nullptr && context->txn_ != nullptr && context->lock_mgr_ != nullptr) {
        context->lock_mgr_->lock_IS_on_table(context->txn_, fd_);
        context->lock_mgr_->lock_shared_on_record(context->txn_, rid, fd_);
    }
    // 1. 获取page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    assert(rid.slot_no >= 0 && rid.slot_no < file_hdr_.num_records_per_page);
    // 2. 校验并构造记录
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    auto rec = std::make_unique<RmRecord>(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
    // 3. 释放页（读操作不脏）
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
    return rec;
}

/**
 * @description: 在当前表中插入一条记录，不指定插入位置
 * @param {char*} buf 要插入的记录的数据
 * @param {Context*} context
 * @return {Rid} 插入的记录的记录号（位置）
 */
Rid RmFileHandle::insert_record(char* buf, Context* context) {
    // 加锁：IX锁表 + X锁记录
    if (context != nullptr && context->txn_ != nullptr && context->lock_mgr_ != nullptr) {
        context->lock_mgr_->lock_IX_on_table(context->txn_, fd_);
    }
    // 1. 获取一个未满页
    RmPageHandle page_handle = create_page_handle();
    // 2. 找到空闲slot
    int slot_no = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);
    assert(slot_no >= 0 && slot_no < file_hdr_.num_records_per_page);
    Rid rid{page_handle.page->get_page_id().page_no, slot_no};
    try {
        if (context != nullptr && context->txn_ != nullptr && context->lock_mgr_ != nullptr) {
            context->lock_mgr_->lock_exclusive_on_record(context->txn_, rid, fd_);
        }
    } catch (...) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw;
    }
    // 3. 写入数据并更新位图
    memcpy(page_handle.get_slot(slot_no), buf, file_hdr_.record_size);
    Bitmap::set(page_handle.bitmap, slot_no);
    page_handle.page_hdr->num_records++;
    // 4. 若变满，更新空闲页链表头
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    }
    if (context != nullptr && context->txn_ != nullptr) {
        auto tab_name = disk_manager_->get_file_name(fd_);
        context->txn_->append_write_record(new WriteRecord(WType::INSERT_TUPLE, tab_name, rid));
    }
    // 标记脏并unpin
    BufferPoolManager::mark_dirty(page_handle.page);
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
    
    return rid;
}

/**
 * @description: 在当前表中的指定位置插入一条记录
 * @param {Rid&} rid 要插入记录的位置
 * @param {char*} buf 要插入记录的数据
 */
void RmFileHandle::insert_record(const Rid& rid, char* buf) {
    // 指定位置插入（用于特殊场景）
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        Bitmap::set(page_handle.bitmap, rid.slot_no);
        page_handle.page_hdr->num_records++;
        if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
            // 如果该页是空闲链表头，则移除
            if (file_hdr_.first_free_page_no == rid.page_no) {
                file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
            }
            page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
        }
    }
    BufferPoolManager::mark_dirty(page_handle.page);
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

/**
 * @description: 删除记录文件中记录号为rid的记录
 * @param {Rid&} rid 要删除的记录的记录号（位置）
 * @param {Context*} context
 */
void RmFileHandle::delete_record(const Rid& rid, Context* context) {
    // 加锁：IX锁表 + X锁记录
    if (context != nullptr && context->txn_ != nullptr && context->lock_mgr_ != nullptr) {
        context->lock_mgr_->lock_IX_on_table(context->txn_, fd_);
        context->lock_mgr_->lock_exclusive_on_record(context->txn_, rid, fd_);
    }
    // 1. 获取page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    // 2. 若存在则删除
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    auto old_record = RmRecord(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
    int prev_num = page_handle.page_hdr->num_records;
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records--;
    // 3. 若从满变为未满，释放到空闲链表
    if (prev_num == file_hdr_.num_records_per_page) {
        release_page_handle(page_handle);
    }
    if (context != nullptr && context->txn_ != nullptr) {
        auto tab_name = disk_manager_->get_file_name(fd_);
        context->txn_->append_write_record(new WriteRecord(WType::DELETE_TUPLE, tab_name, rid, old_record));
    }
    BufferPoolManager::mark_dirty(page_handle.page);
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}


/**
 * @description: 更新记录文件中记录号为rid的记录
 * @param {Rid&} rid 要更新的记录的记录号（位置）
 * @param {char*} buf 新记录的数据
 * @param {Context*} context
 */
void RmFileHandle::update_record(const Rid& rid, char* buf, Context* context) {
    // 加锁：IX锁表 + X锁记录
    if (context != nullptr && context->txn_ != nullptr && context->lock_mgr_ != nullptr) {
        context->lock_mgr_->lock_IX_on_table(context->txn_, fd_);
        context->lock_mgr_->lock_exclusive_on_record(context->txn_, rid, fd_);
    }
    // 1. 获取page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    // 必须是有效记录
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    auto old_record = RmRecord(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
    // 2. 更新内容
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    if (context != nullptr && context->txn_ != nullptr) {
        auto tab_name = disk_manager_->get_file_name(fd_);
        context->txn_->append_write_record(new WriteRecord(WType::UPDATE_TUPLE, tab_name, rid, old_record));
    }
    BufferPoolManager::mark_dirty(page_handle.page);
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);

}

/**
 * 以下函数为辅助函数，仅提供参考，可以选择完成如下函数，也可以删除如下函数，在单元测试中不涉及如下函数接口的直接调用
*/
/**
 * @description: 获取指定页面的页面句柄
 * @param {int} page_no 页面号
 * @return {RmPageHandle} 指定页面的句柄
 */
RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    // 使用缓冲池获取指定页面，并生成page_handle返回给上层
    if (page_no < RM_FIRST_RECORD_PAGE || page_no >= file_hdr_.num_pages) {
        throw PageNotExistError(disk_manager_->get_file_name(fd_), page_no);
    }
    PageId pid;
    pid.fd = fd_;
    pid.page_no = page_no;
    Page* page = buffer_pool_manager_->fetch_page(pid);
    assert(page != nullptr);
    return RmPageHandle(&file_hdr_, page);
}

/**
 * @description: 创建一个新的page handle
 * @return {RmPageHandle} 新的PageHandle
 */
RmPageHandle RmFileHandle::create_new_page_handle() {
    // 1. 通过缓冲池创建新页
    PageId pid;
    pid.fd = fd_;
    pid.page_no = INVALID_PAGE_ID;
    Page* page = buffer_pool_manager_->new_page(&pid);
    // 2. 初始化page handle
    RmPageHandle page_handle(&file_hdr_, page);
    page_handle.page_hdr->num_records = 0;
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    Bitmap::init(page_handle.bitmap, file_hdr_.bitmap_size);
    // 3. 更新文件头
    file_hdr_.num_pages++;
    file_hdr_.first_free_page_no = pid.page_no;
    BufferPoolManager::mark_dirty(page);

    return page_handle;
}

/**
 * @brief 创建或获取一个空闲的page handle
 *
 * @return RmPageHandle 返回生成的空闲page handle
 * @note pin the page, remember to unpin it outside!
 */
RmPageHandle RmFileHandle::create_page_handle() {
    if (file_hdr_.first_free_page_no == RM_NO_PAGE) {
        return create_new_page_handle();
    } else {
        return fetch_page_handle(file_hdr_.first_free_page_no);
    }

}

/**
 * @description: 当一个页面从没有空闲空间的状态变为有空闲空间状态时，更新文件头和页头中空闲页面相关的元数据
 */
void RmFileHandle::release_page_handle(RmPageHandle&page_handle) {
    // 将该页插回空闲链表头
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
    BufferPoolManager::mark_dirty(page_handle.page);
    
}
