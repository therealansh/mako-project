//
// Column-Delta MVCC Evaluation Test
// Runs A/B comparison: Baseline (full row) vs Column-Delta (only changed columns)
//

#include <iostream>
#include <chrono>
#include <thread>
#include <algorithm>
#include <mako.hh>
#include <examples/common.h>
#include "../src/mako/benchmarks/tpcc.h"
#include "../src/mako/benchmarks/tpcc_column_delta.h"

using namespace std;

// Results structure for storing benchmark results
struct BenchmarkResults {
    double throughput;           // payments/sec
    double avg_latency_us;       // microseconds
    double min_latency_us;
    double max_latency_us;
    uint64_t total_updates;
    uint64_t delta_updates;
    uint64_t bytes_full_row;
    uint64_t bytes_delta;
    uint64_t bytes_saved;
};

class ColumnDeltaEvalWorker {
public:
    ColumnDeltaEvalWorker(abstract_db *db) : db(db) {
        txn_obj_buf.reserve(str_arena::MinStrReserveLength);
        txn_obj_buf.resize(db->sizeof_txn_object(0));
    }

    void initialize() {
        scoped_db_thread_ctx ctx(db, false);
        TThread::enable_multiverison();
    }

    void setup_initial_data(const char* suffix) {
        // Create warehouse table and insert initial data
        std::string w_name = std::string("warehouse_") + suffix;
        std::string d_name = std::string("district_") + suffix;
        std::string c_name = std::string("customer_") + suffix;
        
        warehouse_table = db->open_index(w_name);
        district_table = db->open_index(d_name);
        customer_table = db->open_index(c_name);
        
        // Insert initial warehouse
        {
            void *txn = db->new_txn(0, arena, txn_buf());
            warehouse::key k_w;
            k_w.w_id = 1;
            warehouse::value v_w;
            v_w.w_ytd = 300000.0f;
            v_w.w_tax = 0.1f;
            v_w.w_name.assign("Warehouse1");
            v_w.w_street_1.assign("123 Main St");
            v_w.w_street_2.assign("Suite 100");
            v_w.w_city.assign("Boston");
            v_w.w_state.assign("MA");
            v_w.w_zip.assign("02101");
            
            try {
                warehouse_table->put(txn, EncodeK(str(), k_w), Encode(str(), v_w));
                db->commit_txn(txn);
            } catch (abstract_db::abstract_abort_exception &ex) {
                db->abort_txn(txn);
            }
        }
        
        // Insert initial district
        {
            void *txn = db->new_txn(0, arena, txn_buf());
            district::key k_d;
            k_d.d_w_id = 1;
            k_d.d_id = 1;
            district::value v_d;
            v_d.d_ytd = 30000.0f;
            v_d.d_tax = 0.1f;
            v_d.d_next_o_id = 3001;
            v_d.d_name.assign("District1");
            v_d.d_street_1.assign("456 Oak Ave");
            v_d.d_street_2.assign("Floor 2");
            v_d.d_city.assign("Boston");
            v_d.d_state.assign("MA");
            v_d.d_zip.assign("02102");
            
            try {
                district_table->put(txn, EncodeK(str(), k_d), Encode(str(), v_d));
                db->commit_txn(txn);
            } catch (abstract_db::abstract_abort_exception &ex) {
                db->abort_txn(txn);
            }
        }
        
        // Insert initial customer
        {
            void *txn = db->new_txn(0, arena, txn_buf());
            customer::key k_c;
            k_c.c_w_id = 1;
            k_c.c_d_id = 1;
            k_c.c_id = 1;
            customer::value v_c;
            v_c.c_discount = 0.1f;
            v_c.c_credit.assign("GC");
            v_c.c_last.assign("Smith");
            v_c.c_first.assign("John");
            v_c.c_credit_lim = 50000.0f;
            v_c.c_balance = 10.0f;
            v_c.c_ytd_payment = 10.0f;
            v_c.c_payment_cnt = 1;
            v_c.c_delivery_cnt = 0;
            v_c.c_street_1.assign("789 Pine Rd");
            v_c.c_street_2.assign("Apt 3");
            v_c.c_city.assign("Boston");
            v_c.c_state.assign("MA");
            v_c.c_zip.assign("02103");
            v_c.c_phone.assign("6175551234");
            v_c.c_since = 20200101;
            v_c.c_middle.assign("NM");
            
            try {
                customer_table->put(txn, EncodeK(str(), k_c), Encode(str(), v_c));
                db->commit_txn(txn);
            } catch (abstract_db::abstract_abort_exception &ex) {
                db->abort_txn(txn);
            }
        }
    }

