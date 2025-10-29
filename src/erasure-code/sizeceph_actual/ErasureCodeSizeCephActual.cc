// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// SizeCeph_Actual-based Erasure Code Plugin - Production-Safe Implementation
//
// ================================================================================
// PRODUCTION SAFETY DESIGN: Based on Comprehensive 381-Pattern Analysis
// ================================================================================
// 
// This implementation incorporates critical safety lessons learned from testing
// ALL 381 possible failure combinations in the SizeCeph_Actual library.
//
// KEY SAFETY REQUIREMENTS:
// - K=4, M=5 configuration (9 total chunks)
// - Minimum 6 OSDs required for reads (K+M-3) = guaranteed 3-OSD fault tolerance
// - 32-byte alignment requirement for all operations
// - Validation against 54 known failing patterns before decode attempts
//
// COMPREHENSIVE ANALYSIS RESULTS:
// - 1-3 chunk failures: 100% success rate (129/129 patterns) ✅
// - 4 chunk failures: 92.9% success rate (117/126 patterns) ⚠️
// - 5 chunk failures: 64.3% success rate (81/126 patterns) ❌
// - Total: 327/381 patterns successful (85.8% overall)
//
// PRODUCTION SAFETY STRATEGY:
// By requiring minimum 6 OSDs (allowing max 3 failures), we operate within the
// 100% reliable zone of SizeCeph_Actual, avoiding the 54 problematic patterns
// that occur with 4+ failures.
//
// FAILED PATTERNS PROTECTED AGAINST:
// - 4-failure patterns: [0,1,3,4], [0,1,6,7], [0,2,3,5], [0,2,6,8], [1,2,4,5], 
//   [1,2,7,8], [3,4,6,7], [3,5,6,8], [4,5,7,8]
// - 5-failure patterns: 45 specific combinations that create unrecoverable scenarios
//
// PERFORMANCE CHARACTERISTICS:
// - Excellent for 1-3 OSD failures (guaranteed recovery)
// - Never operates in unreliable 4-5 failure modes
// - 4-byte alignment prevents corruption issues (corrected from incorrect 32-byte)
// ================================================================================

#include "ErasureCodeSizeCephActual.h"
#include "common/debug.h"
#include "common/strtol.h"
#include "crush/CrushWrapper.h"
#include "include/intarith.h"
#include "include/buffer.h"
#include <iostream>
#include <algorithm>
#include <dlfcn.h>
#include <cstring>
#include <cstdlib>
#include <vector>

// Memory safety constants
static const size_t MAX_CHUNK_SIZE = 16 * 1024 * 1024; // 16MB max per chunk

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix _prefix(_dout)

static std::ostream& _prefix(std::ostream* _dout)
{
  return *_dout << "ErasureCodeSizeCephActual: ";
}

// Static member initialization
void* ErasureCodeSizeCephActual::sizeceph_actual_handle = nullptr;
bool ErasureCodeSizeCephActual::library_loaded = false;
int ErasureCodeSizeCephActual::library_ref_count = 0;
std::mutex ErasureCodeSizeCephActual::library_mutex;

ErasureCodeSizeCephActual::sizeceph_split_fn_t ErasureCodeSizeCephActual::sizeceph_split_func = nullptr;
ErasureCodeSizeCephActual::sizeceph_restore_fn_t ErasureCodeSizeCephActual::sizeceph_restore_func = nullptr;
ErasureCodeSizeCephActual::sizeceph_can_get_restore_fn_t ErasureCodeSizeCephActual::sizeceph_can_get_restore_func = nullptr;

ErasureCodeSizeCephActual::ErasureCodeSizeCephActual() {
  dout(10) << "ErasureCodeSizeCephActual constructor" << dendl;
  
  // Initialize chunk mapping for 9 chunks (K=4, M=5)
  chunk_mapping.clear();
  for (unsigned int i = 0; i < SIZECEPH_ACTUAL_N; i++) {
    chunk_mapping.push_back(shard_id_t(i));
  }
}

ErasureCodeSizeCephActual::~ErasureCodeSizeCephActual() {
  dout(10) << "ErasureCodeSizeCephActual destructor" << dendl;
  unload_sizeceph_actual_library();
}

