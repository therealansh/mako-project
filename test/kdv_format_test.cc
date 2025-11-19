#include "../src/mako/kdv_format.h"
#include <cassert>
#include <iostream>
#include <random>
#include <vector>
#include <cstring>

using namespace mako::kdv;

// Test utilities
void assert_equal(const std::string& a, const std::string& b, const char* msg) {
    if (a != b) {
        std::cerr << "FAIL: " << msg << std::endl;
        std::cerr << "  Expected size: " << b.size() << ", Got size: " << a.size() << std::endl;
        exit(1);
    }
}

void assert_true(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << std::endl;
        exit(1);
    }
}

// Generate random string
std::string random_string(size_t length, std::mt19937& rng) {
    std::uniform_int_distribution<int> dist(0, 255);
    std::string result(length, '\0');
    for (size_t i = 0; i < length; i++) {
        result[i] = static_cast<char>(dist(rng));
    }
    return result;
}

// Test 1: Basic encode/decode identity
void test_encode_decode_identity() {
    std::cout << "Test 1: Encode/Decode Identity..." << std::endl;
    
    KDVStoreState::getInstance().reset();
    
    std::string original = "Hello, World! This is a test log entry.";
    
    // Encode
    std::string encoded = kdv_encode_log(0, 0, 1, 0, original.data(), original.size());
    
    // Decode
    std::string decoded = kdv_decode_log(0, 0, 1, encoded.data(), encoded.size());
    
    assert_equal(decoded, original, "Decoded value should match original");
    
    std::cout << "  PASS: Basic encode/decode identity" << std::endl;
}

// Test 2: Small update (delta compression)
void test_small_update() {
    std::cout << "Test 2: Small Update (Delta Compression)..." << std::endl;
    
    KDVStoreState::getInstance().reset();
    
    // Create base with lots of 'A's
    std::string base(1024, 'A');
    
    // Create value with small change in middle
    std::string value = base;
    for (int i = 500; i < 516; i++) {
        value[i] = 'B';
    }
    
    // Use a consistent key_hash for both encodes (simulating updates to the same logical key)
    uint64_t key_hash = 12345;
    
    // Encode base (should be BASE mode)
    std::string encoded1 = kdv_encode_log(0, 0, 1, key_hash, base.data(), base.size());
    std::string decoded1 = kdv_decode_log(0, 0, 1, encoded1.data(), encoded1.size());
    assert_equal(decoded1, base, "First decode should match base");
    
    // Encode value (should be DELTA mode) - use same key_hash
    std::string encoded2 = kdv_encode_log(0, 0, 2, key_hash, value.data(), value.size());
    std::string decoded2 = kdv_decode_log(0, 0, 2, encoded2.data(), encoded2.size());
    assert_equal(decoded2, value, "Second decode should match value");
    
    // Check compression ratio
    size_t delta_size = encoded2.size();
    size_t original_size = value.size();
    double compression_ratio = (double)delta_size / original_size;
    
    std::cout << "  Original size: " << original_size << " bytes" << std::endl;
    std::cout << "  Delta size: " << delta_size << " bytes" << std::endl;
    std::cout << "  Compression ratio: " << compression_ratio << std::endl;
    
    assert_true(compression_ratio < 0.1, "Delta should be much smaller than original");
    
    std::cout << "  PASS: Small update compression" << std::endl;
}

// Test 3: Large update (should write base)
void test_large_update() {
    std::cout << "Test 3: Large Update (Should Write Base)..." << std::endl;
    
    KDVStoreState::getInstance().reset();
    
    std::string base(1024, 'A');
    std::string value(1024, 'B');  // Completely different
    
    // Use a consistent key_hash for both encodes
    uint64_t key_hash = 67890;
    
    // Encode base
    std::string encoded1 = kdv_encode_log(0, 0, 1, key_hash, base.data(), base.size());
    std::string decoded1 = kdv_decode_log(0, 0, 1, encoded1.data(), encoded1.size());
    assert_equal(decoded1, base, "First decode should match base");
    
    // Encode value (should be BASE mode due to large delta)
    std::string encoded2 = kdv_encode_log(0, 0, 2, key_hash, value.data(), value.size());
    std::string decoded2 = kdv_decode_log(0, 0, 2, encoded2.data(), encoded2.size());
    assert_equal(decoded2, value, "Second decode should match value");
    
    // Check that it's stored as base (size should be close to original + header)
    size_t expected_size = sizeof(KDVHeader) + value.size();
    assert_true(encoded2.size() == expected_size, "Large update should be stored as base");
    
    std::cout << "  PASS: Large update stored as base" << std::endl;
}

