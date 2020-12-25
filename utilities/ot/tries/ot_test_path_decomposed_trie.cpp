//#define BOOST_TEST_MODULE centroid_trie
#include <gtest/gtest.h>
#include "succinct/test_common.hpp"
#include "test_binary_trie_common.hpp"

#include "vbyte_string_pool.hpp"
#include "compressed_string_pool.hpp"
#include "path_decomposed_trie.hpp"
#include "rs_bit_vector.hpp"

//using namespace rocksdb;

//BOOST_AUTO_TEST_CASE(path_decomposed_trie)
//{
    // Centroid trie only roundtrips
//    test_trie_roundtrip<rocksdb::succinct::tries::path_decomposed_trie<rocksdb::succinct::tries::vbyte_string_pool> >();
//    test_trie_roundtrip<rocksdb::succinct::tries::path_decomposed_trie<rocksdb::succinct::tries::compressed_string_pool> >();

    // Lexicographic one also has monotone indexes
//    test_index_binary<rocksdb::succinct::tries::path_decomposed_trie<rocksdb::succinct::tries::vbyte_string_pool, true> >();
//    test_trie_roundtrip<rocksdb::succinct::tries::path_decomposed_trie<rocksdb::succinct::tries::vbyte_string_pool, true> >();
//}


TEST(OT_PDT, OT_PATH_DECOMPOSED_TRIE) {
  // Centroid trie only roundtrips
    rocksdb::test_trie_roundtrip<rocksdb::succinct::tries::path_decomposed_trie<rocksdb::succinct::tries::vbyte_string_pool> >();
    rocksdb::test_trie_roundtrip<rocksdb::succinct::tries::path_decomposed_trie<rocksdb::succinct::tries::compressed_string_pool> >();

  // Lexicographic one also has monotone indexes
    rocksdb::test_index_binary<rocksdb::succinct::tries::path_decomposed_trie<rocksdb::succinct::tries::vbyte_string_pool, true> >();
    rocksdb::test_trie_roundtrip<rocksdb::succinct::tries::path_decomposed_trie<rocksdb::succinct::tries::vbyte_string_pool, true> >();
}

GTEST_API_ int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
