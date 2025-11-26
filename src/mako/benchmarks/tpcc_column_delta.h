#ifndef _TPCC_COLUMN_DELTA_H_
#define _TPCC_COLUMN_DELTA_H_

#include "../lib/column_delta.h"
#include "tpcc.h"
#include <cstddef>

namespace mako {

// Customer value field comparator specialization
template<>
struct FieldComparator<customer::value> {
    static uint32_t compare(const customer::value& old_val, const customer::value& new_val) {
        uint32_t changed = 0;
        if (old_val.c_discount != new_val.c_discount) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_DISCOUNT));
        if (old_val.c_credit != new_val.c_credit) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_CREDIT));
        if (old_val.c_last != new_val.c_last) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_LAST));
        if (old_val.c_first != new_val.c_first) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_FIRST));
        if (old_val.c_credit_lim != new_val.c_credit_lim) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_CREDIT_LIM));
        if (old_val.c_balance != new_val.c_balance) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_BALANCE));
        if (old_val.c_ytd_payment != new_val.c_ytd_payment) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_YTD_PAYMENT));
        if (old_val.c_payment_cnt != new_val.c_payment_cnt) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_PAYMENT_CNT));
        if (old_val.c_delivery_cnt != new_val.c_delivery_cnt) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_DELIVERY_CNT));
        if (old_val.c_street_1 != new_val.c_street_1) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_STREET_1));
        if (old_val.c_street_2 != new_val.c_street_2) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_STREET_2));
        if (old_val.c_city != new_val.c_city) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_CITY));
        if (old_val.c_state != new_val.c_state) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_STATE));
        if (old_val.c_zip != new_val.c_zip) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_ZIP));
        if (old_val.c_phone != new_val.c_phone) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_PHONE));
        if (old_val.c_since != new_val.c_since) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_SINCE));
        if (old_val.c_middle != new_val.c_middle) changed |= (1u << static_cast<uint8_t>(CustomerColId::C_MIDDLE));
        return changed;
    }
    
    static size_t getFieldOffset(uint8_t field_id) {
        switch (static_cast<CustomerColId>(field_id)) {
            case CustomerColId::C_DISCOUNT: return offsetof(customer::value, c_discount);
            case CustomerColId::C_CREDIT: return offsetof(customer::value, c_credit);
            case CustomerColId::C_LAST: return offsetof(customer::value, c_last);
            case CustomerColId::C_FIRST: return offsetof(customer::value, c_first);
            case CustomerColId::C_CREDIT_LIM: return offsetof(customer::value, c_credit_lim);
            case CustomerColId::C_BALANCE: return offsetof(customer::value, c_balance);
            case CustomerColId::C_YTD_PAYMENT: return offsetof(customer::value, c_ytd_payment);
            case CustomerColId::C_PAYMENT_CNT: return offsetof(customer::value, c_payment_cnt);
            case CustomerColId::C_DELIVERY_CNT: return offsetof(customer::value, c_delivery_cnt);
            case CustomerColId::C_STREET_1: return offsetof(customer::value, c_street_1);
            case CustomerColId::C_STREET_2: return offsetof(customer::value, c_street_2);
            case CustomerColId::C_CITY: return offsetof(customer::value, c_city);
            case CustomerColId::C_STATE: return offsetof(customer::value, c_state);
            case CustomerColId::C_ZIP: return offsetof(customer::value, c_zip);
            case CustomerColId::C_PHONE: return offsetof(customer::value, c_phone);
            case CustomerColId::C_SINCE: return offsetof(customer::value, c_since);
            case CustomerColId::C_MIDDLE: return offsetof(customer::value, c_middle);
            default: return 0;
        }
    }
    
    static size_t getFieldSize(uint8_t field_id) {
        switch (static_cast<CustomerColId>(field_id)) {
            case CustomerColId::C_DISCOUNT: return sizeof(float);
            case CustomerColId::C_CREDIT: return sizeof(customer::value::c_credit);
            case CustomerColId::C_LAST: return sizeof(customer::value::c_last);
            case CustomerColId::C_FIRST: return sizeof(customer::value::c_first);
            case CustomerColId::C_CREDIT_LIM: return sizeof(float);
            case CustomerColId::C_BALANCE: return sizeof(float);
            case CustomerColId::C_YTD_PAYMENT: return sizeof(float);
            case CustomerColId::C_PAYMENT_CNT: return sizeof(int32_t);
            case CustomerColId::C_DELIVERY_CNT: return sizeof(int32_t);
            case CustomerColId::C_STREET_1: return sizeof(customer::value::c_street_1);
            case CustomerColId::C_STREET_2: return sizeof(customer::value::c_street_2);
            case CustomerColId::C_CITY: return sizeof(customer::value::c_city);
            case CustomerColId::C_STATE: return sizeof(customer::value::c_state);
            case CustomerColId::C_ZIP: return sizeof(customer::value::c_zip);
            case CustomerColId::C_PHONE: return sizeof(customer::value::c_phone);
            case CustomerColId::C_SINCE: return sizeof(uint32_t);
            case CustomerColId::C_MIDDLE: return sizeof(customer::value::c_middle);
            default: return 0;
        }
    }
};

