#!/usr/bin/env python3
"""Capture real upstream normalization calls while running the original tests.

The reference is an isolated copy made during the audit; the side repo is read only.
The produced corpus has no dependency on Node or JavaScript in the guest.
"""
import argparse, json, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
parser=argparse.ArgumentParser()
parser.add_argument('--reference',type=Path,default=ROOT/'build/syma-audit/reference')
parser.add_argument('--node-modules',type=Path,default=ROOT.parent/'syma-fe/node_modules')
args=parser.parse_args()
out=ROOT/'ports/syma/tests/corpus.jsonl'
engine=args.reference/'packages/core/src/core/engine.js'
old=engine.read_text()
wrapper='''import fs from 'node:fs';
import * as original from './engine/index.js';
export * from './engine/index.js';
function capture(op,args) {
 const [expr,rules,maxsteps=10000,skipprims=false,fold=null,preserve=false]=args;
 const rs=Array.isArray(rules)?rules:rules?.allRules||[];
 const entry={op,expr:JSON.parse(JSON.stringify(expr)),rules:rs.map(r=>Object.fromEntries(
 ['name','lhs','rhs','guard','prio','scope','withPattern','letPattern','innermost'].map(k=>[k,r[k]??null]))),maxsteps,skipprims,preserve,fold:!!fold,meta:!!fold?.skipFolds};
 try {const result=original[op](...args);entry.expected=result;return result;}
 catch(e){entry.error=e.message;throw e;}
 finally {fs.appendFileSync(OUTPUT,JSON.stringify(entry)+'\\n');}
}
export function normalize(...args){return capture('normalize',args);}
export function normalizeIterative(...args){return capture('normalizeIterative',args);}
'''.replace('OUTPUT',json.dumps(str(out)))
out.write_text('')
try:
 engine.write_text(old+'\n'+wrapper)
 subprocess.run(['node',str(args.node_modules/'vitest/vitest.mjs'),'run','packages/core/src',
 '--root',str(args.reference),'--config',str(args.reference/'vitest.config.js'),
 '--maxWorkers=1','--minWorkers=1'],check=True,cwd=args.reference)
finally:engine.write_text(old)
cases=[json.loads(s) for s in out.read_text().splitlines()]
print(f'Captured {len(cases)} original normalization calls in {out}')
