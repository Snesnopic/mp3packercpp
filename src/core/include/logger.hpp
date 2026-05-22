#ifndef MP3PACKERCPP_LOGGER_HPP
#define MP3PACKERCPP_LOGGER_HPP

#include <iostream>

namespace mp3packer {

/**
 * @brief Global logger configuration for mp3packercpp.
 */
class Logger {
public:
#ifndef NDEBUG
    /** @brief Toggles debug logging output. Enabled by default in Debug builds. */
    static inline bool verbose = true;
#else
    /** @brief Toggles debug logging output. Disabled by default in Release builds. */
    static inline bool verbose = false;
#endif
};

} // namespace mp3packer

#ifndef NDEBUG
    #define DEBUG_LOG(msg) do { if (mp3packer::Logger::verbose) { std::cout << msg << std::endl; } } while (0)
#else
    #define DEBUG_LOG(msg) do {} while (0)
#endif

#endif // MP3PACKERCPP_LOGGER_HPP
