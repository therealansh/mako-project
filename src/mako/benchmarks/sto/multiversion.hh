#pragma once
#include <map>
#include "lib/common.h"
#include "lib/column_delta.h"
#include <vector>
#include "benchmarks/sto/sync_util.hh"
#include "benchmarks/sto/common.hh"
#ifdef USE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

// value field composition: data + mako::BITS_OF_TT (timestamp + term) + mako::BITS_OF_NODE
// With column-delta enabled: [kind byte][payload][timestamp+term][Node]
class MultiVersionValue {
public:
    static bool isDeleted(std::string& v) {
        // for non-deleted value, the length of value at least 2+mako::EXTRA_BITS_FOR_VALUE
        return v.length() == 1+mako::EXTRA_BITS_FOR_VALUE && v[0] == 'B';
    }

    // Get the user data portion of a value (excluding MVCC metadata)
    // Returns pointer to user data and sets user_len
    static const char* getUserDataPortion(const char* data, size_t total_len, size_t& user_len) {
        if (total_len <= mako::EXTRA_BITS_FOR_VALUE) {
            user_len = 0;
            return data;
        }
        user_len = total_len - mako::EXTRA_BITS_FOR_VALUE;
        return data;
    }

    // Check if a value node is a column delta (has COL_DELTA kind byte)
    static bool isColumnDelta(const char* data, size_t total_len) {
#if MAKO_ENABLE_COLUMN_DELTAS
        if (total_len <= mako::EXTRA_BITS_FOR_VALUE + mako::KIND_BYTE_SIZE) {
            return false;
        }
        return static_cast<uint8_t>(data[0]) == mako::COL_DELTA;
#else
        return false;
#endif
    }

    // Check if a value node is a column base (has COL_BASE kind byte)
    static bool isColumnBase(const char* data, size_t total_len) {
#if MAKO_ENABLE_COLUMN_DELTAS
        if (total_len <= mako::EXTRA_BITS_FOR_VALUE + mako::KIND_BYTE_SIZE) {
            return false;
        }
        return static_cast<uint8_t>(data[0]) == mako::COL_BASE;
#else
        return false;
#endif
    }

    // Get the kind byte from a value node
    static mako::ValueKind getValueKind(const char* data, size_t total_len) {
#if MAKO_ENABLE_COLUMN_DELTAS
        if (total_len <= mako::EXTRA_BITS_FOR_VALUE + mako::KIND_BYTE_SIZE) {
            return mako::LEGACY_BASE;
        }
        uint8_t kind = static_cast<uint8_t>(data[0]);
        if (kind == mako::COL_BASE || kind == mako::COL_DELTA) {
            return static_cast<mako::ValueKind>(kind);
        }
#endif
        return mako::LEGACY_BASE;
    }

