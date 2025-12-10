#ifndef _TPCC_COLUMN_DELTA_H_
#define _TPCC_COLUMN_DELTA_H_

#include "tpcc.h"
#include "lib/column_delta.h"

// ============================================================================
// Parent struct specializations for TPCC tables
// These enable the generic FieldComparator to work with TPCC value types
// ============================================================================

namespace mako {

// Customer table
template<>
struct parent_struct<customer::value> {
    using type = customer;
};

// District table
template<>
struct parent_struct<district::value> {
    using type = district;
};

// Warehouse table
template<>
struct parent_struct<warehouse::value> {
    using type = warehouse;
};

// Stock table
template<>
struct parent_struct<stock::value> {
    using type = stock;
};

// Order line table
template<>
struct parent_struct<order_line::value> {
    using type = order_line;
};

// History table
template<>
struct parent_struct<history::value> {
    using type = history;
};

// New order table
template<>
struct parent_struct<new_order::value> {
    using type = new_order;
};

// Order table
template<>
struct parent_struct<oorder::value> {
    using type = oorder;
};

// Item table
template<>
struct parent_struct<item::value> {
    using type = item;
};

// Stock data table
template<>
struct parent_struct<stock_data::value> {
    using type = stock_data;
};

// Customer data table
template<>
struct parent_struct<customer_data::value> {
    using type = customer_data;
};

// ============================================================================
// Column ID enums for TPCC tables (for debugging/logging purposes)
// These IDs correspond to the field order in the *_VALUE_FIELDS macros
// ============================================================================

// Customer table column IDs (matches CUSTOMER_VALUE_FIELDS order)
enum class CustomerColId : uint8_t {
    C_DISCOUNT = 0,
    C_CREDIT = 1,
    C_LAST = 2,
    C_FIRST = 3,
    C_CREDIT_LIM = 4,
    C_BALANCE = 5,        // Frequently updated in Payment
    C_YTD_PAYMENT = 6,    // Frequently updated in Payment
    C_PAYMENT_CNT = 7,    // Frequently updated in Payment
    C_DELIVERY_CNT = 8,   // Updated in Delivery
    C_STREET_1 = 9,
    C_STREET_2 = 10,
    C_CITY = 11,
    C_STATE = 12,
    C_ZIP = 13,
    C_PHONE = 14,
    C_SINCE = 15,
    C_MIDDLE = 16,
    CUSTOMER_NFIELDS = 17
};

// District table column IDs (matches DISTRICT_VALUE_FIELDS order)
enum class DistrictColId : uint8_t {
    D_YTD = 0,            // Updated in Payment
    D_TAX = 1,
    D_NEXT_O_ID = 2,      // Updated in NewOrder
    D_NAME = 3,
    D_STREET_1 = 4,
    D_STREET_2 = 5,
    D_CITY = 6,
    D_STATE = 7,
    D_ZIP = 8,
    DISTRICT_NFIELDS = 9
};

// Warehouse table column IDs (matches WAREHOUSE_VALUE_FIELDS order)
enum class WarehouseColId : uint8_t {
    W_YTD = 0,            // Updated in Payment
    W_TAX = 1,
    W_NAME = 2,
    W_STREET_1 = 3,
    W_STREET_2 = 4,
    W_CITY = 5,
    W_STATE = 6,
    W_ZIP = 7,
    WAREHOUSE_NFIELDS = 8
};

// Stock table column IDs (matches STOCK_VALUE_FIELDS order)
enum class StockColId : uint8_t {
    S_QUANTITY = 0,       // Updated in NewOrder
    S_YTD = 1,            // Updated in NewOrder
    S_ORDER_CNT = 2,      // Updated in NewOrder
    S_REMOTE_CNT = 3,     // Updated in NewOrder
    STOCK_NFIELDS = 4
};

// ============================================================================
// Metrics tracking helpers for TPCC tables
// ============================================================================

// Record metrics for TPCC table updates
// This records metrics at the put() call site, which includes attempted updates
// (not just committed ones). For TPCC, abort rates are typically ~0%, so this
// is a good approximation of actual committed updates.
inline void recordTpccUpdateMetrics(const char* table_name, uint32_t changed_fields, 
                                     size_t full_row_size, size_t delta_size) {
#if MAKO_ENABLE_COLUMN_DELTAS
    // Only record metrics if column-delta is enabled at runtime
    if (!isColumnDeltasEnabled()) {
        return;
    }
    
    auto& metrics = getColumnDeltaMetrics();
    metrics.total_updates++;
    
    // Track per-table metrics
    if (strcmp(table_name, "customer") == 0) {
        metrics.customer_updates++;
    } else if (strcmp(table_name, "warehouse") == 0) {
        metrics.warehouse_updates++;
    } else if (strcmp(table_name, "district") == 0) {
        metrics.district_updates++;
    }
    
    // Track byte metrics
    metrics.bytes_full_row += full_row_size;
    
    // Determine if this would be a delta update (few fields changed)
    uint32_t num_changed = countChangedFields(changed_fields);
    if (num_changed > 0 && num_changed <= DELTA_COLUMN_THRESHOLD) {
        metrics.delta_updates++;
        metrics.bytes_delta += delta_size;
        if (full_row_size > delta_size) {
            metrics.bytes_saved += (full_row_size - delta_size);
        }
    } else {
        metrics.full_row_updates++;
    }
#else
    (void)table_name;
    (void)changed_fields;
    (void)full_row_size;
    (void)delta_size;
#endif
}

} // namespace mako

#endif // _TPCC_COLUMN_DELTA_H_
