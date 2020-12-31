//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "rocksdb/filter_policy.h"

#include "rocksdb/slice.h"
#include "table/block_based/block_based_filter_block.h"
#include "table/block_based/full_filter_block.h"
#include "table/full_filter_bits_builder.h"
#include "util/coding.h"
#include "util/hash.h"

namespace rocksdb {

class BlockBasedFilterBlockBuilder;
class FullFilterBlockBuilder;

FullFilterBitsBuilder::FullFilterBitsBuilder(const size_t bits_per_key,
                                             const size_t num_probes)
    : bits_per_key_(bits_per_key), num_probes_(num_probes) {
  assert(bits_per_key_);
  }

  FullFilterBitsBuilder::~FullFilterBitsBuilder() {}

  void FullFilterBitsBuilder::AddKey(const Slice& key) {
    uint32_t hash = BloomHash(key);
    if (hash_entries_.size() == 0 || hash != hash_entries_.back()) {
      hash_entries_.push_back(hash);
    }
  }

  Slice FullFilterBitsBuilder::Finish(std::unique_ptr<const char[]>* buf) {
    uint32_t total_bits, num_lines;
    char* data = ReserveSpace(static_cast<int>(hash_entries_.size()),
                              &total_bits, &num_lines);
    assert(data);

    if (total_bits != 0 && num_lines != 0) {
      for (auto h : hash_entries_) {
        AddHash(h, data, num_lines, total_bits);
      }
    }
    data[total_bits/8] = static_cast<char>(num_probes_);
    EncodeFixed32(data + total_bits/8 + 1, static_cast<uint32_t>(num_lines));

    const char* const_data = data;
    buf->reset(const_data);
    hash_entries_.clear();

    return Slice(data, total_bits / 8 + 5);
  }

uint32_t FullFilterBitsBuilder::GetTotalBitsForLocality(uint32_t total_bits) {
  uint32_t num_lines =
      (total_bits + CACHE_LINE_SIZE * 8 - 1) / (CACHE_LINE_SIZE * 8);

  // Make num_lines an odd number to make sure more bits are involved
  // when determining which block.
  if (num_lines % 2 == 0) {
    num_lines++;
  }
  return num_lines * (CACHE_LINE_SIZE * 8);
}

uint32_t FullFilterBitsBuilder::CalculateSpace(const int num_entry,
                                               uint32_t* total_bits,
                                               uint32_t* num_lines) {
  assert(bits_per_key_);
  if (num_entry != 0) {
    uint32_t total_bits_tmp = num_entry * static_cast<uint32_t>(bits_per_key_);

    *total_bits = GetTotalBitsForLocality(total_bits_tmp);
    *num_lines = *total_bits / (CACHE_LINE_SIZE * 8);
    assert(*total_bits > 0 && *total_bits % 8 == 0);
  } else {
    // filter is empty, just leave space for metadata
    *total_bits = 0;
    *num_lines = 0;
  }

  // Reserve space for Filter
  uint32_t sz = *total_bits / 8;
  sz += 5;  // 4 bytes for num_lines, 1 byte for num_probes
  return sz;
}

char* FullFilterBitsBuilder::ReserveSpace(const int num_entry,
                                          uint32_t* total_bits,
                                          uint32_t* num_lines) {
  uint32_t sz = CalculateSpace(num_entry, total_bits, num_lines);
  char* data = new char[sz];
  memset(data, 0, sz);
  return data;
}

int FullFilterBitsBuilder::CalculateNumEntry(const uint32_t space) {
  assert(bits_per_key_);
  assert(space > 0);
  uint32_t dont_care1, dont_care2;
  int high = static_cast<int>(space * 8 / bits_per_key_ + 1);
  int low = 1;
  int n = high;
  for (; n >= low; n--) {
    uint32_t sz = CalculateSpace(n, &dont_care1, &dont_care2);
    if (sz <= space) {
      break;
    }
  }
  assert(n < high);  // High should be an overestimation
  return n;
}

inline void FullFilterBitsBuilder::AddHash(uint32_t h, char* data,
    uint32_t num_lines, uint32_t total_bits) {
#ifdef NDEBUG
  static_cast<void>(total_bits);
#endif
  assert(num_lines > 0 && total_bits > 0);

  const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
  uint32_t b = (h % num_lines) * (CACHE_LINE_SIZE * 8);

  for (uint32_t i = 0; i < num_probes_; ++i) {
    // Since CACHE_LINE_SIZE is defined as 2^n, this line will be optimized
    // to a simple operation by compiler.
    const uint32_t bitpos = b + (h % (CACHE_LINE_SIZE * 8));
    data[bitpos / 8] |= (1 << (bitpos % 8));

    h += delta;
  }
}

namespace {
class FullFilterBitsReader : public FilterBitsReader {
 public:
  explicit FullFilterBitsReader(const Slice& contents)
      : data_(const_cast<char*>(contents.data())),
        data_len_(static_cast<uint32_t>(contents.size())),
        num_probes_(0),
        num_lines_(0),
        log2_cache_line_size_(0) {
    assert(data_);
    GetFilterMeta(contents, &num_probes_, &num_lines_);
    // Sanitize broken parameter
    if (num_lines_ != 0 && (data_len_-5) % num_lines_ != 0) {
      num_lines_ = 0;
      num_probes_ = 0;
    } else if (num_lines_ != 0) {
      while (true) {
        uint32_t num_lines_at_curr_cache_size =
            (data_len_ - 5) >> log2_cache_line_size_;
        if (num_lines_at_curr_cache_size == 0) {
          // The cache line size seems not a power of two. It's not supported
          // and indicates a corruption so disable using this filter.
          assert(false);
          num_lines_ = 0;
          num_probes_ = 0;
          break;
        }
        if (num_lines_at_curr_cache_size == num_lines_) {
          break;
        }
        ++log2_cache_line_size_;
      }
    }
  }

