// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// SizeCeph_Actual-based Erasure Code Plugin - Production-Safe Implementation
// Based on comprehensive analysis of 381 failure patterns with guaranteed 3-OSD fault tolerance

#ifndef CEPH_ERASURE_CODE_SIZE_CEPH_ACTUAL_H
#define CEPH_ERASURE_CODE_SIZE_CEPH_ACTUAL_H

#include "erasure-code/ErasureCodeInterface.h"
#include "osd/osd_types.h"
#include <mutex>

class ErasureCodeSizeCephActual : public ceph::ErasureCodeInterface {
public:
  ErasureCodeSizeCephActual();
  ~ErasureCodeSizeCephActual() override;
  
  // ErasureCodeInterface required virtual methods
  int init(ceph::ErasureCodeProfile &profile, std::ostream *ss) override;
  const ceph::ErasureCodeProfile &get_profile() const override;
  int create_rule(const std::string &name, CrushWrapper &crush, std::ostream *ss) const override;
  
  unsigned int get_chunk_count() const override;
  unsigned int get_data_chunk_count() const override;
  unsigned int get_coding_chunk_count() const override;
  int get_sub_chunk_count() override;
  unsigned int get_chunk_size(unsigned int stripe_width) const override;
  
  int minimum_to_decode(const shard_id_set &want_to_read,
                        const shard_id_set &available,
                        shard_id_set &minimum_set,
                        mini_flat_map<shard_id_t, std::vector<std::pair<int, int>>> *minimum_sub_chunks) override;
  
  [[deprecated]]
  int minimum_to_decode(const std::set<int> &want_to_read,
                        const std::set<int> &available,
                        std::map<int, std::vector<std::pair<int, int>>> *minimum) override;
  
  int minimum_to_decode_with_cost(const shard_id_set &want_to_read,
                                  const shard_id_map<int> &available,
                                  shard_id_set *minimum) override;
  
  [[deprecated]]
  int minimum_to_decode_with_cost(const std::set<int> &want_to_read,
                                  const std::map<int, int> &available,
                                  std::set<int> *minimum) override;
  
  size_t get_minimum_granularity() override;
  
  // Encode methods
  int encode(const shard_id_set &want_to_encode,
             const ceph::bufferlist &in,
             shard_id_map<ceph::bufferlist> *encoded) override;
  
  [[deprecated]]
  int encode(const std::set<int> &want_to_encode,
             const ceph::bufferlist &in,
             std::map<int, ceph::bufferlist> *encoded) override;
  
  [[deprecated]]
  int encode_chunks(const std::set<int> &want_to_encode,
                    std::map<int, ceph::bufferlist> *encoded) override;
  
  int encode_chunks(const shard_id_map<ceph::bufferptr> &in,
                    shard_id_map<ceph::bufferptr> &out) override;
  
  void encode_delta(const ceph::bufferptr &old_data,
                    const ceph::bufferptr &new_data,
                    ceph::bufferptr *delta_maybe_in_place) override;
  
  void apply_delta(const shard_id_map<ceph::bufferptr> &in,
                   shard_id_map<ceph::bufferptr> &out) override;
  
  // Decode methods
  int decode(const shard_id_set &want_to_read,
             const shard_id_map<ceph::bufferlist> &chunks,
             shard_id_map<ceph::bufferlist> *decoded, int chunk_size) override;
  
  [[deprecated]]
  int decode(const std::set<int> &want_to_read,
             const std::map<int, ceph::bufferlist> &chunks,
             std::map<int, ceph::bufferlist> *decoded, int chunk_size) override;
  
  int decode_chunks(const shard_id_set &want_to_read,
                    shard_id_map<ceph::bufferptr> &in,
                    shard_id_map<ceph::bufferptr> &out) override;
  
  [[deprecated]]
  int decode_chunks(const std::set<int> &want_to_read,
                    const std::map<int, ceph::bufferlist> &chunks,
                    std::map<int, ceph::bufferlist> *decoded) override;
  
  const std::vector<shard_id_t> &get_chunk_mapping() const override;
  
  [[deprecated]]
  int decode_concat(const std::set<int>& want_to_read,
                    const std::map<int, ceph::bufferlist> &chunks,
                    ceph::bufferlist *decoded) override;
  
  [[deprecated]]
  int decode_concat(const std::map<int, ceph::bufferlist> &chunks,
                    ceph::bufferlist *decoded) override;
  
  plugin_flags get_supported_optimizations() const override;

private:
  // SizeCeph_Actual configuration based on comprehensive 381-pattern analysis
  static const unsigned int SIZECEPH_ACTUAL_K = 4;    // Data chunks
  static const unsigned int SIZECEPH_ACTUAL_M = 5;    // Parity chunks  
  static const unsigned int SIZECEPH_ACTUAL_N = 9;    // Total chunks (K+M)
  static const unsigned int SIZECEPH_ACTUAL_MIN_OSDS = 6;    // K+M-3 = minimum OSDs for safe reads
  static const unsigned int SIZECEPH_ACTUAL_MAX_FAILURES = 3; // Guaranteed safe failure tolerance
  static const unsigned int SIZECEPH_ACTUAL_ALIGNMENT = 4;     // SizeCeph_actual processes 4 bytes at a time
  
  ceph::ErasureCodeProfile profile;
  std::vector<shard_id_t> chunk_mapping;
  
  // SizeCeph_Actual library interface
  static void* sizeceph_actual_handle;
  static bool library_loaded;
  static int library_ref_count;
  static std::mutex library_mutex;
  
  // Function pointers for sizeceph_actual library
  typedef void (*sizeceph_split_fn_t)(unsigned char **pp_dst, unsigned char *p_src, unsigned int len);
  typedef int (*sizeceph_restore_fn_t)(unsigned char *p_dst, const unsigned char **pp_src, unsigned int len);
  typedef int (*sizeceph_can_get_restore_fn_t)(const unsigned char **pp_src);
  
  static sizeceph_split_fn_t sizeceph_split_func;
  static sizeceph_restore_fn_t sizeceph_restore_func;
  static sizeceph_can_get_restore_fn_t sizeceph_can_get_restore_func;
  
  bool load_sizeceph_actual_library();
  void unload_sizeceph_actual_library();
  void unload_sizeceph_actual_library_unsafe(); // Internal version without mutex
  
  // Safety validation based on 381-pattern analysis
  bool validate_failure_pattern(const shard_id_set &available) const;
  bool is_safe_to_decode(const shard_id_set &available, const shard_id_set &want_to_read) const;
  
  // Helper methods
  unsigned get_alignment() const;
  int calculate_aligned_size(int original_size) const;
  void interleave_data(const char *src, char **dst_chunks, int chunk_size, int num_chunks) const;
  void deinterleave_data(char **src_chunks, char *dst, int chunk_size, int num_chunks) const;
  
  // Internal encode/decode with production-safe 6-OSD requirement
  int sizeceph_actual_encode_internal(const ceph::bufferlist &in, shard_id_map<ceph::bufferlist> *encoded);
  int sizeceph_actual_decode_internal(const shard_id_map<ceph::bufferlist> &chunks, 
                                      ceph::bufferlist *decoded, int original_size);
};

#endif // CEPH_ERASURE_CODE_SIZE_CEPH_ACTUAL_H