// District value field comparator specialization
template<>
struct FieldComparator<district::value> {
    static uint32_t compare(const district::value& old_val, const district::value& new_val) {
        uint32_t changed = 0;
        if (old_val.d_ytd != new_val.d_ytd) changed |= (1u << static_cast<uint8_t>(DistrictColId::D_YTD));
        if (old_val.d_tax != new_val.d_tax) changed |= (1u << static_cast<uint8_t>(DistrictColId::D_TAX));
        if (old_val.d_next_o_id != new_val.d_next_o_id) changed |= (1u << static_cast<uint8_t>(DistrictColId::D_NEXT_O_ID));
        if (old_val.d_name != new_val.d_name) changed |= (1u << static_cast<uint8_t>(DistrictColId::D_NAME));
        if (old_val.d_street_1 != new_val.d_street_1) changed |= (1u << static_cast<uint8_t>(DistrictColId::D_STREET_1));
        if (old_val.d_street_2 != new_val.d_street_2) changed |= (1u << static_cast<uint8_t>(DistrictColId::D_STREET_2));
        if (old_val.d_city != new_val.d_city) changed |= (1u << static_cast<uint8_t>(DistrictColId::D_CITY));
        if (old_val.d_state != new_val.d_state) changed |= (1u << static_cast<uint8_t>(DistrictColId::D_STATE));
        if (old_val.d_zip != new_val.d_zip) changed |= (1u << static_cast<uint8_t>(DistrictColId::D_ZIP));
        return changed;
    }
    
    static size_t getFieldOffset(uint8_t field_id) {
        switch (static_cast<DistrictColId>(field_id)) {
            case DistrictColId::D_YTD: return offsetof(district::value, d_ytd);
            case DistrictColId::D_TAX: return offsetof(district::value, d_tax);
            case DistrictColId::D_NEXT_O_ID: return offsetof(district::value, d_next_o_id);
            case DistrictColId::D_NAME: return offsetof(district::value, d_name);
            case DistrictColId::D_STREET_1: return offsetof(district::value, d_street_1);
            case DistrictColId::D_STREET_2: return offsetof(district::value, d_street_2);
            case DistrictColId::D_CITY: return offsetof(district::value, d_city);
            case DistrictColId::D_STATE: return offsetof(district::value, d_state);
            case DistrictColId::D_ZIP: return offsetof(district::value, d_zip);
            default: return 0;
        }
    }
    
    static size_t getFieldSize(uint8_t field_id) {
        switch (static_cast<DistrictColId>(field_id)) {
            case DistrictColId::D_YTD: return sizeof(float);
            case DistrictColId::D_TAX: return sizeof(float);
            case DistrictColId::D_NEXT_O_ID: return sizeof(int32_t);
            case DistrictColId::D_NAME: return sizeof(district::value::d_name);
            case DistrictColId::D_STREET_1: return sizeof(district::value::d_street_1);
            case DistrictColId::D_STREET_2: return sizeof(district::value::d_street_2);
            case DistrictColId::D_CITY: return sizeof(district::value::d_city);
            case DistrictColId::D_STATE: return sizeof(district::value::d_state);
            case DistrictColId::D_ZIP: return sizeof(district::value::d_zip);
            default: return 0;
        }
    }
};

