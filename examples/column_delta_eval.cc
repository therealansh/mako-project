//
// Column-Delta MVCC Evaluation Test
// Simulates Payment transaction pattern to collect metrics
//

#include <iostream>
#include <chrono>
#include <thread>
#include <mako.hh>
#include <examples/common.h>
#include "../src/mako/benchmarks/tpcc.h"
#include "../src/mako/benchmarks/tpcc_column_delta.h"

using namespace std;

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

    void setup_initial_data() {
        printf("\n--- Setting up initial data ---\n");
        
        // Create warehouse table and insert initial data
        warehouse_table = db->open_index("warehouse_eval");
        district_table = db->open_index("district_eval");
        customer_table = db->open_index("customer_eval");
        
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
        
        VERIFY_PASS("Initial data setup");
    }

    void run_payment_simulation(int num_payments) {
        printf("\n--- Running %d Payment simulations ---\n", num_payments);
        
#if MAKO_ENABLE_COLUMN_DELTAS
        mako::getColumnDeltaMetrics().reset();
#endif
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < num_payments; i++) {
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
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        printf("Completed %d payments in %ld ms\n", num_payments, duration.count());
        printf("Throughput: %.2f payments/sec\n", 
               num_payments * 1000.0 / duration.count());
        
        VERIFY_PASS("Payment simulation");
    }

    void print_metrics() {
#if MAKO_ENABLE_COLUMN_DELTAS
        printf("\n");
        mako::getColumnDeltaMetrics().print();
#else
        printf("\n--- Column-delta metrics disabled (MAKO_ENABLE_COLUMN_DELTAS=0) ---\n");
#endif
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

void run_evaluation(abstract_db *db) {
    auto worker = new ColumnDeltaEvalWorker(db);
    worker->initialize();
    worker->setup_initial_data();
    worker->run_payment_simulation(1000);
    worker->print_metrics();
    delete worker;
}

int main() {
    abstract_db *db = new mbta_wrapper;
    db->init();
    printf("=== Column-Delta MVCC Evaluation ===\n");
    
    auto config = new transport::Configuration(
        get_current_absolute_path() + "../src/mako/config/local-shards2-warehouses1.yml"
    );
    BenchmarkConfig::getInstance().setConfig(config);
    
    run_evaluation(db);
    
    delete db;
    
    printf("\n" GREEN "Evaluation completed!" RESET "\n");
    return 0;
}
