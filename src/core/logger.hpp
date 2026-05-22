#ifndef MP3PACKERCPP_LOGGER_HPP
#define MP3PACKERCPP_LOGGER_HPP

#include <iostream>

namespace mp3packer {

class Logger {
public:
#ifndef NDEBUG
    static inline bool verbose = true;
#else
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
