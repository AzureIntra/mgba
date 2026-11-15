/* PKLink: fast-forward's sound at the game's own pitch, by time-stretching (pklink-tempo.h).
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "pklink-tempo.h"

#include <math.h>
#include <string.h>

#define PKLINK_TEMPO_FRAME_BYTES (2 * sizeof(int16_t))
// The frames each grain adds to the output: its whole, less the crossfade the next one shares.
#define PKLINK_TEMPO_STEP (PKLINK_TEMPO_SEQUENCE - PKLINK_TEMPO_OVERLAP)

// `speed` kept from 1 to PKLINK_TEMPO_MOST_SPEED.
static double pklinkTempoClamp(double speed) {
	if (!(speed >= 1.0)) {
		return 1.0;
	}
	return speed > PKLINK_TEMPO_MOST_SPEED ? PKLINK_TEMPO_MOST_SPEED : speed;
}

// Drops the oldest `count` frames waiting (all of them, at most).
static void pklinkTempoDrop(struct PkLinkTempo* tempo, size_t count) {
	if (count >= tempo->inputFrames) {
		tempo->inputFrames = 0;
		return;
	}
	tempo->inputFrames -= count;
	memmove(tempo->input, &tempo->input[count * 2], tempo->inputFrames * PKLINK_TEMPO_FRAME_BYTES);
}

void pklinkTempoReset(struct PkLinkTempo* tempo) {
	tempo->inputFrames = 0;
	tempo->outputFrames = 0;
	tempo->hasTail = false;
	tempo->skipFraction = 0;
}

size_t pklinkTempoPush(struct PkLinkTempo* tempo, const int16_t* frames, size_t count) {
	size_t dropped = 0;
	if (count > PKLINK_TEMPO_INPUT_CAPACITY) {
		dropped += count - PKLINK_TEMPO_INPUT_CAPACITY;
		frames += dropped * 2;
		count = PKLINK_TEMPO_INPUT_CAPACITY;
	}
	size_t room = PKLINK_TEMPO_INPUT_CAPACITY - tempo->inputFrames;
	if (count > room) {
		dropped += count - room;
		pklinkTempoDrop(tempo, count - room);
	}
	memcpy(&tempo->input[tempo->inputFrames * 2], frames, count * PKLINK_TEMPO_FRAME_BYTES);
	tempo->inputFrames += count;
	return dropped;
}

size_t pklinkTempoKeepLatest(struct PkLinkTempo* tempo, size_t frames) {
	if (tempo->inputFrames <= frames) {
		return 0;
	}
	size_t dropped = tempo->inputFrames - frames;
	pklinkTempoDrop(tempo, dropped);
	return dropped;
}

// The frames a grain needs waiting: room to move it, and the tempo's skip past it.
static size_t pklinkTempoGrainInput(double speed) {
	size_t grain = PKLINK_TEMPO_SEEK + PKLINK_TEMPO_SEQUENCE;
	size_t skip = (size_t) ceil(pklinkTempoClamp(speed) * PKLINK_TEMPO_STEP) + 1;
	return grain > skip ? grain : skip;
}

size_t pklinkTempoNeededInput(size_t count, double speed) {
	size_t grains = (count + PKLINK_TEMPO_STEP - 1) / PKLINK_TEMPO_STEP;
	return pklinkTempoGrainInput(speed) + grains * (size_t) ceil(pklinkTempoClamp(speed) * PKLINK_TEMPO_STEP);
}

// Where, from the tempo's point up to PKLINK_TEMPO_SEEK frames on, the next grain's start best
// matches the last grain's end: the most correlation with it, over the candidate's own energy, on
// the left and right summed, every other frame.
static size_t pklinkTempoBestStart(const struct PkLinkTempo* tempo) {
	if (!tempo->hasTail) {
		return 0;
	}
	size_t best = 0;
	double bestScore = -INFINITY;
	for (size_t start = 0; start < PKLINK_TEMPO_SEEK; ++start) {
		const int16_t* candidate = &tempo->input[start * 2];
		double correlation = 0;
		double energy = 0;
		for (size_t frame = 0; frame < PKLINK_TEMPO_OVERLAP; frame += 2) {
			double last = (double) tempo->tail[frame * 2] + tempo->tail[frame * 2 + 1];
			double next = (double) candidate[frame * 2] + candidate[frame * 2 + 1];
			correlation += last * next;
			energy += next * next;
		}
		double score = correlation / sqrt(energy + 1.0);
		if (score > bestScore) {
			bestScore = score;
			best = start;
		}
	}
	return best;
}

// Adds one grain to the output and moves the tempo's point on; false while too little waits, or
// the output has no room for it.
static bool pklinkTempoStep(struct PkLinkTempo* tempo, double speed) {
	if (tempo->inputFrames < pklinkTempoGrainInput(speed) ||
	    tempo->outputFrames + PKLINK_TEMPO_STEP > PKLINK_TEMPO_OUTPUT_CAPACITY) {
		return false;
	}
	const int16_t* grain = &tempo->input[pklinkTempoBestStart(tempo) * 2];
	int16_t* out = &tempo->output[tempo->outputFrames * 2];
	for (size_t frame = 0; frame < PKLINK_TEMPO_OVERLAP; ++frame) {
		for (size_t channel = 0; channel < 2; ++channel) {
			int32_t next = grain[frame * 2 + channel];
			if (tempo->hasTail) {
				int32_t last = tempo->tail[frame * 2 + channel];
				next = (last * (int32_t) (PKLINK_TEMPO_OVERLAP - frame) + next * (int32_t) frame) /
				    (int32_t) PKLINK_TEMPO_OVERLAP;
			}
			out[frame * 2 + channel] = (int16_t) next;
		}
	}
	memcpy(&out[PKLINK_TEMPO_OVERLAP * 2], &grain[PKLINK_TEMPO_OVERLAP * 2],
	       (PKLINK_TEMPO_SEQUENCE - 2 * PKLINK_TEMPO_OVERLAP) * PKLINK_TEMPO_FRAME_BYTES);
	memcpy(tempo->tail, &grain[PKLINK_TEMPO_STEP * 2], PKLINK_TEMPO_OVERLAP * PKLINK_TEMPO_FRAME_BYTES);
	tempo->hasTail = true;
	tempo->outputFrames += PKLINK_TEMPO_STEP;
	double skip = pklinkTempoClamp(speed) * PKLINK_TEMPO_STEP + tempo->skipFraction;
	size_t whole = (size_t) skip;
	tempo->skipFraction = skip - (double) whole;
	pklinkTempoDrop(tempo, whole);
	return true;
}

size_t pklinkTempoPull(struct PkLinkTempo* tempo, int16_t* out, size_t count, double speed) {
	while (tempo->outputFrames < count && pklinkTempoStep(tempo, speed)) {
	}
	size_t written = count < tempo->outputFrames ? count : tempo->outputFrames;
	memcpy(out, tempo->output, written * PKLINK_TEMPO_FRAME_BYTES);
	tempo->outputFrames -= written;
	memmove(tempo->output, &tempo->output[written * 2], tempo->outputFrames * PKLINK_TEMPO_FRAME_BYTES);
	return written;
}