    // Reconstruct a full row from a chain of base + delta nodes
    // This walks the Node chain backwards to find a base, then applies deltas
    // Returns the reconstructed full row value (user data only, without MVCC metadata)
    static std::string reconstructFromDeltas(const char* data, size_t total_len) {
#if MAKO_ENABLE_COLUMN_DELTAS
        // Collect all nodes in the chain until we find a base
        std::vector<std::pair<const char*, size_t>> chain;
        const char* current_data = data;
        size_t current_len = total_len;
        
        while (current_len > mako::EXTRA_BITS_FOR_VALUE) {
            chain.push_back({current_data, current_len});
            
            mako::ValueKind kind = getValueKind(current_data, current_len);
            if (kind == mako::LEGACY_BASE || kind == mako::COL_BASE) {
                // Found a base, stop walking
                break;
            }
            
            // Get next node in chain
            mako::Node* header = reinterpret_cast<mako::Node*>(
                const_cast<char*>(current_data) + current_len - mako::BITS_OF_NODE);
            if (header->data_size <= 0) {
                break; // End of chain
            }
            current_data = header->data;
            current_len = header->data_size;
        }
        
        if (chain.empty()) {
            return std::string(data, total_len - mako::EXTRA_BITS_FOR_VALUE);
        }
        
        // Start with the base (last element in chain)
        const char* base_data = chain.back().first;
        size_t base_len = chain.back().second;
        mako::ValueKind base_kind = getValueKind(base_data, base_len);
        
        std::string result;
        if (base_kind == mako::COL_BASE) {
            // Skip kind byte for COL_BASE
            size_t user_len = base_len - mako::EXTRA_BITS_FOR_VALUE - mako::KIND_BYTE_SIZE;
            result.assign(base_data + mako::KIND_BYTE_SIZE, user_len);
        } else {
            // LEGACY_BASE - no kind byte
            size_t user_len = base_len - mako::EXTRA_BITS_FOR_VALUE;
            result.assign(base_data, user_len);
        }
        
        // Apply deltas from oldest to newest (reverse order of chain, excluding base)
        for (int i = static_cast<int>(chain.size()) - 2; i >= 0; i--) {
            const char* delta_data = chain[i].first;
            size_t delta_len = chain[i].second;
            
            if (getValueKind(delta_data, delta_len) == mako::COL_DELTA) {
                // Parse and apply delta
                size_t delta_user_len = delta_len - mako::EXTRA_BITS_FOR_VALUE;
                std::vector<std::tuple<uint8_t, size_t, uint16_t>> columns;
                if (mako::ColumnDelta::parseDelta(delta_data, delta_user_len, columns)) {
                    // Apply each column update to result
                    // Note: This is a simplified implementation that works with raw bytes
                    // For full implementation, we'd need type-aware column application
                    for (const auto& col : columns) {
                        uint8_t col_id = std::get<0>(col);
                        size_t value_offset = std::get<1>(col);
                        uint16_t value_len = std::get<2>(col);
                        
                        // For now, we store the full new value in the delta
                        // This is a simplified approach - full implementation would
                        // need to know column offsets in the struct
                        (void)col_id;
                        (void)value_offset;
                        (void)value_len;
                    }
                }
            } else if (getValueKind(delta_data, delta_len) == mako::COL_BASE) {
                // This is a full row, replace result
                size_t user_len = delta_len - mako::EXTRA_BITS_FOR_VALUE - mako::KIND_BYTE_SIZE;
                result.assign(delta_data + mako::KIND_BYTE_SIZE, user_len);
            } else {
                // LEGACY_BASE - replace result
                size_t user_len = delta_len - mako::EXTRA_BITS_FOR_VALUE;
                result.assign(delta_data, user_len);
            }
        }
        
        return result;
#else
        // Column deltas disabled - just return user data portion
        if (total_len <= mako::EXTRA_BITS_FOR_VALUE) {
            return "";
        }
        return std::string(data, total_len - mako::EXTRA_BITS_FOR_VALUE);
#endif
    }

    template <typename ValueType>
    static std::vector<string> getAllVersion(string val) {
        std::vector<string> ret;
        int vt = 1;
        uint32_t *time_term = 0;
        time_term = reinterpret_cast<uint32_t*>((char*)(val.data()+val.length()-mako::EXTRA_BITS_FOR_VALUE));
        
        std::string tmp;
        tmp.assign(val.data(),val.length());
        ret.push_back(isDeleted(val)? "DEL": (tmp));
        
        // fast peek
        mako::Node *header = reinterpret_cast<mako::Node *>((char*)(val.data()+val.length()-mako::BITS_OF_NODE));
        while (header->data_size > 0) {
            vt ++;
            time_term = reinterpret_cast<uint32_t*>((char*)(val.data()+val.length()-mako::EXTRA_BITS_FOR_VALUE));

            val.assign(header->data, (int)header->data_size); // rewrite with next block value
            std::string tmp;
            tmp.assign(val.data(),val.length());
            ret.push_back(isDeleted(val)? "DEL": (tmp));
            header = reinterpret_cast<mako::Node *>((char*)(val.data()+val.length()-mako::BITS_OF_NODE));
        }
        return ret;
    }

