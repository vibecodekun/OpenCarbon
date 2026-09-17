// Menu audio thread: music + 4 sound effects on an nn::audio AudioRenderer in the original.
// Original: thread entry @ 0x710009fda0 -> MenuAudio_Init @ 0x710009f750 (never returns),
// SFX loading @ 0x710009f530, WAV file loader @ 0x710009f450, RIFF parser @ 0x7100096f20.
//
// Renderer: 32000 Hz, 160-sample frames, 6-channel final mix "MainAudioOut".
//  - music  rom:/assets/sounds/sndMenu.wav (looping), src L->FL, src R->FR at 0.5
//  - sfxBack   sndchange.wav                  src L->FC at 1.0
//  - sfxSelect sndselect.wav                  src L->FC at 1.0
//  - sfxMove   menu_scrub_through_items3.wav  src L->FC at 0.6
//  - sfxToggle menu_toggle_yes_no3.wav        src L->FC at 1.0
// A sound effect only (re)starts if its previous playback has finished. Music fades +-0.01 per 160-sample
// frame towards g::musicFadeInTarget (1.0; 0.7 after returning from a game) / g::musicFadeOutTarget (0.0).
#pragma once

namespace menu_audio {
void Start();  // spawns the audio thread
}