// Warehouse value field comparator specialization
template<>
struct FieldComparator<warehouse::value> {
    static uint32_t compare(const warehouse::value& old_val, const warehouse::value& new_val) {
        uint32_t changed = 0;
        if (old_val.w_ytd != new_val.w_ytd) changed |= (1u << static_cast<uint8_t>(WarehouseColId::W_YTD));
        if (old_val.w_tax != new_val.w_tax) changed |= (1u << static_cast<uint8_t>(WarehouseColId::W_TAX));
        if (old_val.w_name != new_val.w_name) changed |= (1u << static_cast<uint8_t>(WarehouseColId::W_NAME));
        if (old_val.w_street_1 != new_val.w_street_1) changed |= (1u << static_cast<uint8_t>(WarehouseColId::W_STREET_1));
        if (old_val.w_street_2 != new_val.w_street_2) changed |= (1u << static_cast<uint8_t>(WarehouseColId::W_STREET_2));
        if (old_val.w_city != new_val.w_city) changed |= (1u << static_cast<uint8_t>(WarehouseColId::W_CITY));
        if (old_val.w_state != new_val.w_state) changed |= (1u << static_cast<uint8_t>(WarehouseColId::W_STATE));
        if (old_val.w_zip != new_val.w_zip) changed |= (1u << static_cast<uint8_t>(WarehouseColId::W_ZIP));
        return changed;
    }
    
    static size_t getFieldOffset(uint8_t field_id) {
        switch (static_cast<WarehouseColId>(field_id)) {
            case WarehouseColId::W_YTD: return offsetof(warehouse::value, w_ytd);
            case WarehouseColId::W_TAX: return offsetof(warehouse::value, w_tax);
            case WarehouseColId::W_NAME: return offsetof(warehouse::value, w_name);
            case WarehouseColId::W_STREET_1: return offsetof(warehouse::value, w_street_1);
            case WarehouseColId::W_STREET_2: return offsetof(warehouse::value, w_street_2);
            case WarehouseColId::W_CITY: return offsetof(warehouse::value, w_city);
            case WarehouseColId::W_STATE: return offsetof(warehouse::value, w_state);
            case WarehouseColId::W_ZIP: return offsetof(warehouse::value, w_zip);
            default: return 0;
        }
    }
    
    static size_t getFieldSize(uint8_t field_id) {
        switch (static_cast<WarehouseColId>(field_id)) {
            case WarehouseColId::W_YTD: return sizeof(float);
            case WarehouseColId::W_TAX: return sizeof(float);
            case WarehouseColId::W_NAME: return sizeof(warehouse::value::w_name);
            case WarehouseColId::W_STREET_1: return sizeof(warehouse::value::w_street_1);
            case WarehouseColId::W_STREET_2: return sizeof(warehouse::value::w_street_2);
            case WarehouseColId::W_CITY: return sizeof(warehouse::value::w_city);
            case WarehouseColId::W_STATE: return sizeof(warehouse::value::w_state);
            case WarehouseColId::W_ZIP: return sizeof(warehouse::value::w_zip);
            default: return 0;
        }
    }
};

// Helper functions for TPCC column-delta operations with metrics tracking

// Compare customer values and return changed fields bitmask
inline uint32_t compareCustomerFields(const customer::value& old_val, const customer::value& new_val) {
    return FieldComparator<customer::value>::compare(old_val, new_val);
}

// Compare district values and return changed fields bitmask
inline uint32_t compareDistrictFields(const district::value& old_val, const district::value& new_val) {
    return FieldComparator<district::value>::compare(old_val, new_val);
}

// Compare warehouse values and return changed fields bitmask
inline uint32_t compareWarehouseFields(const warehouse::value& old_val, const warehouse::value& new_val) {
    return FieldComparator<warehouse::value>::compare(old_val, new_val);
}

