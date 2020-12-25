#pragma once

#include <set>
#include <boost/test/test_tools.hpp>
#include "succinct/util.hpp"
#include "bit_strings.hpp"

namespace rocksdb {

template <typename Trie>
inline void test_index_binary(bool check_negative = false) {
  rocksdb::succinct::util::mmap_lines strings_lines("propernames");
  std::vector<std::string> strings(strings_lines.begin(), strings_lines.end());
  std::set<std::string> string_set(strings.begin(), strings.end());

  rocksdb::succinct::tries::stl_string_adaptor adaptor;
  Trie trie(strings, adaptor);

  for (size_t i = 0; i < strings.size(); ++i) {
    BOOST_REQUIRE_EQUAL(i, trie.index(strings[i], adaptor));
  }

  if (check_negative) {
    for (size_t i = 0; i < strings.size(); ++i) {
      std::string s = strings[i] + "X";
      if (string_set.count(s) == 0) {
        BOOST_REQUIRE_EQUAL(-1, trie.index(s, adaptor));
      }
      s = strings[i].substr(0, std::max(strings[i].size() - 1, size_t(0)));
      if (string_set.count(s) == 0) {
        BOOST_REQUIRE_EQUAL(-1, trie.index(s, adaptor));
      }
    }
  }
}

template <typename Trie>
inline void test_trie_roundtrip() {
  rocksdb::succinct::util::mmap_lines strings_lines("propernames");
//  rocksdb::succinct::util::mmap_lines strings_lines("000026.8char.csv");
  std::vector<std::string> strings(strings_lines.begin(), strings_lines.end());

  Trie trie(strings);

  for (size_t i = 0; i < strings.size(); ++i) {
    size_t idx = trie.index(strings[i]);
    BOOST_REQUIRE(idx != (size_t) -1);
    BOOST_REQUIRE_EQUAL(strings[i], trie[idx]);
  }


}
}