///////////////////////////////////////
// AUTO GENERATED FILE - DO NOT EDIT //
///////////////////////////////////////

/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Copyright (c) 2016-2019 Intel Corporation
 *
 * This code is subject to the terms of the BSD+Patent license.
 * See LICENSE file for more details.
 */

#include <tlvf/ieee_1905_1/tlvDeviceBridgingCapability.h>
#include <tlvf/tlvflogging.h>

using namespace ieee1905_1;

tlvDeviceBridgingCapability::tlvDeviceBridgingCapability(uint8_t* buff, size_t buff_len, bool parse, bool swap_needed) :
    BaseClass(buff, buff_len, parse, swap_needed) {
    m_init_succeeded = init();
}
tlvDeviceBridgingCapability::tlvDeviceBridgingCapability(std::shared_ptr<BaseClass> base, bool parse, bool swap_needed) :
BaseClass(base->getBuffPtr(), base->getBuffRemainingBytes(), parse, swap_needed){
    m_init_succeeded = init();
}
tlvDeviceBridgingCapability::~tlvDeviceBridgingCapability() {
}
const eTlvType& tlvDeviceBridgingCapability::type() {
    return (const eTlvType&)(*m_type);
}

const uint16_t& tlvDeviceBridgingCapability::length() {
    return (const uint16_t&)(*m_length);
}

uint8_t& tlvDeviceBridgingCapability::bridging_tuples_list_length() {
    return (uint8_t&)(*m_bridging_tuples_list_length);
}

std::tuple<bool, cMacList&> tlvDeviceBridgingCapability::bridging_tuples_list(size_t idx) {
    bool ret_success = ( (m_bridging_tuples_list_idx__ > 0) && (m_bridging_tuples_list_idx__ > idx) );
    size_t ret_idx = ret_success ? idx : 0;
    if (!ret_success) {
        TLVF_LOG(ERROR) << "Requested index is greater than the number of available entries";
    }
    if (*m_bridging_tuples_list_length > 0) {
        return std::forward_as_tuple(ret_success, *(m_bridging_tuples_list_vector[ret_idx]));
    }
    else {
        return std::forward_as_tuple(ret_success, *(m_bridging_tuples_list));
    }
}

std::shared_ptr<cMacList> tlvDeviceBridgingCapability::create_bridging_tuples_list() {
    size_t len = cMacList::get_initial_size();
    if (m_lock_allocation__ || getBuffRemainingBytes() < len) {
        TLVF_LOG(ERROR) << "Not enough available space on buffer";
        return nullptr;
    }
    m_lock_allocation__ = true;
    return std::make_shared<cMacList>(getBuffPtr(), getBuffRemainingBytes(), m_parse__, m_swap__);
}

bool tlvDeviceBridgingCapability::add_bridging_tuples_list(std::shared_ptr<cMacList> ptr) {
    if (ptr == nullptr) {
        TLVF_LOG(ERROR) << "Received entry is nullptr";
        return false;
    }
    if (m_lock_allocation__ == false) {
        TLVF_LOG(ERROR) << "No call to create_bridging_tuples_list was called before add_bridging_tuples_list";
        return false;
    }
    if (ptr->getStartBuffPtr() != getBuffPtr()) {
        TLVF_LOG(ERROR) << "Received to entry pointer is different than expected (excepting the same pointer returned from add method)";
        return false;
    }
    if (ptr->getLen() > getBuffRemainingBytes()) {;
        TLVF_LOG(ERROR) << "Not enough available space on buffer";
        return false;
    }
    if (!m_parse__) {
        m_bridging_tuples_list_idx__++;
        (*m_bridging_tuples_list_length)++;
    }
    size_t len = ptr->getLen();
    m_bridging_tuples_list_vector.push_back(ptr);
    m_buff_ptr__ += len;
    if(m_length){ (*m_length) += len; }
    m_lock_allocation__ = false;
    return true;
}

void tlvDeviceBridgingCapability::class_swap()
{
    tlvf_swap(16, reinterpret_cast<uint8_t*>(m_length));
    for (size_t i = 0; i < (size_t)*m_bridging_tuples_list_length; i++){
        std::get<1>(bridging_tuples_list(i)).class_swap();
    }
}

size_t tlvDeviceBridgingCapability::get_initial_size()
{
    size_t class_size = 0;
    class_size += sizeof(eTlvType); // type
    class_size += sizeof(uint16_t); // length
    class_size += sizeof(uint8_t); // bridging_tuples_list_length
    return class_size;
}