    BenchmarkResults run_payment_simulation(int num_payments, bool enable_column_delta) {
        BenchmarkResults results = {};
        
#if MAKO_ENABLE_COLUMN_DELTAS
        mako::getColumnDeltaMetrics().reset();
        mako::setColumnDeltasEnabled(enable_column_delta);
#endif
        
        // Track latencies
        double total_latency_us = 0;
        double min_latency_us = 1e9;
        double max_latency_us = 0;
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < num_payments; i++) {
            auto payment_start = std::chrono::high_resolution_clock::now();
            float paymentAmount = 100.0f + (i % 100);
            
            // Simulate Payment transaction pattern
            // 1. Update warehouse (only w_ytd changes)
            {
                void *txn = db->new_txn(0, arena, txn_buf());
                warehouse::key k_w;
                k_w.w_id = 1;
                std::string obj_v;
                
                try {
                    warehouse_table->get(txn, EncodeK(str(), k_w), obj_v);
                    warehouse::value v_w;
                    Decode(obj_v, v_w);
                    
                    warehouse::value v_w_new(v_w);
                    v_w_new.w_ytd += paymentAmount;
                    
                    mako::recordWarehouseUpdateMetrics(v_w, v_w_new, sizeof(warehouse::value));
                    
                    warehouse_table->put(txn, EncodeK(str(), k_w), Encode(str(), v_w_new));
                    db->commit_txn(txn);
                } catch (abstract_db::abstract_abort_exception &ex) {
                    db->abort_txn(txn);
                }
            }
            
            // 2. Update district (only d_ytd changes)
            {
                void *txn = db->new_txn(0, arena, txn_buf());
                district::key k_d;
                k_d.d_w_id = 1;
                k_d.d_id = 1;
                std::string obj_v;
                
                try {
                    district_table->get(txn, EncodeK(str(), k_d), obj_v);
                    district::value v_d;
                    Decode(obj_v, v_d);
                    
                    district::value v_d_new(v_d);
                    v_d_new.d_ytd += paymentAmount;
                    
                    mako::recordDistrictUpdateMetrics(v_d, v_d_new, sizeof(district::value));
                    
                    district_table->put(txn, EncodeK(str(), k_d), Encode(str(), v_d_new));
                    db->commit_txn(txn);
                } catch (abstract_db::abstract_abort_exception &ex) {
                    db->abort_txn(txn);
                }
            }
            
            // 3. Update customer (c_balance, c_ytd_payment, c_payment_cnt change)
            {
                void *txn = db->new_txn(0, arena, txn_buf());
                customer::key k_c;
                k_c.c_w_id = 1;
                k_c.c_d_id = 1;
                k_c.c_id = 1;
                std::string obj_v;
                
                try {
                    customer_table->get(txn, EncodeK(str(), k_c), obj_v);
                    customer::value v_c;
                    Decode(obj_v, v_c);
                    
                    customer::value v_c_new(v_c);
                    v_c_new.c_balance -= paymentAmount;
                    v_c_new.c_ytd_payment += paymentAmount;
                    v_c_new.c_payment_cnt++;
                    
                    mako::recordCustomerUpdateMetrics(v_c, v_c_new, sizeof(customer::value));
                    
                    customer_table->put(txn, EncodeK(str(), k_c), Encode(str(), v_c_new));
                    db->commit_txn(txn);
                } catch (abstract_db::abstract_abort_exception &ex) {
                    db->abort_txn(txn);
                }
            }
            
            auto payment_end = std::chrono::high_resolution_clock::now();
            double latency_us = std::chrono::duration_cast<std::chrono::microseconds>(payment_end - payment_start).count();
            total_latency_us += latency_us;
            min_latency_us = std::min(min_latency_us, latency_us);
            max_latency_us = std::max(max_latency_us, latency_us);
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        // Calculate results
        results.throughput = num_payments * 1000.0 / (duration_ms > 0 ? duration_ms : 1);
        results.avg_latency_us = total_latency_us / num_payments;
        results.min_latency_us = min_latency_us;
        results.max_latency_us = max_latency_us;
        
#if MAKO_ENABLE_COLUMN_DELTAS
        auto& metrics = mako::getColumnDeltaMetrics();
        results.total_updates = metrics.total_updates.load();
        results.delta_updates = metrics.delta_updates.load();
        results.bytes_full_row = metrics.bytes_full_row.load();
        results.bytes_delta = metrics.bytes_delta.load();
        results.bytes_saved = metrics.bytes_saved.load();
#endif
        
        return results;
    }

protected:
    abstract_db *const db;
    str_arena arena;
    std::string txn_obj_buf;
    abstract_ordered_index *warehouse_table;
    abstract_ordered_index *district_table;
    abstract_ordered_index *customer_table;
    
