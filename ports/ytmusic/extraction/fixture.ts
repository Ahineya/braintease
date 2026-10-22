/* SPDX-License-Identifier: MIT */
// Imports are resolved to the pinned, pristine upstream tree by bundle.mjs.
import { preprocessPlayer, getFromPrepared } from 'ejs:solver';
import { tests } from 'ejs:tests';

export { preprocessPlayer as prepare };

export function run(player: string, id: string, negative: boolean, cached = false): string {
    return runUsing(player, id, negative, cached, preprocessPlayer);
}

export function runUsing(player: string, id: string, negative: boolean, cached: boolean,
                         prepare: (source: string) => string): string {
    const test = tests.find(t => t.player === id);
    if (!test) throw new Error('Unknown pinned player fixture');
    const started = Date.now();
    const prepared = cached ? player : prepare(player);
    const preparedAt = Date.now();
    globalThis.extractionPhase?.('prepared');
    const solvers = getFromPrepared(prepared);
    const compiledAt = Date.now();
    globalThis.extractionPhase?.('compiled');
    const results = [];
    let checks = 0;
    for (const mode of ['n', 'sig'] as const) {
        if (!solvers[mode]) throw new Error('Missing ' + mode + ' solver');
        for (const vector of test[mode] || []) {
            const actual = solvers[mode]!(vector.input);
            const expected = negative && checks === 0 ? vector.expected + 'WRONG' : vector.expected;
            if (actual !== expected) throw new Error('EJS vector mismatch: ' + mode + ' ' + checks);
            results.push({ mode, input: vector.input, output: actual });
            checks++;
        }
    }
    if (!checks) throw new Error('No upstream checks executed');
    return JSON.stringify({ player: id, checks, playerLength: player.length,
        preparedLength: prepared.length, milliseconds: Date.now() - started,
        phases: { prepare: preparedAt - started, compile: compiledAt - preparedAt,
                  solve: Date.now() - compiledAt }, results });
}
