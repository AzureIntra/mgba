/* PKLink memory export: Module.pklinkRead and Module.pklinkWrite over pklink.c.
 *
 * Emscripten runs this --pre-js inside the module factory, where Module, cwrap and HEAPU8 are in
 * scope. Call both from the core's videoFrameEndedCallback, while the emulator thread waits.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// One heap buffer, allocated on first use and kept for the module's life: one call moves at most
// 8 KiB, and the caller splits anything longer.
const PKLINK_SCRATCH_BYTES = 8192;
let pklinkScratch = 0;
let pklinkReadC = null;
let pklinkWriteC = null;

// The wasm exports exist only once the module is instantiated, so look them up on first call.
const pklinkFunctions = () => {
  if (pklinkReadC === null) {
    pklinkReadC = cwrap("pklinkRead", "number", ["number", "number", "number"]);
    pklinkWriteC = cwrap("pklinkWrite", "number", ["number", "number", "number"]);
  }
};

const pklinkScratchFor = (length) => {
  if (!Number.isInteger(length) || length < 0 || length > PKLINK_SCRATCH_BYTES) {
    throw new RangeError(`PKLink transfers 0 to ${PKLINK_SCRATCH_BYTES} bytes, not ${length}`);
  }
  if (pklinkScratch === 0) {
    pklinkScratch = Module._malloc(PKLINK_SCRATCH_BYTES);
    if (pklinkScratch === 0) {
      throw new Error("PKLink could not allocate its scratch buffer");
    }
  }
  return pklinkScratch;
};

// The C side takes a uint32_t, and the call would wrap any other number (0x102000000 included)
// into one, so anything but an integer in 0..0xFFFFFFFF is refused here.
const pklinkCheckAddress = (name, address) => {
  if (!Number.isInteger(address) || address < 0 || address > 0xffffffff) {
    throw new RangeError(`${name} takes a 32-bit address, not ${address}`);
  }
};

// Reads come from EWRAM or IWRAM (the game's save-block pointers live there); writes go to EWRAM.
const PKLINK_READABLE = "EWRAM (0x02000000-0x0203ffff) and IWRAM (0x03000000-0x03007fff),";
const PKLINK_WRITABLE = "EWRAM (0x02000000-0x0203ffff)";

const pklinkRefusal = (name, regions, address, length) =>
  new RangeError(
    `${name} refused 0x${address.toString(16)} + ${length}: outside ${regions} or no game loaded`,
  );

// Returns a copy of the bytes, not a view of the shared heap: the caller keeps them after the
// next call.
Module.pklinkRead = (address, length) => {
  pklinkCheckAddress("pklinkRead", address);
  const scratch = pklinkScratchFor(length);
  pklinkFunctions();
  if (pklinkReadC(address, scratch, length) !== length) {
    throw pklinkRefusal("pklinkRead", PKLINK_READABLE, address, length);
  }
  return HEAPU8.slice(scratch, scratch + length);
};

Module.pklinkWrite = (address, bytes) => {
  pklinkCheckAddress("pklinkWrite", address);
  const scratch = pklinkScratchFor(bytes.length);
  pklinkFunctions();
  HEAPU8.set(bytes, scratch);
  if (pklinkWriteC(address, scratch, bytes.length) !== bytes.length) {
    throw pklinkRefusal("pklinkWrite", PKLINK_WRITABLE, address, bytes.length);
  }
};