  ~FullFilterBitsReader() override {}

  bool MayMatch(const Slice& entry) override {
    if (data_len_ <= 5) {   // remain same with original filter
      return false;
    }
    // Other Error params, including a broken filter, regarded as match
    if (num_probes_ == 0 || num_lines_ == 0) return true;
    uint32_t hash = BloomHash(entry);
    uint32_t bit_offset;
    FilterPrepare(hash, Slice(data_, data_len_), num_lines_, &bit_offset);
    return HashMayMatch(hash, Slice(data_, data_len_), num_probes_, bit_offset);
  }

  virtual void MayMatch(int num_keys, Slice** keys, bool* may_match) override {
    if (data_len_ <= 5) {  // remain same with original filter
      for (int i = 0; i < num_keys; ++i) {
        may_match[i] = false;
      }
      return;
    }
    for (int i = 0; i < num_keys; ++i) {
      may_match[i] = true;
    }
    // Other Error params, including a broken filter, regarded as match
    if (num_probes_ == 0 || num_lines_ == 0) return;
    uint32_t hashes[MultiGetContext::MAX_BATCH_SIZE];
    uint32_t bit_offsets[MultiGetContext::MAX_BATCH_SIZE];
    for (int i = 0; i < num_keys; ++i) {
      hashes[i] = BloomHash(*keys[i]);
      FilterPrepare(hashes[i], Slice(data_, data_len_), num_lines_,
                    &bit_offsets[i]);
    }

    for (int i = 0; i < num_keys; ++i) {
      if (!HashMayMatch(hashes[i], Slice(data_, data_len_), num_probes_,
                        bit_offsets[i])) {
        may_match[i] = false;
      }
    }
  }

 private:
  // Filter meta data
  char* data_;
  uint32_t data_len_;
  size_t num_probes_;
  uint32_t num_lines_;
  uint32_t log2_cache_line_size_;

  // Get num_probes, and num_lines from filter
  // If filter format broken, set both to 0.
  void GetFilterMeta(const Slice& filter, size_t* num_probes,
                             uint32_t* num_lines);

  // "filter" contains the data appended by a preceding call to
  // FilterBitsBuilder::Finish. This method must return true if the key was
  // passed to FilterBitsBuilder::AddKey. This method may return true or false
  // if the key was not on the list, but it should aim to return false with a
  // high probability.
  //
  // hash: target to be checked
  // filter: the whole filter, including meta data bytes
  // num_probes: number of probes, read before hand
  // num_lines: filter metadata, read before hand
  // Before calling this function, need to ensure the input meta data
  // is valid.
  bool HashMayMatch(const uint32_t& hash, const Slice& filter,
                    const size_t& num_probes, const uint32_t& bit_offset);