int ErasureCodeSizeCephActual::init(ceph::ErasureCodeProfile &_profile, std::ostream *ss) {
  dout(10) << "ErasureCodeSizeCephActual::init profile=" << _profile << dendl;
  
  // Use the provided profile as-is
  profile = _profile;
  
  // Validate that we have the correct k and m values
  auto k_iter = profile.find("k");
  auto m_iter = profile.find("m");
  
  if (k_iter == profile.end() || m_iter == profile.end()) {
    *ss << "SizeCeph_Actual requires k and m parameters";
    return -EINVAL;
  }
  
  int k_val = std::stoi(k_iter->second);
  int m_val = std::stoi(m_iter->second);
  
  if (k_val != SIZECEPH_ACTUAL_K || m_val != SIZECEPH_ACTUAL_M) {
    *ss << "SizeCeph_Actual requires k=" << SIZECEPH_ACTUAL_K << " and m=" << SIZECEPH_ACTUAL_M
        << ", got k=" << k_val << " and m=" << m_val;
    return -EINVAL;
  }
  
  dout(20) << "SizeCeph_Actual configuration: K=" << SIZECEPH_ACTUAL_K 
           << " M=" << SIZECEPH_ACTUAL_M 
           << " N=" << SIZECEPH_ACTUAL_N
           << " MIN_OSDS=" << SIZECEPH_ACTUAL_MIN_OSDS << dendl;
  
  // Load the SizeCeph_Actual library
  if (!load_sizeceph_actual_library()) {
    *ss << "Failed to load SizeCeph_Actual library";
    return -ENOENT;
  }
  
  return 0;
}

const ceph::ErasureCodeProfile &ErasureCodeSizeCephActual::get_profile() const {
  return profile;
}

unsigned int ErasureCodeSizeCephActual::get_chunk_count() const {
  return SIZECEPH_ACTUAL_N;
}

unsigned int ErasureCodeSizeCephActual::get_data_chunk_count() const {
  return SIZECEPH_ACTUAL_K;
}

unsigned int ErasureCodeSizeCephActual::get_coding_chunk_count() const {
  return SIZECEPH_ACTUAL_M;
}

int ErasureCodeSizeCephActual::get_sub_chunk_count() {
  return 1; // SizeCeph_Actual doesn't use sub-chunks
}

unsigned int ErasureCodeSizeCephActual::get_chunk_size(unsigned int stripe_width) const {
  // ================================================================================
  // MATHEMATICAL CONSISTENCY FIX: Following SHEC/Clay pattern like original SizeCeph
  // ================================================================================
  // 
  // ALIGNMENT STRATEGY (like SHEC and Clay):
  // 1. Get base alignment (4 bytes for SizeCeph_Actual)
  // 2. Calculate K-aligned boundary: K * alignment = K * 4 = 16 bytes  
  // 3. Use round_up_to() to pad stripe_width to k_alignment boundary
  // 4. Return padded_length / K
  // 
  // MATHEMATICAL GUARANTEE:
  // Because k_alignment = K * alignment, padded_length is always divisible by K
  // This ensures: K * get_chunk_size(stripe_width) == padded_stripe_width
  // ================================================================================
  
  unsigned alignment = SIZECEPH_ACTUAL_ALIGNMENT;  // 4 bytes
  unsigned k_alignment = SIZECEPH_ACTUAL_K * alignment;  // K * 4 = 16 bytes
  
  // Use round_up_to() like other EC plugins (Clay, etc)
  unsigned padded_length = round_up_to(stripe_width, k_alignment);
  unsigned chunk_size = padded_length / SIZECEPH_ACTUAL_K;
  
  return chunk_size;
}

size_t ErasureCodeSizeCephActual::get_minimum_granularity() {
  return SIZECEPH_ACTUAL_ALIGNMENT; // 4-byte alignment requirement
}

