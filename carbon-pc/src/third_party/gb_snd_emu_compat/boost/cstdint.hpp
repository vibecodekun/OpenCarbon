// Replaces Gb_Snd_Emu's bundled boost/cstdint.hpp, which picks `long` for int32_t whenever long is 32 bits.
// On MSVC that clashes with <cstdint>; on the Switch (LP64) the bundled header picked `int`, as this does.
#ifndef BOOST_CSTDINT_HPP
#define BOOST_CSTDINT_HPP
#include <climits>
#include <cstdint>
#endif
