#!/bin/js
// A small system utility using the same host on Fuse-X and RVM/FUZIX.
import { cwd, getenv, readDir, readTextFile, stat, run, sleep, writeTextFile } from 'fuse:system';

const directory = scriptArgs[1] || cwd();
const configPath = scriptArgs[2] || directory + '/sample.json';
const [names, source, configInfo] = await Promise.all([
    readDir(directory), readTextFile(configPath), stat(configPath)
]);
const config = JSON.parse(source);
const note = await readTextFile(directory + '/' + config.note);
console.log(config.title);
console.log(note);
console.log('Files: ' + names.sort().join(', '));

// The child runs while the host continues servicing timers and Promise jobs.
const child = run(config.program || '/bin/echo', config.arguments || ['Hello from JavaScript']);
await sleep(1);
const result = await child;
const report = {
    directory,
    home: getenv('HOME') || null,
    files: names,
    configSize: configInfo.size,
    note,
    child: result
};
console.log(JSON.stringify(report, null, 2));
if (scriptArgs[3]) {
    await writeTextFile(scriptArgs[3], JSON.stringify(report, null, 2) + '\n');
    console.log('Saved ' + scriptArgs[3]);
}
if (result.code !== 0) throw new Error('Program did not exit successfully');
