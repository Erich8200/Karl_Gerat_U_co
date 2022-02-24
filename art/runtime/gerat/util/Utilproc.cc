#include "Utilproc.h"

namespace gerat {

	Utilproc::Utilproc(/* args */)
	{

	}
	
	Utilproc::~Utilproc()
	{

	}

	bool Utilproc::init_uidmap()
	{
		std::ifstream f ("/data/system/packages.list");
		if ( !f ) {
			return false;	
		}
		
		// read a line and get path
		std::string s;
		std::string dir;
		u4 d;
		u4 e;
		std::string attr;
		std::string ids;

		while ( !f.eof() )
		{
			f >> s >> d >> e >> dir >> attr >> ids;
			uidMap_[d] = dir;
		}

		f.close();
		return true;	
	}

	std::string Utilproc::get_proc_name()
	{
		// query other process is forbidden
		u4 pid = getpid();
		std::string s;
		// Get procFile's name from pid
		char name[ProcFileNameMaxLen];
		
		snprintf( name,	ProcFileNameMaxLen, "/proc/%d/cmdline", pid );
		std::ifstream f( name );
		if ( f )
		{
			std::getline(f, s);
			for (size_t i = 0; i < s.size(); i++)
			{
				// if (!((s[i] >= 'a' && s[i] <= 'z') || s[i] == '.' || (s[i] >= '0' && s[i] <= '9') || (s[i] == '_'))) {
				if (!(isprint(s[i]) > 0 && s[i] != ' ')) {
					// end_index = i;
					// line[i] = 0;
					s = s.substr(0, i);
					break;
				}
			}
			f.close();
		}
		return s;
	}

	std::string Utilproc::get_apk_dir()
	{
		// // don't allow to query other process' uid
		// u4 uid = getuid();
		// if (uid != lastUid_)
		// {
		// 	LOG(art::LogSeverity::INFO) << "Karl gerat Utilproc::get_apk_dir: uid changed";
		// 	if (!init_uidmap()) {
		// 		LOG(art::LogSeverity::ERROR) << "Karl gerat Utilproc::get_apk_dir: /data/system/packages.list open failed";
		// 		return std::string("");
		// 	}
		// 	lastUid_ = uid;
		// }

		// std::map<u4, std::string>::const_iterator it = uidMap_.find(uid);

		// if ( it != uidMap_.end() )
		// 	return it->second; 

		// LOG(art::LogSeverity::ERROR) << "Karl gerat Utilproc::get_apk_dir: uidMap find failed, probably not an APK process?";
		// return std::string(""); // 不是APK进程

		std::string apk_dir_guess = std::string("/data/data/") + get_proc_name();
		struct stat buf;
		if (stat(apk_dir_guess.c_str(), &buf) == 0) {
			return apk_dir_guess;   // Find APK dir by guessing
		} else {
			return std::string(""); // Could not find APK dir
		}
	}

}