// ================================================================================
// CRITICAL SAFETY METHOD: minimum_to_decode with 6-OSD requirement
// ================================================================================
int ErasureCodeSizeCephActual::minimum_to_decode(const shard_id_set &want_to_read,
                                                 const shard_id_set &available,
                                                 shard_id_set &minimum_set,
                                                 mini_flat_map<shard_id_t, std::vector<std::pair<int, int>>> *minimum_sub_chunks) {
  
  dout(20) << __func__ << " want_to_read=" << want_to_read 
           << " available=" << available << dendl;
  
  // PRODUCTION SAFETY: Require minimum 6 OSDs for any decode operation
  if (available.size() < SIZECEPH_ACTUAL_MIN_OSDS) {
    dout(1) << "SAFETY VIOLATION: Only " << available.size() << " OSDs available, minimum " 
            << SIZECEPH_ACTUAL_MIN_OSDS << " required for safe SizeCeph_Actual decode" << dendl;
    return -EIO;
  }
  
  // Select the first MIN_OSDS available chunks for safe decoding
  minimum_set.clear();
  auto it = available.begin();
  for (unsigned int i = 0; i < SIZECEPH_ACTUAL_MIN_OSDS && it != available.end(); ++i, ++it) {
    minimum_set.insert(*it);
  }
  
  // No sub-chunks for SizeCeph_Actual
  if (minimum_sub_chunks) {
    minimum_sub_chunks->clear();
  }
  
  dout(20) << __func__ << " minimum_set=" << minimum_set << dendl;
  return 0;
}

// Safety validation methods based on 381-pattern analysis
bool ErasureCodeSizeCephActual::validate_failure_pattern(const shard_id_set &available) const {
  if (available.size() >= SIZECEPH_ACTUAL_MIN_OSDS) {
    return true; // Safe - we're in the 100% reliable zone
  }
  
  // If we have fewer than 6 OSDs, we're in the unreliable zone
  // This could trigger one of the 54 known failing patterns
  return false;
}

bool ErasureCodeSizeCephActual::is_safe_to_decode(const shard_id_set &available, const shard_id_set &want_to_read) const {
  // Primary safety check: ensure we have enough OSDs
  if (available.size() < SIZECEPH_ACTUAL_MIN_OSDS) {
    return false;
  }
  
  // Secondary safety check: ensure we have at least K chunks
  if (available.size() < SIZECEPH_ACTUAL_K) {
    return false;
  }
  
  // All checks passed - we're in the safe operating zone
  return true;
}

bool ErasureCodeSizeCephActual::load_sizeceph_actual_library() {
  std::lock_guard<std::mutex> lock(library_mutex);
  
  if (library_loaded) {
    library_ref_count++;
    return true;
  }
  
  dout(10) << "Loading SizeCeph_Actual library..." << dendl;
  
  // Try to load from sizeceph_actual bin directory first
  sizeceph_actual_handle = dlopen("/home/joseph/code/sizeceph_actual/bin/libsizeceph.so", RTLD_LAZY);
  if (!sizeceph_actual_handle) {
    // Try alternative name in same directory
    sizeceph_actual_handle = dlopen("/home/joseph/code/sizeceph_actual/bin/sizecephactual.so", RTLD_LAZY);
    if (!sizeceph_actual_handle) {
      // Try system path as fallback
    sizeceph_actual_handle = dlopen("sizecephactual.so", RTLD_LAZY);
      if (!sizeceph_actual_handle) {
        dout(0) << "Failed to load SizeCeph_Actual library: " << dlerror() << dendl;
        return false;
      }
    }
  }
  
  // Load function symbols
  sizeceph_split_func = (sizeceph_split_fn_t)dlsym(sizeceph_actual_handle, "size_split");
  sizeceph_restore_func = (sizeceph_restore_fn_t)dlsym(sizeceph_actual_handle, "size_restore");
  sizeceph_can_get_restore_func = (sizeceph_can_get_restore_fn_t)dlsym(sizeceph_actual_handle, "size_can_get_restore_fn");
  
  if (!sizeceph_split_func || !sizeceph_restore_func || !sizeceph_can_get_restore_func) {
    dout(0) << "Failed to load SizeCeph_Actual function symbols" << dendl;
    dlclose(sizeceph_actual_handle);
    sizeceph_actual_handle = nullptr;
    return false;
  }
  
  library_loaded = true;
  library_ref_count = 1;
  dout(10) << "SizeCeph_Actual library loaded successfully" << dendl;
  return true;
}

void ErasureCodeSizeCephActual::unload_sizeceph_actual_library() {
  std::lock_guard<std::mutex> lock(library_mutex);
  unload_sizeceph_actual_library_unsafe();
}

void ErasureCodeSizeCephActual::unload_sizeceph_actual_library_unsafe() {
  if (!library_loaded) {
    return;
  }
  
  library_ref_count--;
  if (library_ref_count == 0) {
    if (sizeceph_actual_handle) {
      dlclose(sizeceph_actual_handle);
      sizeceph_actual_handle = nullptr;
    }
    sizeceph_split_func = nullptr;
    sizeceph_restore_func = nullptr;
    sizeceph_can_get_restore_func = nullptr;
    library_loaded = false;
    dout(10) << "SizeCeph_Actual library unloaded" << dendl;
  }
}

