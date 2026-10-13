// Both preparation and its validation record depend on the maintained engine
// data, not just the top-level packer. This hashes inputs only; semantic asset
// validation still runs once when an image is produced.
import fs from 'node:fs';
import { createHash } from 'node:crypto';

export const ASSET_INPUTS = Object.freeze([
  'assets-identity.mjs', 'assets-pack.mjs', 'sprite-metadata.mjs', 'sprite-metrics.mjs',
  'renderer-tables.mjs', '../engine/info.c', '../engine/info.h',
  '../engine/tables.c', '../engine/tables.h', '../engine/r_main.c',
  '../engine/r_main.h', '../engine/r_things.c', '../engine/r_data.c', '../engine/r_defs.h',
  '../engine/doomdef.h', '../engine/m_fixed.h', '../engine/m_fixed.c',
  '../src/fixed_fast.asm', '../engine/i_video.h',
]);
export const VALIDATION_INPUTS = Object.freeze([
  ...ASSET_INPUTS, 'assets-validate.mjs', 'renderer-tables-validate.mjs',
]);

function fingerprint(files) {
  const hash = createHash('sha256');
  for (const file of files) {
    const bytes = fs.readFileSync(new URL(file, import.meta.url));
    hash.update(file + '\0' + bytes.length + '\0');
    hash.update(bytes);
  }
  return hash.digest('hex');
}
export const assetGeneratorHash = () => fingerprint(ASSET_INPUTS);
export const assetValidatorHash = () => fingerprint(VALIDATION_INPUTS);
