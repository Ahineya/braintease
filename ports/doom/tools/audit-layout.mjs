#!/usr/bin/env node
// Inventory target cells and initializer instructions in linked assembly units.
// This is a diagnostic; linker placement/relocation checks remain authoritative.
import fs from 'node:fs';
import { parseArgs } from 'node:util';
import { require } from './test-support.mjs';

function inspect(file) {
  let data = false, initializer = false, label = null, instructions = 0;
  const symbols = [];
  fs.readFileSync(file, 'utf8').split(/\r?\n/).forEach((raw, index) => {
    const line = raw.split(';', 1)[0].trim();
    if (!line) return;
    if (line === '.data') { data = true; return; }
    if (line === '.code') { data = false; return; }
    if (line.endsWith(':')) {
      label = line.slice(0, -1);
      if (label.startsWith('__rcc_init_')) initializer = true;
      return;
    }
    if (data) {
      const match = /^\.space\s+(0x[0-9a-fA-F]+|[0-9]+)$/.exec(line);
      require(match, file + ':' + (index + 1) + ': unrecognized data directive: ' + line);
      symbols.push({ name: label, cells: Number(match[1]) });
    } else if (initializer && !line.startsWith('.')) {
      instructions++;
      if (line.split(/\s+/)[0] === 'RET') initializer = false;
    }
  });
  return { file, data_cells: symbols.reduce((sum, symbol) => sum + symbol.cells, 0),
    initializer_instructions: instructions, symbols: symbols.sort((a, b) => b.cells - a.cells) };
}
const { values, positionals } = parseArgs({ allowPositionals: true, options: {
  'bank-size': { type: 'string', default: '64000' }, 'data-offset': { type: 'string', default: '1032' },
  json: { type: 'boolean' }, 'check-limit': { type: 'boolean' }, help: { type: 'boolean' },
} });
if (values.help) console.log('Usage: node tools/audit-layout.mjs ASSEMBLY... [--json] [--check-limit]\n'
  + '  [--bank-size 64000] [--data-offset 1032]');
else try {
  const bank = Number(values['bank-size']), offset = Number(values['data-offset']);
  require(positionals.length && Number.isSafeInteger(bank) && bank > 0
    && Number.isSafeInteger(offset) && offset >= 0, 'provide assembly paths, positive bank size and nonnegative data offset');
  const units = positionals.map(inspect), total = units.reduce((sum, unit) => sum + unit.data_cells, 0);
  const result = { units, total_data_cells: total, assumed_data_offset: offset,
    bank_size: bank, fits_single_gp_bank: total + offset <= bank };
  if (values.json) console.log(JSON.stringify(result, null, 2));
  else {
    for (const unit of [...units].sort((a, b) => b.data_cells - a.data_cells)) {
      console.log(unit.file + ': ' + unit.data_cells + ' data cells; ' + unit.initializer_instructions + ' initializer instructions');
      for (const symbol of unit.symbols.slice(0, 5)) console.log('  ' + symbol.cells + '  ' + symbol.name);
    }
    console.log('Total: ' + total + ' data cells; ' + (total + offset) + '/' + bank + ' cells including assumed data offset');
  }
  if (values['check-limit'] && !result.fits_single_gp_bank) process.exitCode = 1;
} catch (error) { console.error(error.message); process.exitCode = 1; }