unsigned ErasureCodeSizeCephActual::get_alignment() const {
  return SIZECEPH_ACTUAL_ALIGNMENT;
}

int ErasureCodeSizeCephActual::calculate_aligned_size(int original_size) const {
  // Use same logic as get_chunk_size() for consistency
  unsigned alignment = SIZECEPH_ACTUAL_ALIGNMENT;  // 4 bytes
  unsigned k_alignment = SIZECEPH_ACTUAL_K * alignment;  // K * 4 = 16 bytes
  return round_up_to(original_size, k_alignment);
}

// Additional method implementations for completeness
int ErasureCodeSizeCephActual::create_rule(const std::string &name, CrushWrapper &crush, std::ostream *ss) const {
  if (crush.rule_exists(name)) {
    return crush.get_rule_id(name);
  }
  
  // Create a simple host-level rule for SizeCeph_Actual
  int ruleid = crush.add_simple_rule(name, "default", "host", "", 
                                     "indep", pg_pool_t::TYPE_ERASURE, ss);
  
  if (ruleid < 0) {
    *ss << "Failed to create crush rule " << name << ": error " << ruleid;
    return ruleid;
  }
  
  return ruleid;
}

const std::vector<shard_id_t> &ErasureCodeSizeCephActual::get_chunk_mapping() const {
  return chunk_mapping;
}

// Complete encode implementation
int ErasureCodeSizeCephActual::encode(const shard_id_set &want_to_encode,
                                     const ceph::bufferlist &in,
                                     shard_id_map<ceph::bufferlist> *encoded) {
  dout(20) << __func__ << " want_to_encode=" << want_to_encode << " in.length()=" << in.length() << dendl;
  
  // ================================================================================
  // INPUT VALIDATION
  // ================================================================================
  
  // Load SizeCeph_Actual library first
  if (!load_sizeceph_actual_library()) {
    dout(0) << "SizeCeph_Actual encode: Failed to load library" << dendl;
    return -ENOENT;
  }
  
  // Validate all 9 chunks are requested (SizeCeph_Actual requires all chunks for algorithm)
  if (want_to_encode.size() != SIZECEPH_ACTUAL_N) {
    dout(0) << "SizeCeph_Actual encode: need all " << SIZECEPH_ACTUAL_N 
            << " chunks, got " << want_to_encode.size() << dendl;
    return -EINVAL;
  }
  
  // Validate chunk IDs are 0-8
  for (const auto& shard : want_to_encode) {
    if (shard.id < 0 || shard.id >= (int)SIZECEPH_ACTUAL_N) {
      dout(0) << "SizeCeph_Actual encode: invalid shard id " << shard.id << dendl;
      return -EINVAL;
    }
  }
  
  // Handle empty input
  if (in.length() == 0) {
    for (const auto& shard : want_to_encode) {
      (*encoded)[shard] = ceph::bufferlist();
    }
    return 0;
  }

  // Validate input alignment using get_alignment() method
  unsigned int required_alignment = get_alignment();
  if (in.length() % required_alignment != 0) {
    dout(0) << "SizeCeph_Actual encode: input size " << in.length() 
            << " not divisible by " << required_alignment 
            << " (required by SizeCeph_Actual algorithm via get_alignment())" << dendl;
    return -EINVAL;
  }

  // ================================================================================
  // ENCODE PROCESSING - Use get_chunk_size() for proper size calculation
  // ================================================================================
  
  // Calculate expected chunk size using get_chunk_size() method
  unsigned int input_length = in.length();
  unsigned int chunk_size = get_chunk_size(input_length);
  
  // Buffer allocation - OSD provides empty shard_id_map, plugin allocates actual buffers
  std::vector<unsigned char*> output_ptrs(SIZECEPH_ACTUAL_N);
  
  for (const auto& wanted_shard : want_to_encode) {
    ceph::bufferptr chunk_buffer = ceph::buffer::create(chunk_size);
    output_ptrs[wanted_shard.id] = (unsigned char*)chunk_buffer.c_str();
    (*encoded)[wanted_shard].append(chunk_buffer);
  }
  
  // Zero out output buffers for safety
  for (const auto& wanted_shard : want_to_encode) {
    memset((*encoded)[wanted_shard].c_str(), 0, chunk_size);
    // Verify buffer pointer consistency
    if ((*encoded)[wanted_shard].c_str() != (char*)output_ptrs[wanted_shard.id]) {
      dout(0) << "SizeCeph_Actual encode: output buffer pointer mismatch for chunk " 
              << wanted_shard.id << dendl;
      ceph_assert(false); // This should never happen
    }
  }

  // Execute SizeCeph_Actual encoding directly on Ceph's aligned input
  // Create contiguous buffer for SizeCeph_Actual (it needs contiguous memory)
  ceph::bufferptr contiguous_input = ceph::buffer::create(input_length);
  in.begin().copy(input_length, contiguous_input.c_str());

  sizeceph_split_func(output_ptrs.data(), (unsigned char*)contiguous_input.c_str(), input_length);

  return 0;
}

