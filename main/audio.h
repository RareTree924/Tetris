#pragma once
// ============================================================
//  AUDIO — 3-channel mixer out of I2S (44.1 kHz mono).
//  Each channel plays either a WAV streamed from the SD card or a
//  generated square-wave beep. Every channel has its own volume, and
//  one master volume scales the final mix.
//
//  WAV files: 16-bit, MONO, 44100 Hz (other rates play at the wrong
//  speed). Names are relative to SOUND_DIR: play_sound("drop.wav", 1)
//  plays /sdcard/Tetris/drop.wav (D:\Tetris\drop.wav on the PC).
// ============================================================
#include <stdbool.h>

#define SOUND_DIR "/sdcard/Tetris"

#define AUDIO_MAX_CHANNELS 3

typedef int sound_handle_t;
#define SOUND_HANDLE_INVALID (-1)

// Mounts the SD card at /sdcard. Call once at boot, before play_sound().
// Returns false if there's no card (beeps still work without one).
bool sd_begin(void);

// Starts I2S and the mixer task. Call once at boot.
void audio_begin(void);

// Plays a WAV on a free channel. loops: 1 = once, N = N times, 0 = forever
// (stop it with stop_sound()). Returns SOUND_HANDLE_INVALID if the file is
// missing/unsupported or all channels are busy.
sound_handle_t play_sound(const char *filename, int loops);

// Plays a square-wave beep: freq_hz for duration_ms, at volume 0.0-1.0.
sound_handle_t play_tone(int freq_hz, int duration_ms, float volume);

void stop_sound(sound_handle_t handle);
bool sound_playing(sound_handle_t handle);
void set_sound_volume(sound_handle_t handle, float volume);   // 0.0 - 1.0
void set_master_volume(float volume);                         // 0.0 - 1.0