  void FilterPrepare(const uint32_t& hash, const Slice& filter,
                     const uint32_t& num_lines, uint32_t* bit_offset);

  // No Copy allowed
  FullFilterBitsReader(const FullFilterBitsReader&);
  void operator=(const FullFilterBitsReader&);
};

void FullFilterBitsReader::GetFilterMeta(const Slice& filter,
    size_t* num_probes, uint32_t* num_lines) {
  uint32_t len = static_cast<uint32_t>(filter.size());
  if (len <= 5) {
    // filter is empty or broken
    *num_probes = 0;
    *num_lines = 0;
    return;
  }

  *num_probes = filter.data()[len - 5];
  *num_lines = DecodeFixed32(filter.data() + len - 4);
}

void FullFilterBitsReader::FilterPrepare(const uint32_t& hash,
                                         const Slice& filter,
                                         const uint32_t& num_lines,
                                         uint32_t* bit_offset) {
  uint32_t len = static_cast<uint32_t>(filter.size());
  if (len <= 5) return;  // remain the same with original filter

  // It is ensured the params are valid before calling it
  assert(num_lines != 0 && (len - 5) % num_lines == 0);

  uint32_t h = hash;
  // Left shift by an extra 3 to convert bytes to bits
  uint32_t b = (h % num_lines) << (log2_cache_line_size_ + 3);
  PREFETCH(&filter.data()[b / 8], 0 /* rw */, 1 /* locality */);
  PREFETCH(&filter.data()[b / 8 + (1 << log2_cache_line_size_) - 1],
      0 /* rw */, 1 /* locality */);
  *bit_offset = b;
}

bool FullFilterBitsReader::HashMayMatch(const uint32_t& hash,
                                        const Slice& filter,
                                        const size_t& num_probes,
                                        const uint32_t& bit_offset) {
  uint32_t len = static_cast<uint32_t>(filter.size());
  if (len <= 5) return false;  // remain the same with original filter

  // It is ensured the params are valid before calling it
  assert(num_probes != 0);
  const char* data = filter.data();

  uint32_t h = hash;
  const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits

  for (uint32_t i = 0; i < num_probes; ++i) {
    // Since CACHE_LINE_SIZE is defined as 2^n, this line will be optimized
    //  to a simple and operation by compiler.
    const uint32_t bitpos =
        bit_offset + (h & ((1 << (log2_cache_line_size_ + 3)) - 1));
    if (((data[bitpos / 8]) & (1 << (bitpos % 8))) == 0) {
      return false;
    }

    h += delta;
  }

  return true;
}


//wp
class OtLexPdtBloomBitsReader : public FilterBitsReader {
 public:
  explicit OtLexPdtBloomBitsReader() {
    fprintf(stderr, "DEBUG pqc7a26 init OtLexPdtBloomBitsReader\n");
  }

