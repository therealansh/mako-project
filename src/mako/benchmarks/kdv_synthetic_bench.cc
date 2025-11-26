/*
 * KDV synthetic microbenchmark.
 *
 * This is a library-level benchmark that exercises kdv_encode_log /
 * kdv_decode_log on a single hot key with controlled record size and
 * update size, to mimic YCSB-style small/medium/large in-place updates.
 *
 * Usage:
 *   ./kdv_synthetic_bench [payload_size] [delta_region] [num_updates]
 *
 * Defaults (if no args are provided):
 *   payload_size = 1024 bytes
 *   delta_region = 16 bytes
 *   num_updates  = 1000
 *
 * The program prints a human-readable summary plus a single CSV-style
 * line starting with "KDV_SYNTHETIC_SUMMARY" that can be parsed by
 * scripts for table generation.
 */

#include "kdv_format.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <cstdlib>

using namespace mako::kdv;

int main(int argc, char** argv) {
    size_t payload_size = 1024;
    size_t num_updates = 1000;
    size_t delta_region = 16;

    if (argc >= 2) {
        payload_size = static_cast<size_t>(std::strtoull(argv[1], nullptr, 10));
    }
    if (argc >= 3) {
        delta_region = static_cast<size_t>(std::strtoull(argv[2], nullptr, 10));
    }
    if (argc >= 4) {
        num_updates = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
    }

    if (payload_size == 0 || delta_region == 0 || num_updates == 0) {
        std::cerr << "Invalid arguments. Usage: " << argv[0]
                  << " [payload_size] [delta_region] [num_updates]" << std::endl;
        return 1;
    }

    std::string base(payload_size, 'A');

    KDVStoreState::getInstance().reset();

    uint32_t shard_id = 0;
    uint32_t partition_id = 0;
    uint64_t key_hash = 12345;

    uint64_t total_original = 0;
    uint64_t total_encoded = 0;

    std::string last_value = base;

    for (size_t i = 0; i < num_updates; ++i) {
        std::string value = last_value;

        size_t start = (payload_size / 2);
        for (size_t j = 0; j < delta_region && start + j < payload_size; ++j) {
            value[start + j] = static_cast<char>('B' + (i % 10));
        }

        uint64_t seq = static_cast<uint64_t>(i + 1);
        std::string encoded = kdv_encode_log(shard_id,
                                             partition_id,
                                             seq,
                                             key_hash,
                                             value.data(),
                                             value.size());

        std::string decoded = kdv_decode_log(shard_id,
                                             partition_id,
                                             seq,
                                             encoded.data(),
                                             encoded.size());
        if (decoded != value) {
            std::cerr << "Decode mismatch at seq " << seq << std::endl;
            return 1;
        }

        total_original += value.size();
        total_encoded += encoded.size();

        last_value = value;
    }

    double compression_ratio = total_original > 0
        ? static_cast<double>(total_encoded) / static_cast<double>(total_original)
        : 1.0;
    double savings_pct = (1.0 - compression_ratio) * 100.0;

    std::cout << "=== KDV Synthetic Benchmark ===" << std::endl;
    std::cout << "Payload size:          " << payload_size << " bytes" << std::endl;
    std::cout << "Updates:               " << num_updates << std::endl;
    std::cout << "Delta region:          " << delta_region << " bytes" << std::endl;
    std::cout << "Total original bytes:  " << total_original << std::endl;
    std::cout << "Total encoded bytes:   " << total_encoded << std::endl;
    std::cout << "Compression ratio:     " << compression_ratio << std::endl;
    std::cout << "Bandwidth reduction:   " << savings_pct << "%" << std::endl;

    // Machine-readable summary line for scripts.
    std::cout << "KDV_SYNTHETIC_SUMMARY"
              << ",payload_size=" << payload_size
              << ",delta_region=" << delta_region
              << ",num_updates=" << num_updates
              << ",compression_ratio=" << compression_ratio
              << ",bandwidth_reduction_pct=" << savings_pct
              << ",total_original_bytes=" << total_original
              << ",total_encoded_bytes=" << total_encoded
              << std::endl;

    return 0;
}
