#include "unpack_globals.h"

namespace gerat {

    std::set<const void*> dex_addrs;
	std::atomic<int> dex_num;
    std::once_flag filter_inited;
    std::once_flag unpack_mode_file_checked;
    gerat::KarlGeratUFilter* filter;
    bool started = false;

    // bool checkUnpackRequirements()
    // {
    //     gerat::Utilproc util;
    //     if (util.get_apk_dir() != "") {
    //         if (gerat::karl_gerat_u_filter == nullptr) {
    //             gerat::karl_gerat_u_filter = new gerat::KarlGeratUFilter();
    //         }
            
    //         if (gerat::karl_gerat_u_filter->get_flag()) {
    //             // if (gerat::log == nullptr) {
    //             //     gerat::log = new gerat::Log(); // Write thread already started
    //             // }
    //             return true;
    //         }
    //         return false;
    //     } else {
    //         // LOG(art::LogSeverity::INFO) << "Karl Gerat T not started";
    //         return false;
    //     }
    // }

    void init()
    {
        // std::shared_ptr<gerat::KarlGeratUFilter> filter_(new gerat::KarlGeratUFilter);
        // filter = filter_.get();
        filter = new gerat::KarlGeratUFilter;
    }

    void set_started_flag()
    {
        started = true;
    }

}