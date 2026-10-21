import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
const reference=path.resolve(process.argv[2]||'build/syma-audit/reference');
const pkg=reference+'/packages/core/src/';
const engine=await import(pathToFileURL(pkg+'core/engine.js'));
const {SymaParser}=await import(pathToFileURL(pkg+'core/parser.js'));
const {Module,ModuleLinker}=await import(pathToFileURL(pkg+'core/module-compiler.js'));
const {Call,Sym}=await import(pathToFileURL(pkg+'ast-helpers.js'));
const {foldPrims}=await import(pathToFileURL(pkg+'primitives.js'));
const {renderToString}=await import(pathToFileURL(reference+'/packages/projectors/src/string.js'));
const parser=new SymaParser(),entries=[],errors=[];
const globalAST=parser.parseString(fs.readFileSync('ports/syma/library/core-syntax-global.syma','utf8'));
for(const filename of fs.readdirSync('ports/syma/examples/notebooks').filter(p=>p.endsWith('.symnb'))){
 const book=JSON.parse(fs.readFileSync('ports/syma/examples/notebooks/'+filename,'utf8'));
 let definitions=[],universe=Call(Sym('Universe'),Call(Sym('Rules'))),maxsteps=10000,block=null,kind='',expression=[];
 const fold=(e,skip=[])=>foldPrims(e,skip,{universe,normalizeFunc:(x,r)=>engine.normalize(x,r,10000,false,fold),extractRulesFunc:engine.extractRules,renderToString});
 function evaluate(){
  const source=expression.join('\n');expression=[];
  if(!source.replace(/;[^\n]*/g,'').trim())return;
  try{
   const expr=parser.parseString(source),rules=engine.extractRulesFromNode(Call(Sym('Rules'),...definitions));
   universe=Call(Sym('Universe'),Call(Sym('Rules'),...definitions));
   const entry={name:filename,universe,expr,rules:rules.allRules,maxsteps,skipprims:false,preserve:false,fold:true};
   try{entry.expected=engine.normalize(expr,rules,maxsteps,false,fold);}catch(e){entry.error=e.message;}
   entries.push(entry);
  }catch(e){errors.push({filename,source,error:e.message});}
 }
 for(const cell of book.cells){
  if(cell.type!=='code')continue;
  for(const line of cell.content.split('\n')){
   if(block){
    if(line===':rule multiline'&&block.join('\n').replace(/;[^\n]*/g,'').trim()===''){block=[];continue;}
    if(line!==':end'){block.push(line);continue;}
    const source=block.join('\n').trim();block=null;
    try{
     if(kind==='module'){
      const ast=parser.parseString(source),module=new Module(ast.a[0].v,ast);
      universe=new ModuleLinker([new Module('Core/Syntax/Global',globalAST),module]).link(module.name,true);
      universe=engine.applyRuleRules(universe,fold);
      definitions=engine.findSection(universe,'Rules').a;
     }else{
      const match=source.match(/^(\S+)\s+([\s\S]*)$/);
      if(match)definitions.push(parser.parseInlineRule(match[1],match[2]));
     }
    }catch(e){errors.push({filename,source,error:e.message});}
    continue;
   }
   if(line.startsWith(':')){
    evaluate();
    if(line===':clear'){definitions=[];universe=Call(Sym('Universe'),Call(Sym('Rules')));}
    else if(line===':module multiline'||line===':rule multiline'){block=[];kind=line.includes('module')?'module':'rule';}
    else if(line.startsWith(':set maxsteps '))maxsteps=Number(line.slice(14));
    else if(line.startsWith(':trace '))expression.push(line.slice(7));
    else if(line.startsWith(':import ')){} // FP's following expressions use qualified R/... names.
    else if(line.startsWith(':rule ')){
     const match=line.match(/^:rule\s+(\S+)\s+([\s\S]*)$/);
     try{definitions.push(parser.parseInlineRule(match[1],match[2]));}catch(e){errors.push({filename,source:line,error:e.message});}
    }else errors.push({filename,source:line,error:'unhandled notebook directive'});
   }else expression.push(line);
  }
  evaluate();
 }
}
fs.writeFileSync('ports/syma/tests/notebooks.jsonl',entries.map(x=>JSON.stringify(x)).join('\n')+'\n');
fs.writeFileSync('ports/syma/tests/notebook-source-errors.json',JSON.stringify(errors,null,2)+'\n');
console.log(`Recorded ${entries.length} notebook evaluations; ${errors.length} source errors`);
