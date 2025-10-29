// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// SizeCeph_Actual Erasure Code Plugin Registration

#include "ceph_ver.h"
#include "common/debug.h"
#include "ErasureCodeSizeCephActual.h"
#include "erasure-code/ErasureCodePlugin.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix _prefix(_dout)

static std::ostream& _prefix(std::ostream* _dout)
{
  return *_dout << "ErasureCodePluginSizeCephActual: ";
}

class ErasureCodePluginSizeCephActual : public ceph::ErasureCodePlugin {
public:
  int factory(const std::string& directory,
              ceph::ErasureCodeProfile &profile,
              ceph::ErasureCodeInterfaceRef *erasure_code,
              std::ostream *ss) override {
    
    dout(10) << "SizeCeph_Actual plugin factory: creating production-safe ErasureCodeInterface instance" << dendl;
    
    // Validate configuration for production safety
    if (profile.find("k") != profile.end() && profile["k"] != "4") {
      *ss << "SizeCeph_Actual requires k=4 (fixed configuration)";
      return -EINVAL;
    }
    
    if (profile.find("m") != profile.end() && profile["m"] != "5") {
      *ss << "SizeCeph_Actual requires m=5 (fixed configuration)";
      return -EINVAL;
    }
    
    ErasureCodeSizeCephActual *interface = new ErasureCodeSizeCephActual();
    
    dout(20) << __func__ << ": profile=" << profile << dendl;
    int r = interface->init(profile, ss);
    if (r) {
      delete interface;
      dout(0) << "SizeCeph_Actual plugin factory: init failed with error " << r << dendl;
      return r;
    }
    
    *erasure_code = ceph::ErasureCodeInterfaceRef(interface);
    dout(10) << "SizeCeph_Actual plugin factory: production-safe instance created successfully" << dendl;
    dout(10) << "SizeCeph_Actual configuration: K=4, M=5, MIN_OSDS=6, MAX_FAILURES=3" << dendl;
    return 0;
  }
};

const char *__erasure_code_version() { return CEPH_GIT_NICE_VER; }

int __erasure_code_init(char *plugin_name, char *directory)
{
  auto plugin = new ErasureCodePluginSizeCephActual();
  return ceph::ErasureCodePluginRegistry::instance().add(plugin_name, plugin);
}