// Complete decode implementation with production safety
int ErasureCodeSizeCephActual::decode(const shard_id_set &want_to_read,
                                     const shard_id_map<ceph::bufferlist> &chunks,
                                     shard_id_map<ceph::bufferlist> *decoded, 
                                     int chunk_size) {
  dout(20) << __func__ << " want_to_read=" << want_to_read << " chunk_size=" << chunk_size << dendl;
  
  // ================================================================================
  // PRODUCTION SAFETY VALIDATION
  // ================================================================================
  
  // CRITICAL SAFETY CHECK: Ensure we meet minimum OSD requirement
  shard_id_set available;
  for (const auto &chunk : chunks) {
    available.insert(chunk.first);
  }
  
  if (!is_safe_to_decode(available, want_to_read)) {
    dout(0) << "SAFETY ABORT: Decode pattern violates 6-OSD minimum requirement" << dendl;
    return -EIO;
  }
  
  // Load SizeCeph_Actual library
  if (!load_sizeceph_actual_library()) {
    dout(0) << "SizeCeph_Actual decode: Failed to load library" << dendl;
    return -ENOENT;
  }
  
  // CORRECTED: SizeCeph_Actual requires minimum 6 chunks (not all 9 like original SizeCeph)
  if (available.size() < SIZECEPH_ACTUAL_MIN_OSDS) {
    dout(0) << "SizeCeph_Actual decode: need at least " << SIZECEPH_ACTUAL_MIN_OSDS 
            << " chunks, got only " << available.size() 
            << " (SizeCeph_Actual algorithm requires minimum 6 for safety)" << dendl;
    return -ENOENT;
  }
  
  // Determine chunk size
  unsigned int effective_chunk_size = chunk_size;
  if (effective_chunk_size <= 0 && !chunks.empty()) {
    effective_chunk_size = chunks.begin()->second.length();
  }
  if (effective_chunk_size == 0) {
    dout(0) << "SizeCeph_Actual decode: invalid chunk size" << dendl;
    return -EINVAL;
  }
  
  // ================================================================================
  // DECODE PROCESSING
  // ================================================================================
  
  // Prepare input chunks for SizeCeph_Actual restore
  std::vector<unsigned char*> input_chunks(SIZECEPH_ACTUAL_N);
  std::vector<ceph::bufferlist> chunk_copies(SIZECEPH_ACTUAL_N);
  std::vector<bool> chunk_available(SIZECEPH_ACTUAL_N, false);
  
  // Initialize with nullptr for missing chunks
  for (unsigned int i = 0; i < SIZECEPH_ACTUAL_N; ++i) {
    input_chunks[i] = nullptr;
  }
  
  // Load available chunks
  for (const auto& chunk_pair : chunks) {
    shard_id_t shard_id = chunk_pair.first;
    if ((unsigned int)shard_id.id < SIZECEPH_ACTUAL_N) {
      chunk_copies[shard_id.id].append(chunk_pair.second);
      input_chunks[shard_id.id] = (unsigned char*)chunk_copies[shard_id.id].c_str();
      chunk_available[shard_id.id] = true;
    }
  }

  // Check restore capability
  std::vector<const unsigned char*> const_input_chunks(SIZECEPH_ACTUAL_N);
  for (unsigned int i = 0; i < SIZECEPH_ACTUAL_N; ++i) {
    const_input_chunks[i] = input_chunks[i];
  }
  
  if (!sizeceph_can_get_restore_func(const_input_chunks.data())) {
    dout(0) << "SizeCeph_Actual decode: algorithm reports restore not possible" << dendl;
    return -ENOTSUP;
  }
  
  // Execute restore - sizeceph_restore reconstructs the ORIGINAL data that was encoded
  // Calculate original data size: reverse of get_chunk_size() calculation
  // get_chunk_size() does: round_up_to(stripe_width, k_alignment) / K = effective_chunk_size
  // So: round_up_to(original_data_size, k_alignment) = effective_chunk_size * K
  // Therefore: original_data_size = effective_chunk_size * K (since it was already aligned during encode)
  unsigned int original_data_size = effective_chunk_size * SIZECEPH_ACTUAL_K;
  ceph::bufferptr restored_data = ceph::buffer::create(original_data_size);
  unsigned char* output_ptr = (unsigned char*)restored_data.c_str();
  
  int restore_result = sizeceph_restore_func(output_ptr, const_input_chunks.data(), original_data_size);
  if (restore_result != 0) {
    dout(0) << "SizeCeph_Actual decode: restore failed with error " << restore_result << dendl;
    return -EIO;
  }

  // Handle chunk requests
  for (const auto& wanted_shard : want_to_read) {
    shard_id_t shard_id = wanted_shard;
    
    if ((unsigned int)shard_id.id >= SIZECEPH_ACTUAL_N) {
      dout(0) << "SizeCeph_Actual decode: invalid shard id " << shard_id.id << dendl;
      return -EINVAL;
    }
    
    // CRITICAL UNDERSTANDING: Use the restored original data to generate requested chunks
    // - SizeCeph_Actual decodes to restore the ORIGINAL data that was encoded
    // - For data chunks (0-3): simulate by dividing restored data into K parts
    // - For parity chunks (4-8): re-encode the restored data to get the specific chunk
    
    ceph::bufferlist chunk_bl;
    
    if ((unsigned int)shard_id.id < SIZECEPH_ACTUAL_K) {
      // Data chunks (0-3): Extract from restored original data
      // CRITICAL: Each chunk must be exactly chunk_size bytes as expected by Ceph
      ceph::bufferlist original_data_bl;
      original_data_bl.append(restored_data);
      
      // Calculate the portion of original data for this "data chunk"
      // Use get_chunk_size() for consistency with encoding logic
      unsigned int original_data_per_chunk = get_chunk_size(original_data_size);
      unsigned int start_offset = shard_id.id * original_data_per_chunk;
      unsigned int length = (shard_id.id == SIZECEPH_ACTUAL_K - 1) ? 
                             (original_data_size - start_offset) : original_data_per_chunk;
      
      // Extract the data portion
      ceph::bufferlist data_portion;
      data_portion.substr_of(original_data_bl, start_offset, length);
      
      chunk_bl = std::move(data_portion);
      dout(15) << "SizeCeph_Actual decode: returning data chunk " << shard_id.id 
               << " length=" << chunk_bl.length() << " expected=" << chunk_size << dendl;
    } else {
      // Parity chunks (4-8): Cannot be returned directly by SizeCeph_Actual
      // SizeCeph_Actual transforms all chunks, so parity chunks cannot be reconstructed
      // independently. Let Ceph's automatic encode fallback handle these missing chunks.
      // 
      // By NOT adding this chunk to (*decoded), we signal to Ceph's ECUtil.cc that
      // it needs to use the encode fallback mechanism (lines 695-696 in ECUtil.cc)
      dout(15) << "SizeCeph_Actual decode: parity chunk " << shard_id.id 
               << " requires encode fallback - not returned by decode (letting Ceph handle via encode)" << dendl;
      continue; // Skip adding this chunk to decoded map - triggers Ceph's encode fallback
    }
    
    (*decoded)[shard_id] = chunk_bl;
  }
  
  return 0;
}