  explicit OtLexPdtBloomBitsReader(const char* buf) {
    //fprintf(stderr,"in OtLexPdtBloomBitsReader\n");
    // construct a ot lex pdt
    // restore essential members from buf
    ot_pdt.pub_m_centroid_path_string.clear();
    ot_pdt.pub_m_labels.clear();
    ot_pdt.pub_m_centroid_path_branches.clear();
    ot_pdt.pub_m_branching_chars.clear();
    ot_pdt.pub_m_bp_m_bits.clear();
    RecoverFromCharArray(ot_pdt.pub_m_centroid_path_string,
                         ot_pdt.pub_m_labels,
                         ot_pdt.pub_m_centroid_path_branches,
                         ot_pdt.pub_m_branching_chars,
                         ot_pdt.pub_m_bp_m_bits,
                         ot_pdt.pub_m_bp_m_size,
                         new_impl,
                         sub_impl,
                         fake_num_probes,
                         buf);
          
    //fprintf(stderr,"in OtLexPdtBloomBitsReader2\n");
//    fprintf(stdout, "DEBUG uq7zbt in otReader sizes for string,label,branch,char,bit,size:%ld,%ld,%ld,%ld,%ld,%ld\n",
//            ot_pdt.pub_m_centroid_path_string.size(),
//            ot_pdt.pub_m_labels.size(),
//            ot_pdt.pub_m_centroid_path_branches.size(),
//            ot_pdt.pub_m_branching_chars.size(),
//            ot_pdt.pub_m_bp_m_bits.size(),
//            ot_pdt.pub_m_bp_m_size);

    //    for (size_t i = 0; i < ot_pdt.pub_m_centroid_path_string.size(); i++) {
//      fprintf(stdout, "Rstring:%ld,%d\n", i, ot_pdt.pub_m_centroid_path_string[i]);
//    }
//    for (size_t i = 0; i < ot_pdt.pub_m_labels.size(); i++) {
//      fprintf(stdout, "Rlabel:%ld,%d\n", i, ot_pdt.pub_m_labels[i]);
//    }
//    for (size_t i = 0; i < ot_pdt.pub_m_centroid_path_branches.size(); i++) {
//      fprintf(stdout, "Rbranch:%ld,%d\n", i, ot_pdt.pub_m_centroid_path_branches[i]);
//    }
//    for (size_t i = 0; i < ot_pdt.pub_m_branching_chars.size(); i++) {
//      fprintf(stdout, "Rchar:%ld,%d\n", i, ot_pdt.pub_m_branching_chars[i]);
//    }
//    for (size_t i = 0; i < ot_pdt.pub_m_bp_m_bits.size(); i++) {
//      fprintf(stdout, "Rbit:%ld,%ld\n", i, ot_pdt.pub_m_bp_m_bits[i]);
//    }

//    fprintf(stderr, "DEBUG c2ys95 after RecoverFromCharArray pub_m_bp_m_size:%lu pub_m_bp_m_bits.size():%lu\n",
//            ot_pdt.pub_m_bp_m_size, ot_pdt.pub_m_bp_m_bits.size());
    // init pub_* members, and create a ot lex pdt instance from it
//        ot_pdt.init_pubs();
//    auto chrono_start = std::chrono::system_clock::now();
//fprintf(stderr,"in OtLexPdtBloomBitsReader3\n");
    ot_pdt.instance();
    // ot_pdt.instance(ot_pdt.pub_m_centroid_path_string,ot_pdt.pub_m_centroid_path_branches,
    // ot_pdt.get_bp(),
    // ot_pdt.pub_m_branching_chars,ot_pdt.pub_m_labels);
//    auto chrono_end = std::chrono::system_clock::now();
//    std::chrono::microseconds elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(chrono_end-chrono_start);
//    std::cout << "DEBUG cor73n ot_pdt.instance() takes " <<
//              elapsed_us.count() << " us." << std::endl;
//fprintf(stderr,"in OtLexPdtBloomBitsReader4\n");
  }

  // No Copy allowed
  //  OtLexPdtBloomBitsReader(const&) = delete;
  void operator=(const OtLexPdtBloomBitsReader&) = delete;

  ~OtLexPdtBloomBitsReader() override {}

  bool MayMatch(const Slice& key) override {
    // idx = search_odt(key.data)
    // if idx != -1, success
    std::string key_string(key.data(), key.data()+key.size());
//    auto chrono_start = std::chrono::system_clock::now();
    size_t idx = ot_pdt.index(key_string);
//    auto chrono_end = std::chrono::system_clock::now();
//    std::chrono::microseconds elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(chrono_end-chrono_start);
//    std::cout << "DEBUG la045n ot_pdt.index ret:" << idx << ", takes(us) " <<
//              elapsed_us.count() << std::endl;

    //    fprintf(stdout, "DEBUG ab42kf in OtLexPdtBloomBitsReader::MayMatch(%s): %ld\n",
//            key.ToString().c_str(), idx);

//     XXX for test
//    return true;

    if (idx != (size_t)-1) {  // hit
      //??? blk_offset = vector<>.find(idx)
//      fprintf(stdout, "DEBUG i0j5xn ot bitsReader MayMatch(%s): %ld,\n",
//              key.ToString().c_str(), idx);
      return true;
    } else {
      return false;
    }
  }

  virtual void MayMatch(int num_keys, Slice** keys, bool* may_match) override {
    fprintf(stdout, "DEBUG w3xm82 in OtBitsReader::MayMatch(num_keys)\n");
    for (int i = 0; i < num_keys; ++i) {
      may_match[i] = MayMatch(*keys[i]);
    }
  }

