#include "karlgeratufilter.h"

namespace gerat {
    
    // std::mutex kgu_filter_init_mutex;
    // KarlGeratUFilter* karl_gerat_u_filter = nullptr;

    KarlGeratUFilter::KarlGeratUFilter()
    {

    }

    void KarlGeratUFilter::check_flag()
    {
        // Already checked
        if (this->flag)
            return;

        // Utilproc may be wrong, either
        gerat::Utilproc util;
        std::string config_file = util.get_apk_dir();
        config_file += std::string("/unpack.txt");
        
        std::ifstream fin(config_file);
        if (fin) {
            // LOG(art::LogSeverity::INFO) << "Karl Gerat check_flag config file name: " << config_file  << " length:" << config_file.size();
            // LOG(art::LogSeverity::INFO) << "length:" << config_file.size();
            fin >> component_name;
            if (!component_name.empty()) {
                this->flag = true;
                fin >> dump_method_str;
                // level1:"dump"/any other string except the ones in level2 or level3
                // level2:"force-dump" 
                // level3:"force-rebuild"
                if (!dump_method_str.empty() && (dump_method_str == "force-dump" || dump_method_str == "force-rebuild")) {
                    force_init_flag = true;
                    if (dump_method_str == "force-rebuild") {
                        this->rebuild_flag = true;
                    } else {
                        this->rebuild_flag = false;
                    }
                    fin >> mode_str;
                    if (!mode_str.empty() && mode_str == "black") {
                        mode = UnpackMode::black;
                    } else if (!mode_str.empty() && mode_str == "white") {
                        mode = UnpackMode::white;
                    } else { // default
                        mode = UnpackMode::black;
                    }
                    return;
                } else
                this->force_init_flag = false;
                return;
            }
            fin.close();
        }
        this->force_init_flag = false;
        this->flag = false;
    }

    std::string KarlGeratUFilter::get_component_name()
    {
        return component_name;
    }

    bool KarlGeratUFilter::get_force_init_flag()
    {
        return force_init_flag;
    }

    bool KarlGeratUFilter::get_rebuild_flag()
    {
        return rebuild_flag;
    }

    bool KarlGeratUFilter::check_in_black_list(std::string& name)
    {
        if (mode == UnpackMode::white) { // wrong mode
            return true;
        }
        if (mode == UnpackMode::black && black_list.find(name) == black_list.end()){
            return false;
        }
        return true;
    }

    bool KarlGeratUFilter::check_in_white_list(std::string& name)
    {
        if (mode == UnpackMode::black) { // wrong mode
            return false;
        }
        if (mode == UnpackMode::white && white_list.find(name) == white_list.end()) {
            return false;
        }
        return true;
    }

    bool KarlGeratUFilter::should_process(std::string& name)
    {
        if (mode == UnpackMode::black) {
            if (check_in_black_list(name))
                return true;
            else 
                return false;
        } else if (mode == UnpackMode::white) {
            if (check_in_white_list(name))
                return false;
            else
                return true;
        }
        return false; // Default not process
    }

    bool KarlGeratUFilter::should_initialize(std::string& name)
    {
        for (auto it = bypass_class_list.begin(); it != bypass_class_list.end(); it++) {
		    // Bypass str in name
		    if (name.find(*it) != std::string::npos)
			return false;
	    }
	    return true;
    }

    void KarlGeratUFilter::read_method_name_list()
    {
        gerat::Utilproc util;
        std::string config_file = util.get_apk_dir();
        if (mode == UnpackMode::black) {
            config_file += std::string("/black.txt");
            std::ifstream fin(config_file);
            if (fin) {
                std::string line;
                while(getline(fin, line)) {
                    if(!line.empty())
                        black_list.insert(line);            
                }
                fin.close();
            }
        } else {
            config_file += std::string("/white.txt");
            std::ifstream fin(config_file);
            if (fin) {
                std::string line;
                while(getline(fin, line)) {
                    if(!line.empty())
                        white_list.insert(line);            
                }
                fin.close();
            }
        }
    }

    void KarlGeratUFilter::read_class_name_list()
    {
        gerat::Utilproc util;
        std::string config_file = util.get_apk_dir();
        config_file += std::string("/bypass_class.txt");
        std::ifstream fin(config_file);
        if (fin) {
            std::string line;
            while(getline(fin, line)) {
                if(!line.empty())
                    bypass_class_list.insert(line);            
            }
            fin.close();
        }
    }

    void KarlGeratUFilter::read_list_files()
    {
        read_method_name_list();
        read_class_name_list();
    }

}