// Test 4: Chain behavior
void test_chain_behavior() {
    std::cout << "Test 4: Chain Behavior..." << std::endl;
    
    KDVStoreState::getInstance().reset();
    KDVStoreState::getInstance().setMaxChainLen(8);
    
    std::string base(1024, 'A');
    
    // Use a consistent key_hash for all encodes
    uint64_t key_hash = 11111;
    
    // Encode base
    std::string encoded = kdv_encode_log(0, 0, 1, key_hash, base.data(), base.size());
    std::string decoded = kdv_decode_log(0, 0, 1, encoded.data(), encoded.size());
    assert_equal(decoded, base, "Base should decode correctly");
    
    // Encode 20 small updates
    for (int i = 0; i < 20; i++) {
        std::string value = base;
        // Change one byte
        value[100 + i] = 'B';
        
        encoded = kdv_encode_log(0, 0, 2 + i, key_hash, value.data(), value.size());
        decoded = kdv_decode_log(0, 0, 2 + i, encoded.data(), encoded.size());
        assert_equal(decoded, value, "Each update should decode correctly");
        
        // Check header
        const KDVHeader* header = reinterpret_cast<const KDVHeader*>(encoded.data());
        
        if (i < 8) {
            // Should be delta
            assert_true(header->mode == static_cast<uint8_t>(KDVEncodeMode::DELTA),
                       "First 8 updates should be deltas");
        } else if (i == 8) {
            // Should force new base due to chain length
            assert_true(header->mode == static_cast<uint8_t>(KDVEncodeMode::BASE),
                       "9th update should be new base due to chain length");
        }
    }
    
    std::cout << "  PASS: Chain length policy working" << std::endl;
}

// Test 5: Multiple partitions
void test_multiple_partitions() {
    std::cout << "Test 5: Multiple Partitions..." << std::endl;
    
    KDVStoreState::getInstance().reset();
    
    std::string data0 = "Partition 0 data";
    std::string data1 = "Partition 1 data";
    std::string data2 = "Partition 2 data";
    
    // Encode for different partitions
    std::string enc0 = kdv_encode_log(0, 0, 1, 0, data0.data(), data0.size());
    std::string enc1 = kdv_encode_log(0, 1, 1, 0, data1.data(), data1.size());
    std::string enc2 = kdv_encode_log(0, 2, 1, 0, data2.data(), data2.size());
    
    // Decode
    std::string dec0 = kdv_decode_log(0, 0, 1, enc0.data(), enc0.size());
    std::string dec1 = kdv_decode_log(0, 1, 1, enc1.data(), enc1.size());
    std::string dec2 = kdv_decode_log(0, 2, 1, enc2.data(), enc2.size());
    
    assert_equal(dec0, data0, "Partition 0 should decode correctly");
    assert_equal(dec1, data1, "Partition 1 should decode correctly");
    assert_equal(dec2, data2, "Partition 2 should decode correctly");
    
    // Update partition 0 with delta
    std::string data0_v2 = "Partition 0 data updated";
    std::string enc0_v2 = kdv_encode_log(0, 0, 2, 0, data0_v2.data(), data0_v2.size());
    std::string dec0_v2 = kdv_decode_log(0, 0, 2, enc0_v2.data(), enc0_v2.size());
    assert_equal(dec0_v2, data0_v2, "Partition 0 update should decode correctly");
    
    // Verify partition 1 is unaffected
    std::string data1_v2 = "Partition 1 data also updated";
    std::string enc1_v2 = kdv_encode_log(0, 1, 2, 0, data1_v2.data(), data1_v2.size());
    std::string dec1_v2 = kdv_decode_log(0, 1, 2, enc1_v2.data(), enc1_v2.size());
    assert_equal(dec1_v2, data1_v2, "Partition 1 update should decode correctly");
    
    std::cout << "  PASS: Multiple partitions work independently" << std::endl;
}