  //wp
  void RecoverFromCharArray(std::vector<uint16_t>& v1,
                            std::vector<uint16_t>& v2,
                            std::vector<uint8_t>& v3,
                            std::vector<uint8_t>& v4,
                            std::vector<uint64_t>& v5,
                            uint64_t& num,
                            char& tmp_new_impl,
                            char& tmp_sub_impl,
                            char& tmp_fake_num_probes,
                            const char* & buf) {
    uint32_t size1 = 0, size2 = 0, size3 = 0, size4 = 0, size5 = 0;

    uint32_t *p = (uint32_t *) buf;
    size1 = *p;
    uint16_t *p1 = (uint16_t *) (buf + 4);
    v1.resize(size1);
    for (uint64_t i = 0; i < size1; i++) {
      v1[i] = *p1;
      p1++;
    }

    p = (uint32_t *) (buf + 4 + size1 * 2);
    size2 = *p;
    p1 = (uint16_t *) (buf + 4 + size1 * 2 + 4);
    v2.resize(size2);
    for (uint64_t i = 0; i < size2; i++) {
      v2[i] = *p1;
      p1++;
    }

    p = (uint32_t *) (buf + 4 + size1 * 2 + 4 + size2 * 2);
    size3 = *p;
    uint8_t *p2 = (uint8_t *) (buf + 4 + size1 * 2 + 4 + size2 * 2 + 4);
    v3.resize(size3);
    for (uint64_t i = 0; i < size3; i++) {
      v3[i] = *p2;
      p2++;
    }

    p = (uint32_t *) (buf + 4 + size1 * 2 + 4 + size2 * 2 + 4 + size3);
    size4 = *p;
    p2 = (uint8_t *) (buf + 4 + size1 * 2 + 4 + size2 * 2 + 4 + size3 + 4);
    v4.resize(size4);
    for (uint64_t i = 0; i < size4; i++) {
      v4[i] = *p2;
      p2++;
    }

    p = (uint32_t *) (buf + 4 + size1 * 2 + 4 + size2 * 2 + 4 + size3 + 4 + size4);
    size5 = *p;
    uint64_t *p4 = (uint64_t *) (buf + 4 + size1 * 2 + 4 + size2 * 2 + 4 + size3 + 4 + size4 + 4);
    v5.resize(size5);
    for (uint64_t i = 0; i < size5; i++) {
      v5[i] = *p4;
      p4++;
    }

    uint64_t *p3 = (uint64_t *) (buf + 4 + size1 * 2 + 4 + size2 * 2 + 4 + size3 + 4 + size4 + 4 + size5 * 8);
    num = *p3;
//    fprintf(stderr, "DEBUG m72qa4 RecoverFromCharArray num: %lu\n", num);

    //xp, be compatible with full filter
    char* pc1 = (char*) (buf + 4 + size1 * 2 + 4 + size2 * 2 + 4 + size3 + 4 + size4 + 4 + size5 * 8 + 8);
    tmp_new_impl = *pc1;
    char* pc2 = (char*) (buf + 4 + size1 * 2 + 4 + size2 * 2 + 4 + size3 + 4 + size4 + 4 + size5 * 8 + 8+1);
    tmp_sub_impl = *pc2;
    char* pc3 = (char*) (buf + 4 + size1 * 2 + 4 + size2 * 2 + 4 + size3 + 4 + size4 + 4 + size5 * 8 + 8+1+1);
    tmp_fake_num_probes = *pc3;
    char* pc4 = (char*) (buf + 4 + size1 * 2 + 4 + size2 * 2 + 4 + size3 + 4 + size4 + 4 + size5 * 8 + 8+1+1+1);
    tmp_fake_num_probes = *pc4;
    char* pc5 = (char*) (buf + 4 + size1 * 2 + 4 + size2 * 2 + 4 + size3 + 4 + size4 + 4 + size5 * 8 + 8+1+1+1+1);
    tmp_fake_num_probes = *pc5;

// uint64_t sssize=pc5-buf+1;
     //fprintf(stderr,"in Recover buf size=%ld\n\n\n",pc5-buf+1);

//     fprintf(stderr,"/n/n/n/n/nbuf contents/n");
//     for(uint64_t i=0;i<sssize;i++)
//     {
//       fprintf(stderr,"%d",buf[i]);
//     }
// fprintf(stderr,"\n");
  }

