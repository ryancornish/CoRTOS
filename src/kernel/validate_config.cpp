#include <cortos/config/config.hpp>
#include <cortos/port/port_traits.h>

#include <cstdint>
#include <limits>

namespace cortos
{

/* ============================================================================
 * Configuration validation
 * ========================================================================= */
static_assert(1 <= config::CORES && config::CORES <= CORTOS_PORT_CORE_COUNT,
              "Port does not support configured amount of cores.");
static_assert(config::TIME_CORE_ID < config::CORES,
              "Time core set to non-existent core.");
static_assert(config::MAX_PRIORITIES < std::numeric_limits<uint32_t>::digits,
              "Priorities unsupported by kernel implementation.");

} // namespace cortos
