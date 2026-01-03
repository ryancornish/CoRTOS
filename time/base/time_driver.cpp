/**
* @file time_driver.cpp
* @brief TimeDriver static member definitions
*/

#include "cortos/time_driver.hpp"

namespace cortos
{

// Definition of static singleton instance
ITimeDriver* ITimeDriver::instance = nullptr;

} // namespace cortos
