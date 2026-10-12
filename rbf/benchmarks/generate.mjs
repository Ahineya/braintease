#!/usr/bin/env node
// Expand explicit code padding; the assembler has no instruction-repeat directive.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

if (process.argv.length !== 3) {
  throw new Error('usage: node rbf/benchmarks/generate.mjs OUTPUT_DIRECTORY');
}
const output = resolve(process.argv[2]);
mkdirSync(output, { recursive: true });
for (const name of ['late-code', 'far-ram', 'bank-jumps']) {
  const path = fileURLToPath(new URL(`${name}.asm`, import.meta.url));
  const template = readFileSync(path, 'utf8');
  const source = template.replace(/^; BENCH_PADDING (\d+) (\d+)$/gm, (_, banks, size) => {
    const count = Number(banks);
    const length = Number(size);
    if (count > 16 || length > 1024) throw new Error('excessive benchmark padding');
    return Array.from({ length: count }, (_, index) =>
      `benchmark_padding_${index}:\n${'NOP\n'.repeat(length)}`).join('');
  });
  writeFileSync(resolve(output, `${name}.asm`), source);
}
