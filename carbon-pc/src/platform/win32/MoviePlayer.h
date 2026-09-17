// The startup movie (MoviePlayer_Play @ 0x710009bf50, called at the top of Graphics_InitEGL @ 0x710009b540).
#pragma once
#include <string>

namespace movie_player {

// Plays a movie from a mounted path into the canvas and blocks until it has finished. Nothing can skip it. A
// missing file is ignored, as in the original; a window close stops playback early and stays pending.
void Play(const std::string& path);

}  // namespace movie_player
