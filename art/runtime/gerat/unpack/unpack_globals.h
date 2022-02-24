#ifndef UNPACK_GLOBALS_H
#define UNPACK_GLOBALS_H
#include <atomic>
#include <mutex>
#include <set>

#include "../filter/karlgeratufilter.h"
#include "../util/Utilproc.h"

namespace gerat{
    extern std::set<const void*> dex_addrs;
    extern std::atomic<int> dex_num;
    extern std::once_flag filter_inited;
    extern std::once_flag unpack_mode_file_checked;
    extern gerat::KarlGeratUFilter* filter;
    extern bool started;

    // bool checkUnpackRequirements();
    void init();
    void set_started_flag();
}

#endif