    inline void *txn_buf() { return (void *)txn_obj_buf.data(); }
    inline std::string &str() { return *arena.next(); }
};

void print_results(const BenchmarkResults& results, int num_payments) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║              BASELINE vs COLUMN-DELTA COMPARISON                           ║\n");
    printf("║                      (%d Payment Transactions)                             ║\n", num_payments);
    printf("╠════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║                                                                            ║\n");
    printf("║  THROUGHPUT:                                                               ║\n");
    printf("║    Measured:                %12.0f payments/sec                       ║\n", results.throughput);
    printf("║                                                                            ║\n");
    printf("╠════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  LATENCY (per payment):                                                    ║\n");
    printf("║    Average:                 %12.1f us                                  ║\n", results.avg_latency_us);
    printf("║    Min:                     %12.1f us                                  ║\n", results.min_latency_us);
    printf("║    Max:                     %12.1f us                                  ║\n", results.max_latency_us);
    printf("║                                                                            ║\n");
    printf("╠════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  STORAGE COMPARISON:                                                       ║\n");
    printf("║                             Baseline        Column-Delta                   ║\n");
    double baseline_per_update = results.total_updates > 0 ? 
        (double)results.bytes_full_row / results.total_updates : 0;
    double delta_per_update = results.delta_updates > 0 ?
        (double)results.bytes_delta / results.delta_updates : 0;
    printf("║    Per update:              %8.1f bytes  %8.1f bytes                 ║\n",
           baseline_per_update, delta_per_update);
    printf("║    Total:                   %8lu bytes  %8lu bytes                 ║\n",
           results.bytes_full_row, results.bytes_delta);
    printf("║                                                                            ║\n");
    printf("║  SAVINGS:                                                                  ║\n");
    printf("║    Bytes saved:             %12lu (%.1f%% reduction)                  ║\n",
           results.bytes_saved,
           results.bytes_full_row > 0 ? 100.0 * results.bytes_saved / results.bytes_full_row : 0);
    printf("║                                                                            ║\n");
    printf("╠════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  UPDATE BREAKDOWN:                                                         ║\n");
    printf("║    Total updates:           %12lu                                     ║\n", results.total_updates);
    printf("║    Delta updates:           %12lu (%.1f%%)                            ║\n", 
           results.delta_updates,
           results.total_updates > 0 ? 100.0 * results.delta_updates / results.total_updates : 0);
    printf("╚════════════════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
}

void run_evaluation(abstract_db *db, int num_payments) {
    printf("\n=== Running Column-Delta MVCC Evaluation ===\n");
    
    auto worker = new ColumnDeltaEvalWorker(db);
    worker->initialize();
    worker->setup_initial_data("eval");
    
    printf("\n--- Running %d Payment simulations with Column-Delta enabled ---\n", num_payments);
    BenchmarkResults results = worker->run_payment_simulation(num_payments, true);
    printf("Completed: %.0f payments/sec, avg latency: %.1f us\n", 
           results.throughput, results.avg_latency_us);
    
    // Print results
    print_results(results, num_payments);
}

int main(int argc, char* argv[]) {
    int num_payments = 1000;
    if (argc > 1) {
        num_payments = atoi(argv[1]);
    }
    
    abstract_db *db = new mbta_wrapper;
    db->init();
    printf("=== Column-Delta MVCC A/B Evaluation ===\n");
    printf("Number of payments: %d\n", num_payments);
    
    auto config = new transport::Configuration(
        get_current_absolute_path() + "../src/mako/config/local-shards2-warehouses1.yml"
    );
    BenchmarkConfig::getInstance().setConfig(config);
    
    run_evaluation(db, num_payments);
    
    printf("\n" GREEN "Evaluation completed!" RESET "\n");
    return 0;
}
