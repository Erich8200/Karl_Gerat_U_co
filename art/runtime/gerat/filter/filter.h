#ifndef FILTER_H
#define FILTER_H

#include <mutex>

namespace gerat {
    class Filter
    {
    protected:
        bool flag = false;
        std::once_flag checked;
        virtual void check_flag() = 0;
    public:
        Filter();
        virtual ~Filter();
        bool get_flag();
    };
}



#endif // FILTER_H
