#ifndef ART_RUNTIME_TWODROID_UTILS_UTILPROC_H_
#define ART_RUNTIME_TWODROID_UTILS_UTILPROC_H_

// #include "twodroid/Constant.h"
// #include "interpreter/interpreter_common.h"

#include <map>
#include <string>
#include <android/log.h>
#include <fstream>
#include <unistd.h>
#include <inttypes.h>
#include <mutex>
#include <sys/stat.h>
#include "UtilProcConstant.h"
#include "../../../runtime/base/logging.h"

#define KARL_GERAT_U_CODE 0x8200666

namespace gerat {

	class Utilproc
	{
	private:
		std::map<u4, std::string> uidMap_;
		u4 lastUid_ = 0xFFFFFFFF; // illegal value -1
	public:
		Utilproc(/* args */);
		~Utilproc();
		std::string	get_apk_dir();
		std::string	get_proc_name();
		bool        init_uidmap();
		// Hash function
		static unsigned int inline BKDRHash(const char* const str, int len)
		{
			const static unsigned int seed = 131; // 31 131 1313 13131 131313 etc..
			unsigned int hash = 0;
			int i = 0;
			while ( i < len )
				hash = hash * seed + str[i++];
			return hash;
		}
	};
	
}

#endif