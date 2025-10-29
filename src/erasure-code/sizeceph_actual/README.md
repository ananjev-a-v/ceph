# SizeCeph_Actual Ceph Erasure Code Plugin

## Overview

The SizeCeph_Actual plugin provides production-safe erasure coding for Ceph using the comprehensive analysis results from testing all 381 possible failure combinations.

## Key Features

### **Production Safety Design**
- **Fixed Configuration**: K=4, M=5 (9 total chunks)
- **Guaranteed Fault Tolerance**: Up to 3 OSD failures (100% reliability)
- **Minimum OSD Requirement**: 6 OSDs required for reads (K+M-3)
- **32-byte Alignment**: Required for optimal performance

### **Based on Comprehensive Analysis**
- **381 Pattern Testing**: Every possible failure combination analyzed
- **100% Reliable Zone**: 1-3 OSD failures (129/129 patterns successful)
- **Avoids Problematic Patterns**: 54 known failing patterns in 4-5 failure scenarios
- **85.8% Overall Success**: 327/381 patterns successful across all scenarios

## Pool Configuration

### Creating a SizeCeph_Actual Pool

```bash
# Create erasure code profile
ceph osd erasure-code-profile set sizeceph_actual_profile \
    plugin=sizeceph_actual \
    k=4 \
    m=5

# Create pool with the profile
ceph osd pool create mypool_sizeceph_actual 32 32 erasure sizeceph_actual_profile

# Enable the pool for RBD (if needed)
ceph osd pool application enable mypool_sizeceph_actual rbd
```

### Pool Parameters

| Parameter | Value | Description |
|-----------|-------|-------------|
| `plugin` | `sizeceph_actual` | Use SizeCeph_Actual erasure coding |
| `k` | `4` | Data chunks (fixed) |
| `m` | `5` | Parity chunks (fixed) |
| Total chunks | `9` | K + M |
| Min OSDs for reads | `6` | K + M - 3 (guaranteed safe) |
| Max failures | `3` | OSDs that can fail simultaneously |

## Cluster Requirements

### **Minimum Cluster Size**
- **9 OSDs minimum** for the pool to be created
- **6 OSDs minimum** for read operations (enforced by plugin)
- **Recommended**: 12+ OSDs for optimal performance and maintenance

### **Network and Hardware**
- **High-speed networking** recommended (10GbE+) due to 9-chunk distribution
- **Balanced OSD placement** across failure domains
- **32-byte aligned storage** for optimal performance

## Usage Examples

### Basic Operations

```bash
# Put an object
rados -p mypool_sizeceph_actual put myobject /path/to/file

# Get an object  
rados -p mypool_sizeceph_actual get myobject /path/to/output

# List objects
rados -p mypool_sizeceph_actual ls

# Object info
rados -p mypool_sizeceph_actual stat myobject
```

### Performance Characteristics

**Optimal Use Cases:**
- Large file storage (>1MB objects)
- Write-once, read-many workloads
- Bulk data operations
- Long-term archival storage

**Suboptimal Use Cases:**
- Small random writes (<64KB)
- Frequent incremental updates
- Partial object modifications
- High-frequency append operations

## Failure Tolerance

### **Guaranteed Recovery Scenarios**
- ✅ **1 OSD failure**: 100% recovery guaranteed
- ✅ **2 OSD failures**: 100% recovery guaranteed  
- ✅ **3 OSD failures**: 100% recovery guaranteed

### **Safety Mechanisms**
- **6-OSD Minimum Enforcement**: Plugin refuses reads with <6 OSDs available
- **Pattern Validation**: Checks against 54 known failing patterns
- **Alignment Verification**: Ensures 32-byte data alignment
- **Library Safety**: Dynamic loading with error handling

## Monitoring and Maintenance

### Key Metrics to Monitor

```bash
# Check OSD status
ceph osd tree

# Pool statistics
ceph osd pool stats mypool_sizeceph_actual

# Check for degraded objects
ceph pg dump_stuck degraded

# Monitor cluster health
ceph health detail
```

### **Critical Alerts**
- Only 6 OSDs available (minimum threshold reached)
- Plugin load failures
- Alignment errors in logs
- Pattern validation failures

## Troubleshooting

### Common Issues

**Plugin Load Failure:**
```bash
# Check if library is installed
ls -la /usr/local/lib/libsizeceph.so

# Check plugin registration
ceph osd erasure-code-profile ls
```

**Insufficient OSDs:**
```
Error: Only X OSDs available, minimum 6 required for safe SizeCeph_Actual decode
```
- **Solution**: Add more OSDs or wait for failed OSDs to recover

**Alignment Errors:**
```
Error: Data size not aligned to 32-byte boundary
```
- **Solution**: Ensure input data is properly aligned

### Performance Optimization

**CRUSH Rule Optimization:**
```bash
# Create custom CRUSH rule for better distribution
ceph osd crush rule create-erasure sizeceph_actual_rule sizeceph_actual_profile
```

**PG Count Tuning:**
```bash
# Calculate optimal PG count for your OSD count
# Rule of thumb: 100-200 PGs per OSD
# For 12 OSDs: ~1200-2400 PGs, round to power of 2
ceph osd pool set mypool_sizeceph_actual pg_num 2048
ceph osd pool set mypool_sizeceph_actual pgp_num 2048
```

## Comparison with Other Erasure Codes

| Feature | SizeCeph_Actual | Reed-Solomon (4+2) | Reed-Solomon (4+5) |
|---------|-----------------|-------------------|-------------------|
| Data chunks | 4 | 4 | 4 |
| Parity chunks | 5 | 2 | 5 |
| Storage overhead | 125% | 50% | 125% |
| Max failures | 3 (guaranteed) | 2 | 5 (theoretical) |
| Min OSDs for reads | 6 | 4 | 4 |
| Reliability | 100% (1-3 failures) | 100% (1-2 failures) | Variable |

## Best Practices

### **Deployment**
1. Start with 12+ OSDs for production
2. Distribute OSDs across multiple failure domains
3. Use high-speed networking (10GbE+)
4. Monitor OSD health continuously
5. Plan maintenance windows carefully

### **Data Management**
1. Use for large objects (>1MB preferred)
2. Avoid frequent small writes
3. Batch operations when possible
4. Monitor storage efficiency
5. Plan for 125% storage overhead

### **Maintenance**
1. Never allow <6 OSDs simultaneously
2. Replace failed OSDs promptly
3. Monitor plugin logs for errors
4. Test recovery procedures regularly
5. Keep sizeceph library updated

## Support and Debugging

### Log Locations
```bash
# OSD logs
/var/log/ceph/ceph-osd.*.log

# Look for SizeCeph_Actual specific messages
grep "ErasureCodeSizeCephActual" /var/log/ceph/ceph-osd.*.log
```

### Debug Settings
```bash
# Enable detailed erasure code logging
ceph tell osd.* config set debug_osd 20
ceph tell osd.* config set debug_erasure 20
```

---

**Note**: This plugin is based on comprehensive analysis of 381 failure patterns and is designed for production safety by operating only within the 100% reliable operational zone of the SizeCeph_Actual algorithm.