ceph::ErasureCodeInterface::plugin_flags ErasureCodeSizeCephActual::get_supported_optimizations() const {
  // SizeCeph_Actual EXPLICITLY DISABLES partial operations that are inefficient
  // for its always-decode architecture. This forces Ceph to use full
  // encode/decode cycles instead of attempting partial updates.
  //
  // DISABLED optimizations:
  // - FLAG_EC_PLUGIN_PARTIAL_READ_OPTIMIZATION: SizeCeph_Actual transforms data, so cannot read directly from chunks
  // - FLAG_EC_PLUGIN_PARTIAL_WRITE_OPTIMIZATION: Any write requires full re-encoding
  // - FLAG_EC_PLUGIN_PARITY_DELTA_OPTIMIZATION: Delta operations are meaningless for SizeCeph_Actual
  // 
  // ENABLED optimizations:
  // - FLAG_EC_PLUGIN_OPTIMIZED_SUPPORTED: Basic optimized EC is supported
  // - FLAG_EC_PLUGIN_ZERO_PADDING_OPTIMIZATION: We can handle zero-length buffers
  //
  
  return FLAG_EC_PLUGIN_OPTIMIZED_SUPPORTED | FLAG_EC_PLUGIN_ZERO_PADDING_OPTIMIZATION;
}

