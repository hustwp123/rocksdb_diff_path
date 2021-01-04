//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#include <tries/vbyte_string_pool.hpp>

#include "include/rocksdb/filter_policy.h"
#include "port/port.h"
#include "util/coding.h"
#include "utilities/ot/tries/compacted_trie_builder.hpp"
#include "utilities/ot/tries/path_decomposed_trie.hpp"

#include <algorithm>

namespace rocksdb {
// An implementation of filter policy
namespace {
//template <bool Lexicographic = false>
//class OtLexPdtFilterPolicy : public FilterPolicy {
// public:
//  explicit OtLexPdtFilterPolicy(bool use_block_based_builder)
//      : use_block_based_builder_(use_block_based_builder) {}

//  ~OtLexPdtFilterPolicy() override {}

//  const char* Name() const override { return "rocksdb.OtLexPdtFilter"; }

  // xp
  // please ensure keys are sorted
//  void CreateFilter(const Slice* keys, int n, std::string* dst) const override {
//    fprintf(stdout, "in OtLexPdt CreateFilter() do nothing\n");
//  }


//  bool KeyMayMatch(const Slice& key, const Slice& bloom_filter) const override {
//    fprintf(stdout, "in OtLexPdt KeyMayMatch() always return 1\n");
//    return true;
//  }

//  FilterBitsBuilder* GetFilterBitsBuilder() const { return nullptr; }

//  FilterBitsReader* GetFilterBitsReader(const Slice& /*contents*/) const {
//    return nullptr;
//  }

// private:
//  const bool use_block_based_builder_;
//};
}  // namespace

//const FilterPolicy* NewOtLexPdtFilterPolicy(bool use_block_based_builer) {
//  return new OtLexPdtFilterPolicy<true>(use_block_based_builer);
//}

}  // namespace rocksdb