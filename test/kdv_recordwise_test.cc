#include "../src/mako/kdv_format.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

using namespace mako::kdv;

static std::string build_simple_log() {
    std::string result;

    uint32_t commit_ts = 123456;
    uint16_t kv_count = 2;

    result.append(reinterpret_cast<const char*>(&commit_ts), sizeof(uint32_t));
    result.append(reinterpret_cast<const char*>(&kv_count), sizeof(uint16_t));

    size_t len_of_kv_offset = result.size();
    uint32_t len_of_kv = 0;
    result.append(reinterpret_cast<const char*>(&len_of_kv), sizeof(uint32_t));

    size_t kv_region_start = result.size();

    // Record 1
    {
        std::string key = "user:1";
        std::string value = "value_one";
        uint16_t table_id = 42;

        uint16_t key_len = static_cast<uint16_t>(key.size());
        uint16_t val_len = static_cast<uint16_t>(value.size());

        result.append(reinterpret_cast<const char*>(&key_len), sizeof(uint16_t));
        result.append(key.data(), key.size());

        result.append(reinterpret_cast<const char*>(&val_len), sizeof(uint16_t));
        result.append(value.data(), value.size());

        result.append(reinterpret_cast<const char*>(&table_id), sizeof(uint16_t));
    }

    // Record 2
    {
        std::string key = "user:2";
        std::string value = "value_two";
        uint16_t table_id = 99;

        uint16_t key_len = static_cast<uint16_t>(key.size());
        uint16_t val_len = static_cast<uint16_t>(value.size());

        result.append(reinterpret_cast<const char*>(&key_len), sizeof(uint16_t));
        result.append(key.data(), key.size());

        result.append(reinterpret_cast<const char*>(&val_len), sizeof(uint16_t));
        result.append(value.data(), value.size());

        result.append(reinterpret_cast<const char*>(&table_id), sizeof(uint16_t));
    }

    size_t kv_region_end = result.size();
    len_of_kv = static_cast<uint32_t>(kv_region_end - kv_region_start);
    std::memcpy(&result[len_of_kv_offset], &len_of_kv, sizeof(uint32_t));

    uint32_t trailer_ts = 789012;
    uint32_t trailer_st_time = 345678;
    result.append(reinterpret_cast<const char*>(&trailer_ts), sizeof(uint32_t));
    result.append(reinterpret_cast<const char*>(&trailer_st_time), sizeof(uint32_t));

    return result;
}

int main() {
    std::cout << "=== KDV Recordwise Roundtrip Test ===" << std::endl;

    KDVStoreState::getInstance().reset();

    std::string log = build_simple_log();
    uint32_t shard_id = 0;
    uint32_t partition_id = 0;
    uint64_t seq = 1;

    std::string encoded = kdv_encode_log_recordwise(shard_id,
                                                    partition_id,
                                                    seq,
                                                    log.data(),
                                                    log.size());

    std::string decoded = kdv_decode_log(shard_id,
                                         partition_id,
                                         seq,
                                         encoded.data(),
                                         encoded.size());

    if (decoded.size() != log.size()) {
        std::cerr << "Size mismatch: original=" << log.size()
                  << ", decoded=" << decoded.size() << std::endl;
        return 1;
    }
    if (decoded != log) {
        std::cerr << "Content mismatch between original and decoded log" << std::endl;
        return 1;
    }

    std::cout << "PASS: Recordwise encode/decode roundtrip matches original log" << std::endl;
    return 0;
}