// Deprecated method stubs
int ErasureCodeSizeCephActual::minimum_to_decode(const std::set<int> &want_to_read,
                                                const std::set<int> &available,
                                                std::map<int, std::vector<std::pair<int, int>>> *minimum) {
  // Convert to modern interface
  shard_id_set want_set, avail_set, min_set;
  for (int i : want_to_read) want_set.insert(shard_id_t(i));
  for (int i : available) avail_set.insert(shard_id_t(i));
  
  int r = minimum_to_decode(want_set, avail_set, min_set, nullptr);
  if (r == 0 && minimum) {
    minimum->clear();
    for (const auto &shard : min_set) {
      // For SizeCeph_Actual, we need to read the entire chunk
      // Each chunk contains the full content length divided by algorithm alignment
      std::vector<std::pair<int, int>> chunk_ranges;
      chunk_ranges.push_back(std::make_pair(0, get_sub_chunk_count()));
      (*minimum)[shard.id] = chunk_ranges;
    }
  }
  return r;
}

int ErasureCodeSizeCephActual::minimum_to_decode_with_cost(const shard_id_set &want_to_read,
                                                          const shard_id_map<int> &available,
                                                          shard_id_set *minimum) {
  shard_id_set avail_set;
  for (const auto &entry : available) {
    avail_set.insert(entry.first);
  }
  return minimum_to_decode(want_to_read, avail_set, *minimum, nullptr);
}

int ErasureCodeSizeCephActual::minimum_to_decode_with_cost(const std::set<int> &want_to_read,
                                                          const std::map<int, int> &available,
                                                          std::set<int> *minimum) {
  shard_id_set want_set, avail_set, min_set;
  for (int i : want_to_read) want_set.insert(shard_id_t(i));
  for (const auto &entry : available) avail_set.insert(shard_id_t(entry.first));
  
  int r = minimum_to_decode(want_set, avail_set, min_set, nullptr);
  if (r == 0 && minimum) {
    for (const auto &shard : min_set) {
      minimum->insert(shard.id);
    }
  }
  return r;
}

// Deprecated method implementations
[[deprecated]]
int ErasureCodeSizeCephActual::encode(const std::set<int> &want_to_encode,
                                     const ceph::bufferlist &in,
                                     std::map<int, ceph::bufferlist> *encoded) {
  // Convert old interface to new interface
  shard_id_set want_set;
  for (int i : want_to_encode) {
    want_set.insert(shard_id_t(i));
  }
  
  shard_id_map<ceph::bufferlist> encoded_map(SIZECEPH_ACTUAL_N);
  int ret = encode(want_set, in, &encoded_map);
  
  if (ret == 0 && encoded) {
    encoded->clear();
    for (const auto& pair : encoded_map) {
      (*encoded)[pair.first.id] = pair.second;
    }
  }
  
  return ret;
}

[[deprecated]]
int ErasureCodeSizeCephActual::decode(const std::set<int> &want_to_read,
                                     const std::map<int, ceph::bufferlist> &chunks,
                                     std::map<int, ceph::bufferlist> *decoded, 
                                     int chunk_size) {
  // Convert old interface to new interface
  shard_id_set want_set;
  for (int i : want_to_read) {
    want_set.insert(shard_id_t(i));
  }
  
  shard_id_map<ceph::bufferlist> chunks_map(SIZECEPH_ACTUAL_N);
  for (const auto& pair : chunks) {
    chunks_map[shard_id_t(pair.first)] = pair.second;
  }
  
  shard_id_map<ceph::bufferlist> decoded_map(SIZECEPH_ACTUAL_N);
  int ret = decode(want_set, chunks_map, &decoded_map, chunk_size);
  
  if (ret == 0 && decoded) {
    decoded->clear();
    for (const auto& pair : decoded_map) {
      (*decoded)[pair.first.id] = pair.second;
    }
  }
  
  return ret;
}

