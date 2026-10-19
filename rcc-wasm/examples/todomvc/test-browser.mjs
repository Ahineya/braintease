// Optional real-DOM check. The page supplies isolated real localStorage.
export async function checkTodoMVC({app,root,effectHost}){
  const check=(value,message)=>{if(!value)throw Error(message);};
  const wait=async test=>{const end=performance.now()+7000;while(!test()){if(performance.now()>end)throw Error('TodoMVC browser test timed out');await new Promise(r=>setTimeout(r,10));}};
  const input=()=>root.querySelector('.new-todo');
  // Run through the public UI with the page's isolated test storage.
  await wait(()=>input()&&!input().disabled);
  check(root.querySelectorAll('.todo-list li').length===0,'Use a fresh demo origin or empty the list before running ?test=todo');
  async function add(title){input().value=title;input().dispatchEvent(new Event('input',{bubbles:true}));input().dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true}));await wait(()=>root.textContent.includes(title)&&input().value==='');}
  await add('Learn RFL 🌍');await add('Build a tiny SPA');
  const first=root.querySelector('.todo-list li'),label=first.querySelector('label');
  label.dispatchEvent(new MouseEvent('dblclick',{bubbles:true}));await wait(()=>first.querySelector('.edit'));
  const edit=first.querySelector('.edit');check(document.activeElement===edit,'deferred edit focus failed');
  edit.value='Edited 🌍';edit.dispatchEvent(new Event('input',{bubbles:true}));edit.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true}));
  await wait(()=>!first.querySelector('.edit')&&label.textContent==='Edited 🌍');
  const toggle=first.querySelector('.toggle');toggle.click();await wait(()=>first.classList.contains('completed'));
  root.querySelector('a[href="#/active"]').click();await wait(()=>location.hash==='#/active'&&root.querySelectorAll('.todo-list li').length===1);
  history.back();await wait(()=>root.querySelectorAll('.todo-list li').length===2);
  root.querySelector('a[href="#/completed"]').click();await wait(()=>root.querySelectorAll('.todo-list li').length===1);
  root.querySelector('.clear-completed').click();await wait(()=>!root.querySelector('.todo-list li'));
  root.querySelector('a[href="#/"]').click();await wait(()=>root.querySelectorAll('.todo-list li').length===1&&root.textContent.includes('Saved locally'));
  check(app.stats.locationSubscriptions===1,'subscription count changed');
  app.dispose();check(app.stats.locationSubscriptions===0&&app.stats.outstandingEffects===0&&app.stats.liveAllocations===1,'application disposal leaked');
  const {mountBrowser}=await import('../../browser/bridge.mjs');
  const reloaded=await mountBrowser(new URL('./build/app.wasm',import.meta.url),{root,effectHost});
  try {
    await wait(()=>root.querySelector('.new-todo')&&!root.querySelector('.new-todo').disabled);
    check(root.querySelectorAll('.todo-list li').length===1&&root.textContent.includes('Build a tiny SPA'),'fresh instance did not restore storage');
    root.querySelector('.destroy').click();await wait(()=>!root.querySelector('.main')&&root.textContent.includes('Saved locally'));
  }finally{reloaded.dispose();}
  check(reloaded.stats.locationSubscriptions===0&&reloaded.stats.liveAllocations===1,'reloaded application disposal leaked');
}