// Record metrics for a customer update
inline void recordCustomerUpdateMetrics(const customer::value& old_val, const customer::value& new_val, size_t full_row_size) {
#if MAKO_ENABLE_COLUMN_DELTAS
    auto& metrics = getColumnDeltaMetrics();
    metrics.total_updates++;
    metrics.customer_updates++;
    
    // Only count as delta update if runtime flag is enabled
    if (!isColumnDeltasEnabled()) {
        metrics.full_row_updates++;
        metrics.bytes_full_row += full_row_size;
        return;
    }
    
    uint32_t changed = compareCustomerFields(old_val, new_val);
    int num_changed = countChangedFields(changed);
    
    if (num_changed > 0 && num_changed <= DELTA_COLUMN_THRESHOLD) {
        size_t delta_size = sizeof(ColumnDeltaHeader);
        for (uint8_t i = 0; i < 17 && changed != 0; i++) {
            if (changed & (1u << i)) {
                delta_size += ColumnDelta::ENTRY_HEADER_SIZE + FieldComparator<customer::value>::getFieldSize(i);
            }
        }
        metrics.delta_updates++;
        metrics.bytes_delta += delta_size;
        metrics.bytes_full_row += full_row_size;
        if (delta_size < full_row_size) {
            metrics.bytes_saved += (full_row_size - delta_size);
        }
    } else {
        metrics.full_row_updates++;
        metrics.bytes_full_row += full_row_size;
    }
#endif
}

// Record metrics for a warehouse update
inline void recordWarehouseUpdateMetrics(const warehouse::value& old_val, const warehouse::value& new_val, size_t full_row_size) {
#if MAKO_ENABLE_COLUMN_DELTAS
    auto& metrics = getColumnDeltaMetrics();
    metrics.total_updates++;
    metrics.warehouse_updates++;
    
    // Only count as delta update if runtime flag is enabled
    if (!isColumnDeltasEnabled()) {
        metrics.full_row_updates++;
        metrics.bytes_full_row += full_row_size;
        return;
    }
    
    uint32_t changed = compareWarehouseFields(old_val, new_val);
    int num_changed = countChangedFields(changed);
    
    if (num_changed > 0 && num_changed <= DELTA_COLUMN_THRESHOLD) {
        size_t delta_size = sizeof(ColumnDeltaHeader);
        for (uint8_t i = 0; i < 8 && changed != 0; i++) {
            if (changed & (1u << i)) {
                delta_size += ColumnDelta::ENTRY_HEADER_SIZE + FieldComparator<warehouse::value>::getFieldSize(i);
            }
        }
        metrics.delta_updates++;
        metrics.bytes_delta += delta_size;
        metrics.bytes_full_row += full_row_size;
        if (delta_size < full_row_size) {
            metrics.bytes_saved += (full_row_size - delta_size);
        }
    } else {
        metrics.full_row_updates++;
        metrics.bytes_full_row += full_row_size;
    }
#endif
}

// Record metrics for a district update
inline void recordDistrictUpdateMetrics(const district::value& old_val, const district::value& new_val, size_t full_row_size) {
#if MAKO_ENABLE_COLUMN_DELTAS
    auto& metrics = getColumnDeltaMetrics();
    metrics.total_updates++;
    metrics.district_updates++;
    
    // Only count as delta update if runtime flag is enabled
    if (!isColumnDeltasEnabled()) {
        metrics.full_row_updates++;
        metrics.bytes_full_row += full_row_size;
        return;
    }
    
    uint32_t changed = compareDistrictFields(old_val, new_val);
    int num_changed = countChangedFields(changed);
    
    if (num_changed > 0 && num_changed <= DELTA_COLUMN_THRESHOLD) {
        size_t delta_size = sizeof(ColumnDeltaHeader);
        for (uint8_t i = 0; i < 9 && changed != 0; i++) {
            if (changed & (1u << i)) {
                delta_size += ColumnDelta::ENTRY_HEADER_SIZE + FieldComparator<district::value>::getFieldSize(i);
            }
        }
        metrics.delta_updates++;
        metrics.bytes_delta += delta_size;
        metrics.bytes_full_row += full_row_size;
        if (delta_size < full_row_size) {
            metrics.bytes_saved += (full_row_size - delta_size);
        }
    } else {
        metrics.full_row_updates++;
        metrics.bytes_full_row += full_row_size;
    }
#endif
}

} // namespace mako

#endif // _TPCC_COLUMN_DELTA_H_