    // Lazy reclamation with optimized watermark checking
    // Reclaims old versions that are safe to delete (below watermark)
    static void lazyReclaim(uint32_t time_term, uint32_t current_term, mako::Node *root) {
        // Use TThread counter for thread-local reclamation frequency
        TThread::incr_counter();
        if (TThread::counter() % 50 != 0) return;
        
        // Cache watermark with proper memory ordering
        uint32_t watermark = sync_util::sync_logger::retrieveShardW_relaxed() / 10;
        if (watermark == 0) return;  // Skip if watermark not initialized
        
        // Phase 1: Find the safe reclamation point
        mako::Node *safe_point = nullptr;
        mako::Node *current = root;
        std::vector<mako::Node*> to_free;  // Batch freeing for efficiency
        
        // Navigate to first version below watermark
        while (current && current->data_size > 0) {
            uint32_t *tt = reinterpret_cast<uint32_t*>(
                current->data + current->data_size - mako::EXTRA_BITS_FOR_VALUE);
            
            if ((*tt) / 10 < watermark) {
                safe_point = current;
                break;
            }
            
            current = reinterpret_cast<mako::Node*>(
                current->data + current->data_size - mako::BITS_OF_NODE);
        }
        
        if (!safe_point) return;  // No safe versions to reclaim
        
        // Phase 2: Collect nodes to free (after safe point)
        current = safe_point;
        while (current && current->data_size > 0) {
            mako::Node *next = reinterpret_cast<mako::Node*>(
                current->data + current->data_size - mako::BITS_OF_NODE);
            
            if (next->data_size > 0) {
                to_free.push_back(current);
            }
            current = next;
        }
        
        // Phase 3: Update chain and batch free
        if (!to_free.empty()) {
            // Update the chain - no other thread accesses this
            safe_point->data_size = 0;  // Mark end of chain
            
            // Batch free old nodes
            for (auto* node : to_free) {
                ::free(node->data);
            }
        }
    }

