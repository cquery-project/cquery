#if __cplusplus < 201703L
#include "variant.hpp"
namespace std {
using mpark::get;
using mpark::variant;
}
#else
#include <variant>
#endif