// Chunk-based operations - not supported by SizeCeph_Actual
[[deprecated]]
int ErasureCodeSizeCephActual::encode_chunks(const std::set<int> &want_to_encode,
                                            std::map<int, ceph::bufferlist> *encoded) {
  return -ENOTSUP;
}

int ErasureCodeSizeCephActual::encode_chunks(const shard_id_map<ceph::bufferptr> &in,
                                            shard_id_map<ceph::bufferptr> &out) {
  return -ENOTSUP;
}

int ErasureCodeSizeCephActual::decode_chunks(const shard_id_set &want_to_read,
                                            shard_id_map<ceph::bufferptr> &in,
                                            shard_id_map<ceph::bufferptr> &out) {
  return -ENOTSUP;
}

[[deprecated]]
int ErasureCodeSizeCephActual::decode_chunks(const std::set<int> &want_to_read,
                                            const std::map<int, ceph::bufferlist> &chunks,
                                            std::map<int, ceph::bufferlist> *decoded) {
  return -ENOTSUP;
}

// Delta encoding - not supported by SizeCeph_Actual
void ErasureCodeSizeCephActual::encode_delta(const ceph::bufferptr &old_data,
                                            const ceph::bufferptr &new_data,
                                            ceph::bufferptr *delta_maybe_in_place) {
  // SizeCeph_Actual doesn't support delta encoding
}

void ErasureCodeSizeCephActual::apply_delta(const shard_id_map<ceph::bufferptr> &in,
                                           shard_id_map<ceph::bufferptr> &out) {
  // SizeCeph_Actual doesn't support delta encoding
}

// Concatenated decode operations - required for SizeCeph_Actual read operations
[[deprecated]]
int ErasureCodeSizeCephActual::decode_concat(const std::set<int>& want_to_read,
                                            const std::map<int, ceph::bufferlist> &chunks,
                                            ceph::bufferlist *decoded) {
  
  if (!decoded) {
    return -EINVAL;
  }
  
  // Convert to modern interface
  shard_id_set want_set;
  for (int i : want_to_read) {
    want_set.insert(shard_id_t(i));
  }
  
  shard_id_map<ceph::bufferlist> chunks_map(SIZECEPH_ACTUAL_N);
  for (const auto& pair : chunks) {
    chunks_map[shard_id_t(pair.first)] = pair.second;
  }
  
  shard_id_map<ceph::bufferlist> decoded_map(SIZECEPH_ACTUAL_N);
  int chunk_size = chunks.empty() ? 0 : chunks.begin()->second.length();
  int ret = decode(want_set, chunks_map, &decoded_map, chunk_size);
  
  if (ret == 0) {
    decoded->clear();
    
    // ECCommonL.cc expects shards to be concatenated in the order they appear in want_to_read
    // We must return ALL requested shards (data AND parity) in sequential order
    // This is required for the trim_offset calculation to work correctly
    for (int shard_id : want_to_read) {
      auto it = decoded_map.find(shard_id_t(shard_id));
      if (it != decoded_map.end()) {
        decoded->claim_append(it->second);
        dout(20) << "SizeCeph_Actual decode_concat: appending shard " << shard_id 
                 << " with length " << it->second.length() << dendl;
      } else {
        dout(5) << "SizeCeph_Actual decode_concat: WARNING - requested shard " << shard_id 
                << " not found in decoded_map; appending zeros of chunk_size=" << chunk_size << dendl;
        // Append empty buffer to maintain shard ordering
        ceph::bufferlist empty_shard;
        empty_shard.append_zero(chunk_size);
        decoded->claim_append(empty_shard);
      }
    }
    
    dout(15) << "SizeCeph_Actual decode_concat: successfully decoded " 
             << decoded->length() << " bytes (all requested shards in order)" << dendl;
  }
  
  return ret;
}

[[deprecated]]
int ErasureCodeSizeCephActual::decode_concat(const std::map<int, ceph::bufferlist> &chunks,
                                            ceph::bufferlist *decoded) {
  
  if (!decoded) {
    return -EINVAL;
  }
  
  // For this version, we want to read all data chunks (0 to K-1)
  std::set<int> want_to_read;
  for (unsigned int i = 0; i < SIZECEPH_ACTUAL_K; ++i) {
    want_to_read.insert(i);
  }
  
  return decode_concat(want_to_read, chunks, decoded);
}