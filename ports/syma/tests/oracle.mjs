// Generate portable expectations using the original engine, never the C port.
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
const root=path.resolve(process.argv[2]||'build/syma-audit/reference');
const core=root+'/packages/core/src/';
const engine=await import(pathToFileURL(core+'core/engine.js'));
const {SymaParser}=await import(pathToFileURL(core+'core/parser.js'));
const {foldPrims}=await import(pathToFileURL(core+'primitives.js'));
const {renderToString}=await import(pathToFileURL(root+'/packages/projectors/src/string.js'));
const parser=new SymaParser();
const cases=[];
function record(name,source,rulesSource='{Rules}',maxsteps=10000){
 const expr=parser.parseString(source),rules=engine.extractRulesFromNode(parser.parseString(rulesSource));
 const universe={k:'Call',h:{k:'Sym',v:'Universe'},a:[]};
 const fold=(e,skip=[])=>foldPrims(e,skip,{universe,normalizeFunc:(x,r)=>engine.normalize(x,r,10000,false,fold),extractRulesFunc:engine.extractRules,renderToString});
 const entry={name,expr,rules:rules.allRules,maxsteps,skipprims:false,preserve:false,fold:true};
 try{entry.expected=engine.normalize(expr,rules,maxsteps,false,fold);}catch(e){entry.error=e.message;}
 cases.push(entry);
}
const expressions=[
 '{BitShiftLeft 1 31}','{BitShiftLeft 1 33}','{BitShiftRight -2147483648 31}','{BitShiftRightUnsigned -1 0}',
 '{BitAnd 4294967295 2147483648}','{BitNot 0}','{BitOr 65536 1}','{BitXor 4294967295 65535}',
 '{StrLen "😀"}','{SplitToChars "😀"}','{ToUpper "їß𐐨"}','{ToLower "ЇABC𐐀"}',
 '{Substring "abcdef" -3 -1}','{Substring "abcdef" 4 1}','{IndexOf "a\\u0000b" "\\u0000"}',
 '{ReplaceAll "aba" "" "-"}','{Replace "abc" "b" "$`$&$\'"}',
 '{Serialize {Test "a\\u0000b"}}','{Deserialize "bad"}','{Deserialize "{\\"k\\":\\"Bad\\",\\"v\\":\\"x\\"}"}',
 '{IsTrue "True"}','{IsFalse "False"}','{And "True" True}','{Not "True"}','{If "True" 1 2}',
 '{ParseNum "0x10"}','{ParseNum "  -2.5e2x"}','{CharFromCode 128512}','{Div 1 0}','{Sqrt -1}',
 '{ToString 1e21}','{ToString 1e-7}','{ToString -0}','{"Add" 1 2}'
];
for(const source of expressions)record(source,source);
record('ordinary FIFO traversal','{Root {Nest {A}} {B}}',`{Rules
 {R "a" {A} X} {R "b" {B} Y}
 {R "chooseB" {Root {Nest {A}} Y} FirstB}
 {R "chooseA" {Root {Nest X} {B}} FirstA}}`);
record('guard exclusions accumulate','{A}',`{Rules {R a {A} Done :guard {B}}
 {R b {B} True :guard {A}}}`);
record('context changes between steps','{Program {State 1} {Get} {Advance}}',`{Rules
 {R get {Get} {Value n_} :with {Program {State n_} ..}}
 {R advance {Program {State 1} {Value 1} {Advance}} {Program {State 2} {Get}}}}`);
const out=path.resolve('ports/syma/tests/extra.jsonl');
fs.writeFileSync(out,cases.map(c=>JSON.stringify(c)).join('\n')+'\n');
console.log(`Recorded ${cases.length} additional reference cases`);
