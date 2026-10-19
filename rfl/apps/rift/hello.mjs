// Rift JavaScript: F5 saves and runs this module with /bin/js.
// The guest QuickJS host supplies fuse:system and fuse:http.
import { cwd, readDir, sleep } from 'fuse:system';
const directory = cwd();
console.log(`Files in ${directory}:`);
for (const name of (await readDir(directory)).sort()) {
    console.log('  ' + name);
}
await sleep(50);
console.log('JavaScript modules, Unicode 🌍 and async I/O are ready.');