bool tlvDeviceBridgingCapability::init()
{
    if (getBuffRemainingBytes() < kMinimumLength) {
        TLVF_LOG(ERROR) << "Not enough available space on buffer. Class init failed";
        return false;
    }
    m_type = (eTlvType*)m_buff_ptr__;
    if (!m_parse__) *m_type = eTlvType::TLV_DEVICE_BRIDGING_CAPABILITY;
    else {
            if (*m_type != eTlvType::TLV_DEVICE_BRIDGING_CAPABILITY) {
            TLVF_LOG(ERROR) << "TLV type mismatch. Expected value: " << int(eTlvType::TLV_DEVICE_BRIDGING_CAPABILITY) << ", received value: " << int(*m_type);
            return false;
        }
    }
    m_buff_ptr__ += sizeof(eTlvType) * 1;
    m_length = (uint16_t*)m_buff_ptr__;
    if (!m_parse__) *m_length = 0;
    m_buff_ptr__ += sizeof(uint16_t) * 1;
    m_bridging_tuples_list_length = (uint8_t*)m_buff_ptr__;
    if (!m_parse__) *m_bridging_tuples_list_length = 0;
    m_buff_ptr__ += sizeof(uint8_t) * 1;
    if(m_length && !m_parse__){ (*m_length) += sizeof(uint8_t); }
    m_bridging_tuples_list = (cMacList*)m_buff_ptr__;
    m_bridging_tuples_list_idx__ = *m_bridging_tuples_list_length;
    for (size_t i = 0; i < *m_bridging_tuples_list_length; i++) {
        if (!add_bridging_tuples_list(create_bridging_tuples_list())) { 
            TLVF_LOG(ERROR) << "Failed adding bridging_tuples_list entry.";
            return false;
        }
    }
    if (m_buff_ptr__ - m_buff__ > ssize_t(m_buff_len__)) {
        TLVF_LOG(ERROR) << "Not enough available space on buffer. Class init failed";
        return false;
    }
    if (m_parse__ && m_swap__) { class_swap(); }
    return true;
}

cMacList::cMacList(uint8_t* buff, size_t buff_len, bool parse, bool swap_needed) :
    BaseClass(buff, buff_len, parse, swap_needed) {
    m_init_succeeded = init();
}
cMacList::cMacList(std::shared_ptr<BaseClass> base, bool parse, bool swap_needed) :
BaseClass(base->getBuffPtr(), base->getBuffRemainingBytes(), parse, swap_needed){
    m_init_succeeded = init();
}
cMacList::~cMacList() {
}
uint8_t& cMacList::mac_list_length() {
    return (uint8_t&)(*m_mac_list_length);
}

std::tuple<bool, sMacAddr&> cMacList::mac_list(size_t idx) {
    bool ret_success = ( (m_mac_list_idx__ > 0) && (m_mac_list_idx__ > idx) );
    size_t ret_idx = ret_success ? idx : 0;
    if (!ret_success) {
        TLVF_LOG(ERROR) << "Requested index is greater than the number of available entries";
    }
    return std::forward_as_tuple(ret_success, m_mac_list[ret_idx]);
}

bool cMacList::alloc_mac_list(size_t count) {
    if (count == 0) {
        TLVF_LOG(WARNING) << "can't allocate 0 bytes";
        return false;
    }
    size_t len = sizeof(sMacAddr) * count;
    if(getBuffRemainingBytes() < len )  {
        TLVF_LOG(ERROR) << "Not enough available space on buffer - can't allocate";
        return false;
    }
//TLVF_TODO: enable call to memmove
    m_mac_list_idx__ += count;
    *m_mac_list_length += count;
    m_buff_ptr__ += len;
    if (!m_parse__) { 
        for (size_t i = m_mac_list_idx__ - count; i < m_mac_list_idx__; i++) { m_mac_list[i].struct_init(); }
    }
    return true;
}

void cMacList::class_swap()
{
    for (size_t i = 0; i < (size_t)*m_mac_list_length; i++){
        m_mac_list[i].struct_swap();
    }
}

size_t cMacList::get_initial_size()
{
    size_t class_size = 0;
    class_size += sizeof(uint8_t); // mac_list_length
    return class_size;
}

bool cMacList::init()
{
    if (getBuffRemainingBytes() < kMinimumLength) {
        TLVF_LOG(ERROR) << "Not enough available space on buffer. Class init failed";
        return false;
    }
    m_mac_list_length = (uint8_t*)m_buff_ptr__;
    if (!m_parse__) *m_mac_list_length = 0;
    m_buff_ptr__ += sizeof(uint8_t) * 1;
    m_mac_list = (sMacAddr*)m_buff_ptr__;
    m_mac_list_idx__ = *m_mac_list_length;
    m_buff_ptr__ += sizeof(sMacAddr)*(*m_mac_list_length);
    if (m_buff_ptr__ - m_buff__ > ssize_t(m_buff_len__)) {
        TLVF_LOG(ERROR) << "Not enough available space on buffer. Class init failed";
        return false;
    }
    if (m_parse__ && m_swap__) { class_swap(); }
    return true;
}


