/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"
#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    // 初始化rid_为第一个非空记录位置，若不存在则置为末尾
    rid_ = {RM_NO_PAGE, RM_NO_PAGE};
    // 遍历记录页
    for (int p = RM_FIRST_RECORD_PAGE; p < file_handle_->file_hdr_.num_pages; ++p) {
        RmPageHandle ph = file_handle_->fetch_page_handle(p);
        int slot = Bitmap::first_bit(true, ph.bitmap, file_handle_->file_hdr_.num_records_per_page);
        file_handle_->buffer_pool_manager_->unpin_page(ph.page->get_page_id(), false);
        if (slot < file_handle_->file_hdr_.num_records_per_page) {
            rid_ = {p, slot};
            break;
        }
    }
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    // 找到下一个为1的位（同页优先，其次跨页）
    assert(!is_end());
    RmPageHandle ph = file_handle_->fetch_page_handle(rid_.page_no);
    int next_slot = Bitmap::next_bit(true, ph.bitmap, file_handle_->file_hdr_.num_records_per_page, rid_.slot_no);
    file_handle_->buffer_pool_manager_->unpin_page(ph.page->get_page_id(), false);
    if (next_slot < file_handle_->file_hdr_.num_records_per_page) {
        rid_.slot_no = next_slot;
        return;
    }
    // 跨页查找
    for (int p = rid_.page_no + 1; p < file_handle_->file_hdr_.num_pages; ++p) {
        RmPageHandle nph = file_handle_->fetch_page_handle(p);
        int slot = Bitmap::first_bit(true, nph.bitmap, file_handle_->file_hdr_.num_records_per_page);
        file_handle_->buffer_pool_manager_->unpin_page(nph.page->get_page_id(), false);
        if (slot < file_handle_->file_hdr_.num_records_per_page) {
            rid_.page_no = p;
            rid_.slot_no = slot;
            return;
        }
    }
    // 到末尾
    rid_ = {RM_NO_PAGE, RM_NO_PAGE};

}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    return rid_.page_no == RM_NO_PAGE;

    return false;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}