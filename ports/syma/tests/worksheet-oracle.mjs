// Independent original-engine check of the author's OOP example and new math rules.
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
const base=path.resolve(process.argv[2] || 'build/syma-audit/reference');
const src=base+'/packages/core/src/';
const engine=await import(pathToFileURL(src+'core/engine.js'));
const {SymaParser}=await import(pathToFileURL(src+'core/parser.js'));
const {Module,ModuleLinker}=await import(pathToFileURL(src+'core/module-compiler.js'));
const {Call,Sym}=await import(pathToFileURL(src+'ast-helpers.js'));
const {foldPrims}=await import(pathToFileURL(src+'primitives.js'));
const parser=new SymaParser();
const directory='ports/syma/library';
const modules=fs.readdirSync(directory).filter(p=>p.endsWith('.syma')).map(p=>{
 const ast=parser.parseString(fs.readFileSync(directory+'/'+p,'utf8'));
 return new Module(ast.a[0].v,ast);
});
const fold=(x,skip=[])=>foldPrims(x,skip,{universe,normalizeFunc:(x,r)=>engine.normalize(x,r,100000,false,fold),extractRulesFunc:engine.extractRules});
let universe;
function linked(name){
 const selected=new Map();
 function include(name){const m=modules.find(m=>m.name===name); if(!m||selected.has(name))return;
  selected.set(name,m);for(const i of m.imports)include(i.module);}
 include('Core/Syntax/Global');include(name);
 universe=new ModuleLinker([...selected.values()]).link(name,true);
 universe=engine.applyRuleRules(universe,fold);
 return universe;
}
const entries=[];
function record(name,expr,rules,expected){
 const result=engine.normalize(expr,engine.indexRules(rules),100000,false,fold);
 if(expected && JSON.stringify(result)!==JSON.stringify(parser.parseString(expected)))
  throw Error('oracle mismatch: '+name+' '+JSON.stringify(result));
 entries.push({name,universe,expr,rules,maxsteps:100000,skipprims:false,preserve:false,fold:true,expected:result});
}
// The original overview's REPL keeps raw module rules; no eager compile-time folding.
const original=fs.readFileSync('ports/syma/examples/oop/overview-cell-67.txt','utf8');
const rawModules=[...original.matchAll(/:module multiline\n([\s\S]*?)\n:end/g)].map(m=>{
 const ast=parser.parseString(m[1]); return new Module(ast.a[0].v,ast);
});
const rawRules=rawModules.flatMap(m=>{
 const u=new ModuleLinker([m]).link(m.name,true);
 return engine.findSection(u,'Rules').a;
});
const vm=original.slice(original.indexOf(':rule Run '));
let block=null;
for(const line of vm.split('\n')){
 if(line===':rule multiline') block=[];
 else if(line===':end'){
  const text=block.join('\n').trim(),match=text.match(/^(\S+)\s+([\s\S]*)$/);
  rawRules.push(parser.parseInlineRule(match[1],match[2])); block=null;
 }else if(block) block.push(line);
 else if(line.startsWith(':rule ')){
  const match=line.match(/^:rule\s+(\S+)\s+([\s\S]*)$/);
  rawRules.push(parser.parseInlineRule(match[1],match[2]));
 }
}
universe=Call(Sym('Universe'),Call(Sym('Rules'),...rawRules));
const originalExpression=parser.parseString(fs.readFileSync('ports/syma/examples/oop/overview-cell-68.txt','utf8'));
const expected='{Output true "woof" true false false "woof" "fuck" true "woof"}';
record('original OOP overview',originalExpression,engine.extractRules(universe).allRules,expected);
linked('OOP/Objects');
// Reuse exactly the original DSL body through the packaged library boundary.
const body=originalExpression.a[0].a[0].a;
record('packaged original OOP overview',Call(Sym('OOP/Objects/Run'),Call(Sym('Program'),...body)),engine.extractRules(universe).allRules,expected);
linked('Math/Small');
const rules=engine.extractRules(universe).allRules;
for(const [source,expected] of [
 ['{Math/Small/Differentiate {Math/Small/Sum {Math/Small/Power x 3} {Math/Small/Product 2 x}} x}', '{Math/Small/Sum {Math/Small/Product 3 {Math/Small/Power x 2}} 2}'],
 ['{Math/Small/Substitute {Math/Small/Sum {Math/Small/Power x 3} {Math/Small/Product 2 x}} x 3}', '33'],
 ['{Math/Small/Differentiate {Math/Small/Sin x} x}', '{Math/Small/Cos x}'],
 ['{Math/Small/Power 0 0}', '{Math/Small/Power 0 0}'],
 ['{Math/Small/Differentiate {Unknown x} x}', '{Math/Small/UnevaluatedDerivative {Unknown x} x}'],
 ['{Math/Small/Expand {Math/Small/Product a {Math/Small/Sum b c}}}', '{Math/Small/Sum {Math/Small/Product a b} {Math/Small/Product a c}}'],
]) record(source,parser.parseString(source),rules,expected);
fs.writeFileSync('ports/syma/tests/worksheet-oracle.jsonl',entries.map(e=>JSON.stringify(e)).join('\n')+'\n');
console.log('Original JS engine confirmed '+entries.length+' OOP/math results.');
