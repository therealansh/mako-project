#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <cstdint>
#include <dirent.h>
#include <sys/stat.h>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/iterator.h>
#include <rocksdb/write_batch.h>

#include "rocksdb_persistence.h"
#include "kdv_format.h"

using namespace mako;

static std::string findRocksDBPath() {
    DIR* dir = opendir("/tmp");
    if (!dir) {
        return "";
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.find("mako_rocksdb_shard0_leader_pid") == 0 &&
            name.find("_partition0") != std::string::npos) {
            std::string full_path = "/tmp/" + name;
            size_t pos = full_path.rfind("_partition0");
            if (pos != std::string::npos) {
                closedir(dir);
                return full_path.substr(0, pos);
            }
        }
    }
    closedir(dir);
    return "";
}

struct CompactionStats {
    uint64_t old_bytes{0};
    uint64_t new_bytes{0};
    size_t entries{0};
};

static bool compactPartition(const std::string& base_path,
                             uint32_t shard_id,
                             uint32_t partition_id,
                             bool dry_run,
                             CompactionStats& stats) {
    std::string partition_path = base_path + "_partition" + std::to_string(partition_id);

    rocksdb::Options options;
    options.create_if_missing = false;

    rocksdb::DB* db_raw = nullptr;
    rocksdb::Status status = rocksdb::DB::Open(options, partition_path, &db_raw);
    if (!status.ok()) {
        std::cerr << "[KDV Compaction] Failed to open RocksDB partition " << partition_id
                  << " at " << partition_path << ": " << status.ToString() << std::endl;
        return false;
    }

    std::unique_ptr<rocksdb::DB> db(db_raw);

    std::vector<std::string> keys;
    std::vector<std::string> raw_values;
    std::vector<size_t> old_sizes;

    auto& kdv_store = mako::kdv::KDVStoreState::getInstance();
    kdv_store.reset();

    rocksdb::ReadOptions read_options;
    std::unique_ptr<rocksdb::Iterator> iter(db->NewIterator(read_options));

    uint64_t seq_num = 0;

    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
        std::string key = iter->key().ToString();
        if (key == "meta") {
            continue;
        }

        std::string value = iter->value().ToString();
        size_t encoded_size = value.size();

        std::string raw;
        bool looks_kdv = false;

        if (encoded_size >= sizeof(mako::kdv::KDVHeader)) {
            auto* header = reinterpret_cast<const mako::kdv::KDVHeader*>(value.data());
            if (header->magic == mako::kdv::KDV_MAGIC) {
                looks_kdv = true;
            }
        }

        if (looks_kdv) {
            raw = mako::kdv::kdv_decode_log(shard_id, partition_id, seq_num,
                                            value.data(), value.size());
            if (raw.empty()) {
                std::cerr << "[KDV Compaction] Decode failed for shard " << shard_id
                          << ", partition " << partition_id << ", seq " << seq_num
                          << " (key=" << key << "), keeping original value" << std::endl;
                raw = value;
            }
        } else {
            raw = value;
        }

        keys.emplace_back(std::move(key));
        raw_values.emplace_back(std::move(raw));
        old_sizes.emplace_back(encoded_size);
        seq_num++;
    }

    if (keys.empty()) {
        return true;
    }

    kdv_store.reset();

    rocksdb::WriteBatch batch;
    rocksdb::WriteOptions write_options;
    write_options.sync = false;

    seq_num = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
        const std::string& raw = raw_values[i];

        // Use per-record KDV encoding so offline compaction matches the
        // online encoding used by RocksDB persistence.
        std::string encoded = mako::kdv::kdv_encode_log_recordwise(shard_id,
                                                                   partition_id,
                                                                   seq_num,
                                                                   raw.data(),
                                                                   raw.size());

        stats.old_bytes += static_cast<uint64_t>(old_sizes[i]);
        stats.new_bytes += static_cast<uint64_t>(encoded.size());
        stats.entries++;

        if (!dry_run) {
            batch.Put(keys[i], encoded);
        }

        seq_num++;
    }

    if (!dry_run) {
        status = db->Write(write_options, &batch);
        if (!status.ok()) {
            std::cerr << "[KDV Compaction] Write failed for partition " << partition_id
                      << ": " << status.ToString() << std::endl;
            return false;
        }
    }

    return true;
}

