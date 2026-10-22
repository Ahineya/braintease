// SPDX-License-Identifier: MIT
import path from 'node:path';
import {pathToFileURL, fileURLToPath} from 'node:url';
const [upstream, dependencies, output, youtubejs] = process.argv.slice(2);
if (!output) throw Error('usage: bundle.mjs EJS_ROOT BUILD_DEPENDENCIES OUTPUT');
const {build} = await import(pathToFileURL(path.resolve(dependencies, 'node_modules/esbuild/lib/main.js')));
await build({
    entryPoints: [fileURLToPath(new URL(youtubejs ? './fixture-ytjs.ts' : './fixture.ts', import.meta.url))],
    bundle: true, minify: true, format: 'iife', globalName: 'extractionProbe',
    platform: 'neutral', target: 'es2020', outfile: output,
    nodePaths: [path.resolve(dependencies, 'node_modules')], legalComments: 'inline',
    plugins: [{name: 'pinned-ejs', setup(b) {
        b.onResolve({filter: /^ejs:/}, a => {
            const names = {'ejs:solver':'solvers.ts', 'ejs:tests':'test/tests.ts', 'ejs:setup':'setup.ts'};
            if (!(a.path in names)) throw Error('Unknown EJS import');
            return {path: path.resolve(upstream, 'src/yt/solver', names[a.path])};
        });
        if (youtubejs) {
            b.onResolve({filter: /^ytjs:/}, a => ({path: path.resolve(youtubejs, 'src/utils/javascript', a.path.slice(5) + '.ts')}));
            b.onResolve({filter: /^meriyah$/}, () => ({path: path.resolve(dependencies, 'node_modules/meriyah-ytjs/dist/meriyah.mjs')}));
        }
    }}],
});
