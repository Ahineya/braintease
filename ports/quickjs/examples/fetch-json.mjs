// Open in Rift and press F5. Optional CLI args: URL, cache path, viewer.
import { fetch } from 'fuse:http';
import { writeTextFile, readTextFile, run } from 'fuse:system';
const url = scriptArgs[1] || 'https://httpbin.org/json';
const cache = scriptArgs[2] || '/tmp/js-fetch-cache.json';
const viewer = scriptArgs[3] || '/bin/cat';
let timer, ticks = 0;
function progress() {
    console.log('Fetching…', ++ticks);
    timer = setTimeout(progress, 500);
}
console.log('GET', url);
progress();
try {
    const reply = await fetch(url, { headers: { Accept: 'application/json' }, timeoutMs: 45000 });
    if (!reply.ok) throw new Error('HTTP ' + reply.status);
    const data = await reply.json();
    clearTimeout(timer);
    await writeTextFile(cache, JSON.stringify(data, null, 2) + '\n');
    // Check the cache through the UTF-8 file API, then launch a guest program.
    JSON.parse(await readTextFile(cache));
    console.log('Cached', cache, 'after', ticks, 'progress updates');
    const shown = await run(viewer, [cache], { maxOutputBytes: 1048576 });
    if (shown.code !== 0) throw new Error('Viewer failed: ' + shown.stderr);
    console.log(shown.stdout);
} finally {
    clearTimeout(timer);
}
