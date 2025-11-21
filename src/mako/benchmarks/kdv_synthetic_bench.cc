#include "kdv_format.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

using namespace mako::kdv;

int main(int argc, char** argv) {
    size_t payload_size = 1024;
    size_t num_updates = 1000;
    size_t delta_region = 16;

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
    std::cout << "Bandwidth reduction:   " << savings_pct << "%"<< std::endl;

    return 0;
}