  // be compatible with full filter in GetBloomBitsReader
  char new_impl;
  char sub_impl;
  char fake_num_probes;
  // a ot lex pdt
  rocksdb::succinct::tries::path_decomposed_trie<
      rocksdb::succinct::tries::vbyte_string_pool, true>
      ot_pdt;
  // a vector<> stores blk boundary keys
  //TODO
};





// An implementation of filter policy
class BloomFilterPolicy : public FilterPolicy {
 public:

  //wp
  bool isPdt=false;
  
  explicit BloomFilterPolicy(int bits_per_key, bool use_block_based_builder)
      : bits_per_key_(bits_per_key), hash_func_(BloomHash),
        use_block_based_builder_(use_block_based_builder) {
    initialize();
  }

  ~BloomFilterPolicy() override {}

  const char* Name() const override { return "rocksdb.BuiltinBloomFilter"; }

  void CreateFilter(const Slice* keys, int n, std::string* dst) const override {
    printf("in bloom CreateFilter\n");
    // Compute bloom filter size (in both bits and bytes)
    size_t bits = n * bits_per_key_;

    // For small n, we can see a very high false positive rate.  Fix it
    // by enforcing a minimum bloom filter length.
    if (bits < 64) bits = 64;

    size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;

    const size_t init_size = dst->size();
    dst->resize(init_size + bytes, 0);
    dst->push_back(static_cast<char>(num_probes_));  // Remember # of probes
    char* array = &(*dst)[init_size];
    for (size_t i = 0; i < static_cast<size_t>(n); i++) {
      // Use double-hashing to generate a sequence of hash values.
      // See analysis in [Kirsch,Mitzenmacher 2006].
      uint32_t h = hash_func_(keys[i]);
      const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
      for (size_t j = 0; j < num_probes_; j++) {
        const uint32_t bitpos = h % bits;
        array[bitpos/8] |= (1 << (bitpos % 8));
        h += delta;
      }
    }
  }

  bool KeyMayMatch(const Slice& key, const Slice& bloom_filter) const override {
    printf("in bloom KeyMayMatch\n");
    const size_t len = bloom_filter.size();
    if (len < 2) return false;

    const char* array = bloom_filter.data();
    const size_t bits = (len - 1) * 8;

    // Use the encoded k so that we can read filters generated by
    // bloom filters created using different parameters.
    const size_t k = array[len-1];
    if (k > 30) {
      // Reserved for potentially new encodings for short bloom filters.
      // Consider it a match.
      return true;
    }

    uint32_t h = hash_func_(key);
    const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
    for (size_t j = 0; j < k; j++) {
      const uint32_t bitpos = h % bits;
      if ((array[bitpos/8] & (1 << (bitpos % 8))) == 0) return false;
      h += delta;
    }
    return true;
  }

  FilterBitsBuilder* GetFilterBitsBuilder() const override {
    printf("in bloom GetFilterBitsBuilder\n");
    if (use_block_based_builder_) {
      return nullptr;
    }
    if(isPdt)
    {
      return new OtLexPdtBloomBitsBuilder();
    }
    else
    {
      return new FullFilterBitsBuilder(bits_per_key_, num_probes_);
    }
  }

  FilterBitsReader* GetFilterBitsReader(const Slice& contents) const override {

    //fprintf(stderr,"in bloom GetFilterBitsReader\n");
    if(isPdt)
    {
      return new OtLexPdtBloomBitsReader(contents.data());
    }
    return new FullFilterBitsReader(contents);
  }

  // If choose to use block based builder
  bool UseBlockBasedBuilder() { return use_block_based_builder_; }

 private:
  size_t bits_per_key_;
  size_t num_probes_;
  uint32_t (*hash_func_)(const Slice& key);

  const bool use_block_based_builder_;

  void initialize() {
    printf("in bloom initialize\n");
    // We intentionally round down to reduce probing cost a little bit
    num_probes_ = static_cast<size_t>(bits_per_key_ * 0.69);  // 0.69 =~ ln(2)
    if (num_probes_ < 1) num_probes_ = 1;
    if (num_probes_ > 30) num_probes_ = 30;
  }
};

}  // namespace

const FilterPolicy* NewBloomFilterPolicy(int bits_per_key,
                                         bool use_block_based_builder) {
      printf("\n\n\n\nin NewBloomFilterPolicy\n");
  BloomFilterPolicy* p=new BloomFilterPolicy(bits_per_key, use_block_based_builder);
  p->isPdt=true;
  return p;
}

}  // namespace rocksdb
