// No fixes: the phone's driver as it is, behind the wrapper core. For testing the core.
#include "wrap.h"

namespace wrap {
const Hook* hooks() {
    static const Hook none[] = {{nullptr, nullptr}};
    return none;
}
}  // namespace wrap
