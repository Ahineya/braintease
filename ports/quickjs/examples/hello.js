#!/bin/js
const name = scriptArgs[1] ?? 'Fuse-X';
console.log(`Hello, ${name}! Ж 😀`);
console.log('UTC:', new Date().toISOString());
Promise.resolve(70000).then(n => console.log('Promise result:', n + 42));
