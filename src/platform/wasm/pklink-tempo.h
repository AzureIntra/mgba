/* PKLink: fast-forward's sound at the game's own pitch.
 *
 * Fast-forward runs the game, and its sound, several times faster. Played back faster, the sound
 * goes up in pitch; this plays it at its own pitch instead, faster in tempo, by time-stretching:
 * the sound is cut into grains of 40 ms, played whole, and each grain starts at the point of the
 * sound its tempo has reached, moved by up to 15 ms to where it best matches the end of the last
 * grain, and crossfades from it over 8 ms (WSOLA, the overlap-add method of SoundTouch). Grains
 * of the sound the tempo skips over are never played.
 *
 * Frames are stereo, interleaved 16-bit (left, right), at the output's rate. pklinkTempo* is the
 * stretch alone, with no mGBA headers, so a host test can build it; pklinkFastForwardAudio, in
 * pklink.c, feeds it from the core in the SDL audio callback (sdl-audio.c).
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef PKLINK_TEMPO_H
#define PKLINK_TEMPO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Each grain, played whole: 40 ms at 48 kHz.
#define PKLINK_TEMPO_SEQUENCE 1920
// The crossfade from one grain to the next: 8 ms.
#define PKLINK_TEMPO_OVERLAP 384
// How far past the tempo's point a grain may start, to match the last one: 15 ms.
#define PKLINK_TEMPO_SEEK 720
// The sound waiting to be stretched, at most: 1.4 s at 48 kHz.
#define PKLINK_TEMPO_INPUT_CAPACITY 65536
// The stretched sound waiting to be played, at most.
#define PKLINK_TEMPO_OUTPUT_CAPACITY 8192
// The fastest tempo the stretch takes: a faster one is played at this one.
#define PKLINK_TEMPO_MOST_SPEED 16.0

struct PkLinkTempo {
	int16_t input[PKLINK_TEMPO_INPUT_CAPACITY * 2];
	size_t inputFrames;
	int16_t output[PKLINK_TEMPO_OUTPUT_CAPACITY * 2];
	size_t outputFrames;
	// The last grain's end, which the next grain crossfades from.
	int16_t tail[PKLINK_TEMPO_OVERLAP * 2];
	bool hasTail;
	// The part of a frame the tempo's point has moved past the last whole one.
	double skipFraction;
};

// Empties the stretch, as at the start of a fast-forward.
void pklinkTempoReset(struct PkLinkTempo* tempo);

// Adds `count` frames of sound; with no room for them, the oldest waiting are dropped first (and
// with more than the stretch holds, only the latest `PKLINK_TEMPO_INPUT_CAPACITY`). Returns the
// frames dropped.
size_t pklinkTempoPush(struct PkLinkTempo* tempo, const int16_t* frames, size_t count);

// Drops the oldest sound waiting past `frames`, so the sound never lags the game by more. Returns
// the frames dropped.
size_t pklinkTempoKeepLatest(struct PkLinkTempo* tempo, size_t frames);

// The sound a pull of `count` frames at `speed` may need waiting: the grains it plays, and the
// tempo's skip past each.
size_t pklinkTempoNeededInput(size_t count, double speed);

// Writes up to `count` frames of the sound at `speed` (1 or more: how much faster the game runs)
// to `out`, stretching what waits. Returns the frames written: fewer while too little waits.
size_t pklinkTempoPull(struct PkLinkTempo* tempo, int16_t* out, size_t count, double speed);

struct mAudioBuffer;

// Fills `frames` stereo frames of `out`, at `rate`, with the core's sound from `source` (at
// `sourceRate`) at its own pitch, `speed` times faster, and silence past what it has. Defined in
// pklink.c; the SDL audio callback calls it while the game fast-forwards.
void pklinkFastForwardAudio(struct mAudioBuffer* source, unsigned sourceRate, double speed, unsigned rate,
                            int16_t* out, size_t frames);

// The game is back at its own speed: the next fast-forward's sound starts afresh. Defined in
// pklink.c.
void pklinkNormalSpeedAudio(void);

#endif
