/* SPDX-License-Identifier: MIT */
// Use upstream dependency analysis/emission, with the same EJS correctness vectors.
import { JsAnalyzer } from 'ytjs:JsAnalyzer';
import { JsExtractor } from 'ytjs:JsExtractor';
import { nsigMatcher } from 'ytjs:matchers';
import { setupNodes } from 'ejs:setup';
import { generate } from 'astring';
import { runUsing } from './fixture';

export function prepare(player: string): string {
    // Meriyah 7 only uses this hook for plain Identifier and string Literal
    // nodes. Keep this narrow adapter local to parsing; it is not a general
    // structuredClone implementation or a capability of evaluated player code.
    const original = globalThis.structuredClone;
    if (!original) globalThis.structuredClone = node => {
        if (node?.type !== 'Identifier' && !(node?.type === 'Literal' && typeof node.value === 'string'))
            throw new Error('Unsupported Meriyah clone node');
        return JSON.parse(JSON.stringify(node));
    };
    let result;
    try {
        const analyzer = new JsAnalyzer(player, {
            extractions: [{ friendlyName: 'nsigFunction', match: nsigMatcher }]
        });
        result = new JsExtractor(analyzer).buildScript({ disallowSideEffectInitializers: true });
    } finally {
        if (!original) delete globalThis.structuredClone;
    }
    if (!result.exported.includes('nsigFunction')) throw new Error('YouTube.js found no decipher function');
    // Adapt the extracted URL constructor to EJS's two solver callbacks. This
    // executes upstream code; it does not implement either transformation.
    return generate({ type: 'Program', body: setupNodes }) + result.output + `
function solve(n, sig) {
    const url = exportedVars.nsigFunction("https://youtube.com/watch?v=yt-dlp-wins", "s", sig ? encodeURIComponent(sig) : undefined);
    url.set("n", n);
    const proto = Object.getPrototypeOf(url);
    const keys = Object.keys(proto).concat(Object.getOwnPropertyNames(proto));
    for (const key of keys) {
        if (!["constructor", "set", "get", "clone"].includes(key)) { url[key](); break; }
    }
    const s = url.get("s");
    return { sig: s ? decodeURIComponent(s) : null, n: url.get("n") ?? null };
}
_result.n = n => solve(n, undefined).n;
_result.sig = s => solve(undefined, s).sig;
`;
}

export function run(player: string, id: string, negative: boolean, cached = false): string {
    return runUsing(player, id, negative, cached, prepare);
}
