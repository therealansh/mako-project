#ifndef _YCSB_SCHEMA_H_
#define _YCSB_SCHEMA_H_

#include "../record/encoder.h"
#include "../record/inline_str.h"
#include "../macros.h"
#include "lib/column_delta.h"

// ============================================================================
// YCSB Schema Definition using DO_STRUCT
// This enables column-delta MVCC optimization for YCSB benchmark
// ============================================================================

// YCSB key is a 64-bit integer
#define YCSB_KEY_FIELDS(x, y) \
  x(uint64_t, y_key)

// YCSB value has 10 fields of 10 bytes each (100 bytes total)
// This matches the standard YCSB record size
// We use 10 separate fields to enable fine-grained column-delta updates
#define YCSB_VALUE_FIELDS(x, y) \
  x(inline_str_fixed<10>, y_field0) \
  y(inline_str_fixed<10>, y_field1) \
  y(inline_str_fixed<10>, y_field2) \
  y(inline_str_fixed<10>, y_field3) \
  y(inline_str_fixed<10>, y_field4) \
  y(inline_str_fixed<10>, y_field5) \
  y(inline_str_fixed<10>, y_field6) \
  y(inline_str_fixed<10>, y_field7) \
  y(inline_str_fixed<10>, y_field8) \
  y(inline_str_fixed<10>, y_field9)

DO_STRUCT(ycsb_kv, YCSB_KEY_FIELDS, YCSB_VALUE_FIELDS)

// ============================================================================
// Parent struct specialization for YCSB
// ============================================================================

namespace mako {

template<>
struct parent_struct<ycsb_kv::value> {
    using type = ycsb_kv;
};

// YCSB column IDs (matches YCSB_VALUE_FIELDS order)
enum class YcsbColId : uint8_t {
    Y_FIELD0 = 0,
    Y_FIELD1 = 1,
    Y_FIELD2 = 2,
    Y_FIELD3 = 3,
    Y_FIELD4 = 4,
    Y_FIELD5 = 5,
    Y_FIELD6 = 6,
    Y_FIELD7 = 7,
    Y_FIELD8 = 8,
    Y_FIELD9 = 9,
    YCSB_NFIELDS = 10
};

// Helper to initialize a YCSB value with a character
inline void initYcsbValue(ycsb_kv::value& v, char c) {
    char buf[11];
    memset(buf, c, 10);
    buf[10] = '\0';
    v.y_field0 = inline_str_fixed<10>(buf, 10);
    v.y_field1 = inline_str_fixed<10>(buf, 10);
    v.y_field2 = inline_str_fixed<10>(buf, 10);
    v.y_field3 = inline_str_fixed<10>(buf, 10);
    v.y_field4 = inline_str_fixed<10>(buf, 10);
    v.y_field5 = inline_str_fixed<10>(buf, 10);
    v.y_field6 = inline_str_fixed<10>(buf, 10);
    v.y_field7 = inline_str_fixed<10>(buf, 10);
    v.y_field8 = inline_str_fixed<10>(buf, 10);
    v.y_field9 = inline_str_fixed<10>(buf, 10);
}

// Helper to update specific fields in a YCSB value
// fields_to_update is a bitmask of which fields to update
inline void updateYcsbFields(ycsb_kv::value& v, uint32_t fields_to_update, char c) {
    char buf[11];
    memset(buf, c, 10);
    buf[10] = '\0';
    inline_str_fixed<10> new_val(buf, 10);
    
    if (fields_to_update & (1 << 0)) v.y_field0 = new_val;
    if (fields_to_update & (1 << 1)) v.y_field1 = new_val;
    if (fields_to_update & (1 << 2)) v.y_field2 = new_val;
    if (fields_to_update & (1 << 3)) v.y_field3 = new_val;
    if (fields_to_update & (1 << 4)) v.y_field4 = new_val;
    if (fields_to_update & (1 << 5)) v.y_field5 = new_val;
    if (fields_to_update & (1 << 6)) v.y_field6 = new_val;
    if (fields_to_update & (1 << 7)) v.y_field7 = new_val;
    if (fields_to_update & (1 << 8)) v.y_field8 = new_val;
    if (fields_to_update & (1 << 9)) v.y_field9 = new_val;
}

// Helper to record YCSB update metrics
inline void recordYcsbUpdateMetrics() {
#if MAKO_ENABLE_COLUMN_DELTAS
    auto& metrics = getColumnDeltaMetrics();
    metrics.ycsb_updates++;
#endif
}

} // namespace mako

#endif // _YCSB_SCHEMA_H_
