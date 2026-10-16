// SPDX-License-Identifier: GPL-2.0-or-later
import fs from 'node:fs';

// Native C16 records: 448 map cells, four state cells, padding to 512 cells.
// Buffer octets become individual C characters in the root-image installer.
export function sokobanLevels(text) {
  const maps = [];
  let rows = [];
  const finish = () => { if (rows.length) maps.push(rows); rows = []; };
  for (const line of text.split(/\r?\n/)) {
    if (line.includes('#')) rows.push(line);
    else finish();
  }
  finish();
  if (maps.length < 2) throw Error('Sokoban collection needs a title and a puzzle');
  return Buffer.concat(maps.map((lines, level) => {
    const width = Math.max(...lines.map(line => line.length));
    if (width > 32 || lines.length > 14) throw Error('Sokoban map exceeds 32x14');
    const record = Buffer.alloc(512), top = Math.floor((14-lines.length)/2), left = Math.floor((32-width)/2);
    record.fill(32, 0, 448);
    let players = 0, boxes = 0, goals = 0, done = 0;
    lines.forEach((line, y) => [...line].forEach((c, x) => {
      if (c === '@' || c === '+') {
        players++; record[448] = y+top; record[449] = x+left;
        c = c === '+' ? '.' : ' ';
      }
      if (level && !' #$.*'.includes(c)) throw Error('Invalid Sokoban map cell');
      if (c === '$' || c === '*') boxes++;
      if (c === '.' || c === '*') goals++;
      if (c === '*') done++;
      record[(y+top)*32+x+left] = c.charCodeAt(0);
    }));
    if (players !== 1 || (level && (!boxes || boxes !== goals))) throw Error('Invalid Sokoban map counts');
    record[450] = done; record[451] = boxes;
    return record;
  }));
}

export function sokobanFiles() {
  const text = fs.readFileSync(new URL('../upstream/Applications/games/ljh.sok', import.meta.url), 'utf8');
  return [
    {name: '/usr/lib/sok/sok.levels', mode: 0o100644, data: sokobanLevels(text)},
    {name: '/usr/lib/sok/README', mode: 0o100644,
     data: Buffer.from(text.slice(0, text.indexOf('\n;')) + '\n\nPlay with sokoban; controls are in man sokoban.\n')}
  ];
}