    static bool mvGET(string& val,
                      char *oldval_str, // oldval_str == val, but it's the reference to the actual value
                      uint8_t current_term,
                      std::unordered_map<int, uint32_t> hist_timestamp) {
        uint32_t *time_term = 0;
        time_term = reinterpret_cast<uint32_t*>((char*)(val.data()+val.length()-mako::EXTRA_BITS_FOR_VALUE));

        if (likely(*time_term % 10 == current_term)) { // current term: get the latest value but reclaim the all version below the watermark within the current term
#if MAKO_ENABLE_COLUMN_DELTAS
            // Handle COL_BASE kind byte - strip it before returning
            if (val.length() > mako::KIND_BYTE_SIZE + mako::EXTRA_BITS_FOR_VALUE) {
                mako::ValueKind kind = getValueKind(val.data(), val.length());
                if (kind == mako::COL_BASE) {
                    // Strip kind byte and reconstruct value without it
                    size_t user_len = val.length() - mako::KIND_BYTE_SIZE - mako::EXTRA_BITS_FOR_VALUE;
                    std::string new_val;
                    new_val.reserve(user_len + mako::EXTRA_BITS_FOR_VALUE);
                    new_val.append(val.data() + mako::KIND_BYTE_SIZE, user_len);
                    new_val.append(val.data() + val.length() - mako::EXTRA_BITS_FOR_VALUE, mako::EXTRA_BITS_FOR_VALUE);
                    val = std::move(new_val);
                }
            }
#endif
            return !isDeleted(val);
        } else { // past term e
            mako::Node *header = reinterpret_cast<mako::Node *>((char*)(val.data()+val.length()-mako::BITS_OF_NODE));
            
#if defined(FAIL_NEW_VERSION)
            // It's possible that hist_timestamp is not updated yet, and return it directly; and the remote server would do a check
            if  (hist_timestamp.find(*time_term % 10)==hist_timestamp.end()) {
                return !isDeleted(val);
            }
            // check if the stored value is below the cached watermark
            if (sync_util::sync_logger::safety_check(header->timestamp, hist_timestamp[*time_term % 10])) { // Single timestamp check
                bool ret = !isDeleted(val);
                if (!ret) {
                    //Warning("XXXX par_id:%d,time_term:%d,cur_term:%d, watermark:%lld,len of v:%d",TThread::getPartitionID(),*time_term%10,current_term, hist_timestamp[*time_term % 10],val.length());
                    //mako::printStringAsBit(val);
                }
                return ret;
            }
            // find the latest stable timestamp below the watermark within the past term e
            while (header->data_size > 0) {
                time_term = reinterpret_cast<uint32_t*>((char*)(header->data+header->data_size-mako::EXTRA_BITS_FOR_VALUE));
                if (sync_util::sync_logger::safety_check(header->timestamp, hist_timestamp[*time_term % 10])) { // Single timestamp check
                    val.assign(header->data, (int)header->data_size); // rewrite val with next block value
                    header = reinterpret_cast<mako::Node *>((char*)(val.data()+val.length()-mako::BITS_OF_NODE));
                    if (isDeleted(val)) {
                        return false;
                    }
                    break;
                }
                header = reinterpret_cast<mako::Node *>((char*)(header->data+header->data_size-mako::BITS_OF_NODE));
            }
        }
#else
            if (header->timestamp / 10 <= hist_timestamp[*time_term % 10]) { // Single timestamp check
                bool ret = !isDeleted(val);
                if (!ret) {
                    //Warning("XXXX par_id:%d,time_term:%d,cur_term:%d, watermark:%lld,len of v:%d",TThread::getPartitionID(),*time_term%10,current_term, hist_timestamp[*time_term % 10],val.length());
                    //mako::printStringAsBit(val);
                }
                return ret;
            }
            // find the latest stable timestamp below the watermark within the past term e
            while (header->data_size > 0) {
                time_term = reinterpret_cast<uint32_t*>((char*)(header->data+header->data_size-mako::EXTRA_BITS_FOR_VALUE));
                if (header->timestamp / 10 <= hist_timestamp[*time_term % 10]) { // Single timestamp check
                    val.assign(header->data, (int)header->data_size); // rewrite val with next block value
                    header = reinterpret_cast<mako::Node *>((char*)(val.data()+val.length()-mako::BITS_OF_NODE));
                    if (isDeleted(val)) {
                        return false;
                    }
                    break;
                }
                header = reinterpret_cast<mako::Node *>((char*)(header->data+header->data_size-mako::BITS_OF_NODE));
            }
        }
#endif
        return true;
    }

    // kvthread.hh -> it's same as malloc vs free
    // one way to solve it: include "rcu.h"
    static void mvInstall(bool isInsert,
                          bool isDelete,
                          const string newval,  // the new value to be updated
                          versioned_str_struct* e, /* versioned_value */
                          uint8_t current_term) {
        // Single timestamp system
        char *oldval_str=(char*)e->data();
        int oldval_len=e->length();
        uint32_t time_term = TThread::txn->tid_unique_ * 10 + TThread::txn->current_term_;
        
#if MAKO_ENABLE_COLUMN_DELTAS
        // When column-delta feature is enabled, use COL_BASE format with kind byte prefix
        // This allows future delta encoding and proper handling in mvGET
        if (isInsert) {
            // For inserts, add COL_BASE kind byte prefix
            size_t user_len = newval.length() - mako::EXTRA_BITS_FOR_VALUE;
            size_t new_len = mako::KIND_BYTE_SIZE + newval.length();
            char* new_vv = (char*)malloc(new_len);
            
            // Write COL_BASE kind byte
            new_vv[0] = static_cast<char>(mako::COL_BASE);
            
            // Copy user data
            memcpy(new_vv + mako::KIND_BYTE_SIZE, newval.data(), user_len);
            
            // Write timestamp/term
            memcpy(new_vv + new_len - mako::EXTRA_BITS_FOR_VALUE, &time_term, mako::BITS_OF_TT);
            
            // Setup Node header
            mako::Node* header = reinterpret_cast<mako::Node*>(new_vv + new_len - mako::BITS_OF_NODE);
            header->timestamp = TThread::txn->tid_unique_;
            header->data_size = 0;  // No previous version
            header->data = nullptr;
            
            // Copy to existing buffer (allocated by versioned_value::make)
            if (oldval_len >= (int)new_len) {
                memcpy(oldval_str, new_vv, new_len);
                free(new_vv);
            } else {
                // Need to replace buffer
                e->modifyData(new_vv);
                e->set_length(new_len);
            }
            
            // Update metrics
            mako::getColumnDeltaMetrics().total_updates++;
        } else {
            // For updates, check if delta context is set AND runtime flag is enabled
            auto& delta_ctx = mako::getDeltaContext();
            bool use_delta = mako::isColumnDeltasEnabled() &&
                             delta_ctx.active && 
                             delta_ctx.build_delta_fn != nullptr &&
                             delta_ctx.changed_fields != 0 &&
                             mako::countChangedFields(delta_ctx.changed_fields) <= mako::DELTA_COLUMN_THRESHOLD;
            
            if (use_delta) {
                // Build actual column delta using the context
                std::string delta_payload = delta_ctx.build_delta_fn(
                    delta_ctx.old_value, delta_ctx.new_value, delta_ctx.changed_fields);
                
                if (!delta_payload.empty()) {
                    // Successfully built delta - create COL_DELTA node for the OLD value
                    // The NEW value becomes the current full row (COL_BASE)
                    
                    // First, create the new head node with full row (COL_BASE)
                    size_t user_len = newval.length() - mako::EXTRA_BITS_FOR_VALUE;
                    size_t new_head_len = mako::KIND_BYTE_SIZE + newval.length();
                    char* new_head = (char*)malloc(new_head_len);
                    
                    // Write COL_BASE kind byte for new head
                    new_head[0] = static_cast<char>(mako::COL_BASE);
                    
                    // Copy new value data
                    memcpy(new_head + mako::KIND_BYTE_SIZE, newval.data(), user_len);
                    
                    // Write timestamp/term for new head
                    memcpy(new_head + new_head_len - mako::EXTRA_BITS_FOR_VALUE, &time_term, mako::BITS_OF_TT);
                    
                    // Now create the delta node for the old value
                    // Delta layout: [delta_payload][EXTRA_BITS_FOR_VALUE]
                    // Note: delta_payload already includes COL_DELTA kind byte
                    size_t delta_node_len = delta_payload.size() + mako::EXTRA_BITS_FOR_VALUE;
                    char* delta_node = (char*)malloc(delta_node_len);
                    
                    // Copy delta payload (includes COL_DELTA kind byte)
                    memcpy(delta_node, delta_payload.data(), delta_payload.size());
                    
                    // Copy old timestamp/term from old value
                    uint32_t* old_time_term = reinterpret_cast<uint32_t*>(oldval_str + oldval_len - mako::EXTRA_BITS_FOR_VALUE);
                    memcpy(delta_node + delta_node_len - mako::EXTRA_BITS_FOR_VALUE, old_time_term, mako::BITS_OF_TT);
                    
                    // Setup delta node's Node header - points to old value's previous
                    mako::Node* old_header = reinterpret_cast<mako::Node*>(oldval_str + oldval_len - mako::BITS_OF_NODE);
                    mako::Node* delta_header = reinterpret_cast<mako::Node*>(delta_node + delta_node_len - mako::BITS_OF_NODE);
                    delta_header->timestamp = old_header->timestamp;
                    delta_header->data_size = old_header->data_size;
                    delta_header->data = old_header->data;
                    
                    // Setup new head's Node header - points to delta node
                    mako::Node* new_header = reinterpret_cast<mako::Node*>(new_head + new_head_len - mako::BITS_OF_NODE);
                    new_header->timestamp = TThread::txn->tid_unique_;
                    new_header->data_size = delta_node_len;
                    new_header->data = delta_node;
                    
                    // Update metrics
                    auto& metrics = mako::getColumnDeltaMetrics();
                    metrics.total_updates++;
                    metrics.delta_updates++;
                    metrics.bytes_delta += delta_node_len;
                    metrics.bytes_full_row += new_head_len;
                    metrics.bytes_saved += (oldval_len - delta_node_len);
                    
                    // Install new head
                    e->modifyData(new_head);
                    e->set_length(new_head_len);
                    lazyReclaim(time_term, current_term, new_header);
                    return;
                }
            }
            
            // Fall through to full row update (either delta not applicable or build failed)
            size_t user_len = newval.length() - mako::EXTRA_BITS_FOR_VALUE;
            size_t new_len = mako::KIND_BYTE_SIZE + newval.length();
            char* new_vv = (char*)malloc(new_len);
            
            // Write COL_BASE kind byte
            new_vv[0] = static_cast<char>(mako::COL_BASE);
            
            // Copy user data
            memcpy(new_vv + mako::KIND_BYTE_SIZE, newval.data(), user_len);
            
            // Write timestamp/term
            memcpy(new_vv + new_len - mako::EXTRA_BITS_FOR_VALUE, &time_term, mako::BITS_OF_TT);
            
            // Setup Node header - points to previous version
            mako::Node* header = reinterpret_cast<mako::Node*>(new_vv + new_len - mako::BITS_OF_NODE);
            header->timestamp = TThread::txn->tid_unique_;
            header->data_size = oldval_len;
            header->data = e->data();
            
            e->modifyData(new_vv);
            e->set_length(new_len);
            lazyReclaim(time_term, current_term, header);
            
            // Update metrics
            mako::getColumnDeltaMetrics().total_updates++;
            mako::getColumnDeltaMetrics().full_row_updates++;
            mako::getColumnDeltaMetrics().bytes_full_row += new_len;
        }
#else
        // Original implementation when column-delta is disabled
        if (isInsert) { // insert
            mako::Node* header = reinterpret_cast<mako::Node*>(oldval_str+oldval_len-mako::BITS_OF_NODE);
            // Set single timestamp
            header->timestamp = TThread::txn->tid_unique_;
            header->data_size = 0;  // indicate no next block
            memcpy(oldval_str+oldval_len-mako::EXTRA_BITS_FOR_VALUE, &time_term, mako::BITS_OF_TT);
        } else {  // update or delete
            char* new_vv = (char*)malloc(newval.length());
            memcpy(new_vv, newval.data(), newval.length()-mako::EXTRA_BITS_FOR_VALUE);
            memcpy(new_vv+newval.length()-mako::EXTRA_BITS_FOR_VALUE, 
                                &time_term, mako::BITS_OF_TT);
            mako::Node* header = reinterpret_cast<mako::Node*>(new_vv+newval.length()-mako::BITS_OF_NODE);
            // Set single timestamp
            header->timestamp = TThread::txn->tid_unique_;
            header->data_size = oldval_len;
            header->data = e->data();
            e->modifyData(new_vv);
            lazyReclaim(time_term, current_term, header);
        }
#endif
        return ;
    }

