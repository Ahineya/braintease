// Node harness for the unchanged browser worker: only adapt platform globals.
import {parentPort,workerData} from 'node:worker_threads';
const platformFetch=globalThis.fetch;
globalThis.fetch=(url,options)=>platformFetch(new URL(url,workerData.base),options);
globalThis.postMessage=(data,transfer)=>parentPort.postMessage(data,transfer);
globalThis.onmessage=null;
await import('../worker.mjs');
parentPort.on('message',data=>globalThis.onmessage({data}));