static void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [--db-path <path>] [--dry-run]" << std::endl;
    std::cout << "  --db-path <path>  Base RocksDB path (e.g., /tmp/mako_rocksdb_shard0_leader_pidXYZ)" << std::endl;
    std::cout << "  --dry-run         Do not write changes, only report potential savings" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string db_path;
    bool dry_run = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--db-path" && i + 1 < argc) {
            db_path = argv[++i];
        } else if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            printUsage(argv[0]);
            return 1;
        }
    }

    if (db_path.empty()) {
        db_path = findRocksDBPath();
        if (db_path.empty()) {
            std::cerr << "[KDV Compaction] No RocksDB found under /tmp/mako_rocksdb_shard0_leader_*" << std::endl;
            return 1;
        }
    }

    uint32_t epoch = 0;
    uint32_t shard_id = 0;
    uint32_t num_shards = 0;
    size_t num_partitions = 0;
    size_t num_workers = 0;
    int64_t timestamp = 0;

    if (!RocksDBPersistence::parseMetadata(db_path,
                                           epoch,
                                           shard_id,
                                           num_shards,
                                           num_partitions,
                                           num_workers,
                                           timestamp)) {
        std::cerr << "[KDV Compaction] Failed to parse RocksDB metadata at " << db_path << std::endl;
        return 1;
    }

    std::cout << "=== KDV RocksDB Compaction Tool ===" << std::endl;
    std::cout << "Base path:      " << db_path << std::endl;
    std::cout << "Shard id:       " << shard_id << std::endl;
    std::cout << "Shards:         " << num_shards << std::endl;
    std::cout << "Partitions:     " << num_partitions << std::endl;
    std::cout << "Workers:        " << num_workers << std::endl;
    std::cout << "Dry run:        " << (dry_run ? "yes" : "no") << std::endl;
    std::cout << std::endl;

    CompactionStats total_stats;

    auto start_time = std::chrono::steady_clock::now();

    for (size_t p = 0; p < num_partitions; ++p) {
        CompactionStats part_stats;
        bool ok = compactPartition(db_path, shard_id, static_cast<uint32_t>(p), dry_run, part_stats);
        if (!ok) {
            std::cerr << "[KDV Compaction] Partition " << p << " failed, aborting" << std::endl;
            return 1;
        }

        if (part_stats.entries > 0) {
            double ratio = part_stats.old_bytes > 0
                               ? 100.0 * (1.0 - static_cast<double>(part_stats.new_bytes) /
                                                  static_cast<double>(part_stats.old_bytes))
                               : 0.0;
            std::cout << "Partition " << p << ": entries=" << part_stats.entries
                      << ", old_bytes=" << part_stats.old_bytes
                      << ", new_bytes=" << part_stats.new_bytes
                      << ", savings=" << ratio << "%";
            if (dry_run) {
                std::cout << " (dry run)";
            }
            std::cout << std::endl;
        }

        total_stats.old_bytes += part_stats.old_bytes;
        total_stats.new_bytes += part_stats.new_bytes;
        total_stats.entries += part_stats.entries;
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::cout << std::endl;
    std::cout << "=== KDV Compaction Summary ===" << std::endl;
    std::cout << "Total entries:  " << total_stats.entries << std::endl;
    std::cout << "Total old bytes:" << total_stats.old_bytes << std::endl;
    std::cout << "Total new bytes:" << total_stats.new_bytes << std::endl;
    if (total_stats.old_bytes > 0) {
        double ratio = 100.0 * (1.0 - static_cast<double>(total_stats.new_bytes) /
                                          static_cast<double>(total_stats.old_bytes));
        std::cout << "Total savings: " << ratio << "%" << std::endl;
    }
    std::cout << "Elapsed time:   " << duration_ms << " ms" << std::endl;

    if (dry_run) {
        std::cout << "(Dry run: no changes were written)" << std::endl;
    }

    return 0;
}