// Test 6: Random data stress test
void test_random_data() {
    std::cout << "Test 6: Random Data Stress Test..." << std::endl;
    
    KDVStoreState::getInstance().reset();
    
    std::mt19937 rng(42);  // Fixed seed for reproducibility
    
    // Use a consistent key_hash for all encodes
    uint64_t key_hash = 99999;
    
    // Generate random base
    std::string base = random_string(2048, rng);
    
    // Encode base
    std::string encoded = kdv_encode_log(0, 0, 1, key_hash, base.data(), base.size());
    std::string decoded = kdv_decode_log(0, 0, 1, encoded.data(), encoded.size());
    assert_equal(decoded, base, "Random base should decode correctly");
    
    // Generate 50 random updates
    for (int i = 0; i < 50; i++) {
        // Randomly modify 10-100 bytes
        std::uniform_int_distribution<int> count_dist(10, 100);
        int num_changes = count_dist(rng);
        
        std::string value = base;
        std::uniform_int_distribution<int> pos_dist(0, value.size() - 1);
        std::uniform_int_distribution<int> byte_dist(0, 255);
        
        for (int j = 0; j < num_changes; j++) {
            int pos = pos_dist(rng);
            value[pos] = static_cast<char>(byte_dist(rng));
        }
        
        // Encode and decode
        encoded = kdv_encode_log(0, 0, 2 + i, key_hash, value.data(), value.size());
        decoded = kdv_decode_log(0, 0, 2 + i, encoded.data(), encoded.size());
        
        if (decoded != value) {
            std::cerr << "FAIL: Random update " << i << " decode mismatch" << std::endl;
            std::cerr << "  Original size: " << value.size() << std::endl;
            std::cerr << "  Decoded size: " << decoded.size() << std::endl;
            exit(1);
        }
    }
    
    std::cout << "  PASS: Random data stress test" << std::endl;
}

// Test 7: Edge cases
void test_edge_cases() {
    std::cout << "Test 7: Edge Cases..." << std::endl;
    
    KDVStoreState::getInstance().reset();
    
    // Empty string
    std::string empty = "";
    std::string enc_empty = kdv_encode_log(0, 0, 1, 0, empty.data(), empty.size());
    std::string dec_empty = kdv_decode_log(0, 0, 1, enc_empty.data(), enc_empty.size());
    assert_equal(dec_empty, empty, "Empty string should encode/decode");
    
    // Single byte
    std::string single = "X";
    std::string enc_single = kdv_encode_log(0, 1, 1, 0, single.data(), single.size());
    std::string dec_single = kdv_decode_log(0, 1, 1, enc_single.data(), enc_single.size());
    assert_equal(dec_single, single, "Single byte should encode/decode");
    
    // Very large string (10KB)
    std::string large(10240, 'Z');
    std::string enc_large = kdv_encode_log(0, 2, 1, 0, large.data(), large.size());
    std::string dec_large = kdv_decode_log(0, 2, 1, enc_large.data(), enc_large.size());
    assert_equal(dec_large, large, "Large string should encode/decode");
    
    std::cout << "  PASS: Edge cases handled correctly" << std::endl;
}

// Test 8: Delta computation correctness
void test_delta_computation() {
    std::cout << "Test 8: Delta Computation Correctness..." << std::endl;
    
    std::string base = "AAAAAABBBBBBCCCCCC";
    std::string value = "AAAAAA123456CCCCCC";
    
    std::string delta = compute_delta(base, value);
    std::string reconstructed = apply_delta(base, delta);
    
    assert_equal(reconstructed, value, "Delta application should reconstruct value");
    
    // Check delta structure
    const DeltaBlock* block = reinterpret_cast<const DeltaBlock*>(delta.data());
    assert_true(block->prefix_len == 6, "Prefix length should be 6");
    assert_true(block->suffix_len == 6, "Suffix length should be 6");
    assert_true(block->middle_len == 6, "Middle length should be 6");
    
    std::cout << "  PASS: Delta computation correct" << std::endl;
}

int main() {
    std::cout << "=== KDV Format Unit Tests ===" << std::endl << std::endl;
    
    try {
        test_encode_decode_identity();
        test_small_update();
        test_large_update();
        test_chain_behavior();
        test_multiple_partitions();
        test_random_data();
        test_edge_cases();
        test_delta_computation();
        
        std::cout << std::endl << "=== ALL TESTS PASSED ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
}
