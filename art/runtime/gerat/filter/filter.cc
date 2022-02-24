#include "filter.h"

namespace gerat {

    Filter::Filter()
    {

    }

    Filter::~Filter()
    {

    }

    bool Filter::get_flag()
    {
        // std::call_once(checked, std::bind(&Filter::check_flag, this));
        check_flag();
        return flag;
    }

}