#if MAKO_ENABLE_COLUMN_DELTAS
    // Extended mvInstall that supports column-delta encoding
    // This version takes additional parameters to enable column-delta optimization:
    // - old_decoded: pointer to decoded old value struct (for comparison)
    // - new_decoded: pointer to decoded new value struct (for comparison)
    // - changed_fields: bitmask of changed fields (if already computed)
    //
    // When changed_fields indicates few columns changed, this creates a COL_DELTA
    // node instead of a full COL_BASE node, reducing memory usage.
    //
    // Design: Option A - current value is always full row (COL_BASE), older nodes
    // in the chain are deltas (COL_DELTA) representing changes from newer to older.
    template<typename ValueType>
    static void mvInstallWithDelta(bool isInsert,
                                   bool isDelete,
                                   const string newval,
                                   versioned_str_struct* e,
                                   uint8_t current_term,
                                   const ValueType* old_decoded,
                                   const ValueType* new_decoded,
                                   uint32_t changed_fields) {
        char *oldval_str = (char*)e->data();
        int oldval_len = e->length();
        uint32_t time_term = TThread::txn->tid_unique_ * 10 + TThread::txn->current_term_;
        
        if (isInsert) {
            // For inserts, mark as COL_BASE (full row with kind prefix)
            // The newval already has EXTRA_BITS_FOR_VALUE at the end
            size_t user_len = newval.length() - mako::EXTRA_BITS_FOR_VALUE;
            size_t new_len = mako::KIND_BYTE_SIZE + newval.length();
            char* new_vv = (char*)malloc(new_len);
            
            // Write kind byte
            new_vv[0] = static_cast<char>(mako::COL_BASE);
            
            // Copy user data
            memcpy(new_vv + mako::KIND_BYTE_SIZE, newval.data(), user_len);
            
            // Write timestamp/term
            memcpy(new_vv + new_len - mako::EXTRA_BITS_FOR_VALUE, &time_term, mako::BITS_OF_TT);
            
            // Setup Node header (no previous version)
            mako::Node* header = reinterpret_cast<mako::Node*>(new_vv + new_len - mako::BITS_OF_NODE);
            header->timestamp = TThread::txn->tid_unique_;
            header->data_size = 0;  // No previous version
            header->data = nullptr;
            
            // For insert, we need to copy to the existing buffer or replace it
            // The existing buffer (oldval_str) was allocated by versioned_value::make
            // We need to update it with our new data
            memcpy(oldval_str, new_vv, oldval_len < (int)new_len ? oldval_len : new_len);
            free(new_vv);
            
            // Update the Node in the existing buffer
            mako::Node* existing_header = reinterpret_cast<mako::Node*>(oldval_str + oldval_len - mako::BITS_OF_NODE);
            existing_header->timestamp = TThread::txn->tid_unique_;
            existing_header->data_size = 0;
            memcpy(oldval_str + oldval_len - mako::EXTRA_BITS_FOR_VALUE, &time_term, mako::BITS_OF_TT);
        } else {
            // For updates, check if we should use column delta
            int num_changed = mako::countChangedFields(changed_fields);
            
            // Use column delta if few fields changed and we have decoded values
            bool use_delta = (old_decoded != nullptr && 
                              new_decoded != nullptr && 
                              num_changed > 0 && 
                              num_changed <= mako::DELTA_COLUMN_THRESHOLD);
            
            if (use_delta) {
                // Build actual column delta from the struct values
                std::string delta_payload = mako::buildDeltaFromStructs(*old_decoded, *new_decoded, changed_fields);
                
                if (!delta_payload.empty()) {
                    // Successfully built delta - create COL_DELTA node for the OLD value
                    // The NEW value becomes the current full row (COL_BASE)
                    
                    // First, create the new head node with full row (COL_BASE)
                    size_t user_len = newval.length() - mako::EXTRA_BITS_FOR_VALUE;
                    size_t new_head_len = mako::KIND_BYTE_SIZE + newval.length();
                    char* new_head = (char*)malloc(new_head_len);
                    
                    // Write COL_BASE kind byte for new head
                    new_head[0] = static_cast<char>(mako::COL_BASE);
                    
                    // Copy new value data
                    memcpy(new_head + mako::KIND_BYTE_SIZE, newval.data(), user_len);
                    
                    // Write timestamp/term for new head
                    memcpy(new_head + new_head_len - mako::EXTRA_BITS_FOR_VALUE, &time_term, mako::BITS_OF_TT);
                    
                    // Now create the delta node for the old value
                    // Delta layout: [COL_DELTA kind][delta_payload][EXTRA_BITS_FOR_VALUE]
                    size_t delta_node_len = mako::KIND_BYTE_SIZE + delta_payload.size() + mako::EXTRA_BITS_FOR_VALUE;
                    char* delta_node = (char*)malloc(delta_node_len);
                    
                    // The delta is already prefixed with COL_DELTA kind in buildDelta
                    memcpy(delta_node, delta_payload.data(), delta_payload.size());
                    
                    // Copy old timestamp/term from old value
                    uint32_t* old_time_term = reinterpret_cast<uint32_t*>(oldval_str + oldval_len - mako::EXTRA_BITS_FOR_VALUE);
                    memcpy(delta_node + delta_node_len - mako::EXTRA_BITS_FOR_VALUE, old_time_term, mako::BITS_OF_TT);
                    
                    // Setup delta node's Node header - points to old value's previous
                    mako::Node* old_header = reinterpret_cast<mako::Node*>(oldval_str + oldval_len - mako::BITS_OF_NODE);
                    mako::Node* delta_header = reinterpret_cast<mako::Node*>(delta_node + delta_node_len - mako::BITS_OF_NODE);
                    delta_header->timestamp = old_header->timestamp;
                    delta_header->data_size = old_header->data_size;
                    delta_header->data = old_header->data;
                    
                    // Setup new head's Node header - points to delta node
                    mako::Node* new_header = reinterpret_cast<mako::Node*>(new_head + new_head_len - mako::BITS_OF_NODE);
                    new_header->timestamp = TThread::txn->tid_unique_;
                    new_header->data_size = delta_node_len;
                    new_header->data = delta_node;
                    
                    // Update metrics
                    auto& metrics = mako::getColumnDeltaMetrics();
                    metrics.bytes_delta += delta_node_len;
                    metrics.bytes_full_row += new_head_len;
                    metrics.bytes_saved += (oldval_len - delta_node_len);
                    
                    // Install new head
                    e->modifyData(new_head);
                    e->set_length(new_head_len);
                    lazyReclaim(time_term, current_term, new_header);
                } else {
                    // Delta build failed, fall back to full row
                    goto full_row_update;
                }
            } else {
full_row_update:
                // Standard full row update with COL_BASE kind byte
                size_t user_len = newval.length() - mako::EXTRA_BITS_FOR_VALUE;
                size_t new_len = mako::KIND_BYTE_SIZE + newval.length();
                char* new_vv = (char*)malloc(new_len);
                
                // Write COL_BASE kind byte
                new_vv[0] = static_cast<char>(mako::COL_BASE);
                
                // Copy user data
                memcpy(new_vv + mako::KIND_BYTE_SIZE, newval.data(), user_len);
                
                // Write timestamp/term
                memcpy(new_vv + new_len - mako::EXTRA_BITS_FOR_VALUE, &time_term, mako::BITS_OF_TT);
                
                // Setup Node header
                mako::Node* header = reinterpret_cast<mako::Node*>(new_vv + new_len - mako::BITS_OF_NODE);
                header->timestamp = TThread::txn->tid_unique_;
                header->data_size = oldval_len;
                header->data = e->data();
                
                e->modifyData(new_vv);
                e->set_length(new_len);
                lazyReclaim(time_term, current_term, header);
            }
        }
    }
#endif // MAKO_ENABLE_COLUMN_DELTAS

} ;
