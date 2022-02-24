#ifndef KARLGERATTFILTER_H
#define KARLGERATTFILTER_H

#include <fstream>
#include <string>
#include <unordered_set>

#include "filter.h"
#include "../../base/logging.h"
#include "../util/Utilproc.h"

namespace gerat {

    
    class KarlGeratUFilter : public gerat::Filter
    {
    public:
        KarlGeratUFilter();
        std::string get_component_name();
        bool get_force_init_flag();
        bool get_rebuild_flag();
        // Default do not force invoke
        bool should_process(std::string& name); // In black list: unpack In white list: Do not unpack
        bool should_initialize(std::string& name);
        void read_list_files();
    private:
        void read_method_name_list();
        void read_class_name_list();
        enum UnpackMode {black, white};
        void check_flag() override;
        std::string component_name;
        std::string dump_method_str;
        bool force_init_flag;
        bool rebuild_flag;
        std::unordered_set<std::string> black_list;
        std::unordered_set<std::string> white_list;
        std::unordered_set<std::string> bypass_class_list;
        std::string mode_str;
        UnpackMode mode;
        bool check_in_black_list(std::string& name);
        bool check_in_white_list(std::string& name);
    };

    // extern std::mutex kgu_filter_init_mutex;
    // extern KarlGeratUFilter* karl_gerat_u_filter;
}



#endif // KARLGERATTFILTER_H
