/* PKLink memory export for the mGBA WebAssembly core.
 *
 * The page reads and writes the game's mailbox through these two functions, once per frame,
 * from the core's videoFrameEnded callback (the emulator thread waits while it runs). Reads may
 * also come from IWRAM, where the game keeps its save-block pointers; writes go to EWRAM only.
 *
 * It also feeds fast-forward's sound to the time-stretch (pklink-tempo.h), for the SDL audio
 * callback.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "main.h"

#include "pklink-tempo.h"

#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>
#include <mgba/core/core.h>
#include <mgba/internal/gba/memory.h>

#include <emscripten.h>
#include <stdint.h>
#include <string.h>

// True when [address, address + length) lies wholly inside [base, base + size).
static bool pklinkIsInside(uint32_t address, int length, uint32_t base, uint32_t size) {
	if (length < 0 || address < base) {
		return false;
	}
	uint32_t offset = address - base;
	return offset <= size && (uint32_t) length <= size - offset;
}

// EWRAM: 0x02000000-0x0203FFFF.
static bool pklinkIsEwramRange(uint32_t address, int length) {
	return pklinkIsInside(address, length, GBA_BASE_EWRAM, GBA_SIZE_EWRAM);
}

// EWRAM, or IWRAM (0x03000000-0x03007FFF): the range lies wholly inside one of the two.
static bool pklinkIsReadableRange(uint32_t address, int length) {
	return pklinkIsEwramRange(address, length) ||
	    pklinkIsInside(address, length, GBA_BASE_IWRAM, GBA_SIZE_IWRAM);
}

// The running GBA core, or NULL when no game is loaded.
static struct mCore* pklinkGbaCore(void) {
	struct mCore* core = pklinkGetCore();
	if (!core || core->platform(core) != mPLATFORM_GBA) {
		return NULL;
	}
	return core;
}

// Copies `length` bytes of EWRAM or IWRAM at `address` into `out`. Returns `length`, or -1 with
// nothing copied when the range leaves both, `out` is NULL or no game is loaded.
EMSCRIPTEN_KEEPALIVE int pklinkRead(uint32_t address, uint8_t* out, int length) {
	struct mCore* core = pklinkGbaCore();
	if (!core || !out || !pklinkIsReadableRange(address, length)) {
		return -1;
	}
	for (int i = 0; i < length; ++i) {
		out[i] = core->busRead8(core, address + i);
	}
	return length;
}

// Copies `length` bytes from `in` into EWRAM at `address`, never IWRAM. Returns `length`, or -1
// with nothing written when the range leaves EWRAM, `in` is NULL or no game is loaded.
EMSCRIPTEN_KEEPALIVE int pklinkWrite(uint32_t address, const uint8_t* in, int length) {
	struct mCore* core = pklinkGbaCore();
	if (!core || !in || !pklinkIsEwramRange(address, length)) {
		return -1;
	}
	for (int i = 0; i < length; ++i) {
		core->busWrite8(core, address + i, in[i]);
	}
	return length;
}

// Fast-forward's sound: the stretch, and the core's sound resampled to the output's rate at its
// own pitch before it is stretched. Only the SDL audio callback touches them, on the page's thread.
static struct PkLinkTempo pklinkTempo;
static struct mAudioBuffer pklinkOwnPitch;
static struct mAudioResampler pklinkOwnPitchResampler;
static bool pklinkIsTempoMade = false;
static bool pklinkIsFastForwarding = false;
#define PKLINK_OWN_PITCH_FRAMES 4096

// Declared in pklink-tempo.h. Each call takes all the sound the game made since the last one,
// since the core's buffer drops what isn't read once it fills, and keeps no more waiting than the
// next call needs, so the sound never lags the game.
void pklinkFastForwardAudio(struct mAudioBuffer* source, unsigned sourceRate, double speed, unsigned rate,
                            int16_t* out, size_t frames) {
	if (!pklinkIsTempoMade) {
		mAudioBufferInit(&pklinkOwnPitch, PKLINK_OWN_PITCH_FRAMES, 2);
		mAudioResamplerInit(&pklinkOwnPitchResampler, mINTERPOLATOR_SINC);
		mAudioResamplerSetDestination(&pklinkOwnPitchResampler, &pklinkOwnPitch, rate);
		pklinkIsTempoMade = true;
	}
	if (!pklinkIsFastForwarding) {
		pklinkTempoReset(&pklinkTempo);
		mAudioBufferClear(&pklinkOwnPitch);
		pklinkIsFastForwarding = true;
	}
	mAudioResamplerSetDestination(&pklinkOwnPitchResampler, &pklinkOwnPitch, rate);
	mAudioResamplerSetSource(&pklinkOwnPitchResampler, source, sourceRate, true);
	int16_t chunk[PKLINK_OWN_PITCH_FRAMES * 2];
	size_t made;
	do {
		made = mAudioResamplerProcess(&pklinkOwnPitchResampler);
		size_t read = mAudioBufferRead(&pklinkOwnPitch, chunk, PKLINK_OWN_PITCH_FRAMES);
		pklinkTempoPush(&pklinkTempo, chunk, read);
	} while (made > 0);
	pklinkTempoKeepLatest(&pklinkTempo, pklinkTempoNeededInput(frames * 2, speed));
	size_t written = pklinkTempoPull(&pklinkTempo, out, frames, speed);
	memset(&out[written * 2], 0, (frames - written) * 2 * sizeof(int16_t));
}

void pklinkNormalSpeedAudio(void) {
	pklinkIsFastForwarding = false;
}
