// Keep device installation in step with both kernels' shared PTY capacity.
import fs from 'node:fs';
const limits = fs.readFileSync(new URL('../ripple/pty-limits.h', import.meta.url), 'utf8');
export const PTY_COUNT = Number(limits.match(/^#define RIPPLE_PTY_COUNT (\d+)$/m)[1]);
export function ptyDevices() {
  return Array.from({length: PTY_COUNT}, (_, i) => [
    {name:`/dev/ptyp${i.toString(16)}`,mode:0o20600,device:0x300+i,data:Buffer.alloc(0)},
    {name:`/dev/ttyp${i.toString(16)}`,mode:0o20600,device:0x202+i,data:Buffer.alloc(0)}
  ]).flat();
}
