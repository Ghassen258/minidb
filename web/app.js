import createMiniDB from './minidb.mjs';

const $ = id => document.getElementById(id);
const STORAGE_KEY = 'minidb-browser-snapshot-v1';
const DEMO = [
  "CREATE TABLE engineers (id INT PRIMARY KEY, name TEXT, specialty TEXT, years INT);",
  "INSERT INTO engineers VALUES (10, 'Ada', 'Algorithms', 7);",
  "INSERT INTO engineers VALUES (20, 'Grace', 'Compilers', 11);",
  "INSERT INTO engineers VALUES (30, 'Linus', 'Systems', 9);",
  "INSERT INTO engineers VALUES (40, 'Margaret', 'Reliability', 12);",
  "INSERT INTO engineers VALUES (50, 'Barbara', 'Databases', 15);",
  "INSERT INTO engineers VALUES (60, 'Ken', 'Unix', 14);"
];
const EXAMPLES = {
  all: 'SELECT * FROM engineers;',
  indexed: 'SELECT name, specialty FROM engineers WHERE id = 50;',
  explain: 'EXPLAIN SELECT * FROM engineers WHERE id = 50;',
  count: 'SELECT COUNT(*) FROM engineers;',
  insert: "INSERT INTO engineers VALUES (70, 'Dennis', 'Languages', 16);",
  update: 'UPDATE engineers SET years = 18 WHERE id = 50;',
  stats: 'STATS;',
  tables: 'SHOW TABLES;'
};
let db;
let lastResult;
const editor = $('sql-editor');

function updateLines() {
  const numberOfLines = Math.max(1, editor.value.split('\n').length);
  $('line-numbers').textContent = Array.from({length: numberOfLines}, (_,i) => i+1).join('\n');
}
editor.addEventListener('input',updateLines);
editor.addEventListener('scroll',()=>{$('line-numbers').scrollTop = editor.scrollTop;});
editor.addEventListener('keydown',event=>{
  if((event.ctrlKey||event.metaKey)&&event.key==='Enter'){event.preventDefault();runEditor();}
  if(event.key==='Tab'){event.preventDefault();const a=editor.selectionStart,b=editor.selectionEnd;editor.setRangeText('  ',a,b,'end');updateLines();}
});

// Split on SQL semicolons but never inside a single-quoted string. Double
// apostrophes are escaped string content, and -- comments end on newline.
function statements(sql) {
  const list=[];
  let current='', quoted=false, comment=false;
  for(let i=0;i<sql.length;i++){
    const c=sql[i],n=sql[i+1];
    if(comment){if(c==='\n')comment=false;else continue;}
    if(!quoted&&c==='-'&&n==='-'){comment=true;i++;continue;}
    if(!quoted&&c==='#'){comment=true;continue;}
    if(c==="'"){
      current+=c;
      if(quoted&&n==="'"){current+=n;i++;continue;}
      quoted=!quoted;continue;
    }
    if(c===';'&&!quoted){if(current.trim())list.push(current.trim()+';');current='';}
    else current+=c;
  }
  if(current.trim())list.push(current.trim());
  return list;
}
function execute(sql){return JSON.parse(db.runSql(sql));}
function save(){
  try {
    const snapshot=db.saveSnapshot();
    localStorage.setItem(STORAGE_KEY,snapshot);
    $('storage-status').textContent='SAVED IN THIS BROWSER';
  } catch(error) {
    console.warn('MiniDB browser snapshot not persisted:',error);
    $('storage-status').textContent='SESSION ONLY — EXPORT TO KEEP';
  }
}
function runBatch(sql, show=true){
  const queries=statements(sql);
  if(!queries.length){if(show)renderResult({ok:false,message:'Enter a SQL statement first.',columns:[],rows:[]},0,0);return null;}
  const start=performance.now();let last,completed=0;
  for(const command of queries){last=execute(command);completed++;if(!last.ok)break;}
  const duration=performance.now()-start;
  if(show)renderResult(last,duration,completed);
  if(last?.ok || completed>1)save();
  refreshSchema();
  return last;
}
function runEditor(){if(!db)return;runBatch(editor.value);}
$('run-query').addEventListener('click',runEditor);
$('examples').addEventListener('change',event=>{
  const sample=EXAMPLES[event.target.value];
  if(sample){editor.value=sample;updateLines();editor.focus();}
});

function renderResult(result,time,count){
  lastResult=result;
  const root=$('results');root.replaceChildren();
  const message=document.createElement('p');message.className='result-message'+(result.ok?'':' error');message.textContent=result.message|| (result.ok?'Query complete.':'Query failed.');root.append(message);
  if(result.columns.length){
    const table=document.createElement('table');table.className='results-table';
    const thead=document.createElement('thead');const header=document.createElement('tr');
    for(const name of result.columns){const th=document.createElement('th');th.textContent=name;header.append(th);}
    thead.append(header);table.append(thead);
    const tbody=document.createElement('tbody');
    for(const row of result.rows){const tr=document.createElement('tr');for(const value of row){const td=document.createElement('td');td.textContent=value;tr.append(td);}tbody.append(tr);}
    table.append(tbody);root.append(table);
  }
  $('result-time').textContent=time.toFixed(1)+' ms';
  $('result-count').textContent=result.rows.length+' '+(result.rows.length===1?'ROW':'ROWS')+' / '+count+' '+(count===1?'STATEMENT':'STATEMENTS');
  $('result-state').textContent=result.ok?'SUCCESS':'ERROR';
  $('result-indicator').className='result-indicator '+(result.ok?'success':'error');
}
function refreshSchema(){
  if(!db)return;
  const result=execute('SHOW TABLES;');
  const list=$('schema-list');list.replaceChildren();
  if(!result.rows.length){const empty=document.createElement('p');empty.className='loading-text';empty.textContent='No tables yet.';list.append(empty);return;}
  for(const [name,count,key] of result.rows){
    const button=document.createElement('button');button.type='button';button.className='schema-table';
    const title=document.createElement('span');title.className='table-name';title.textContent='▤ '+name;
    const meta=document.createElement('span');meta.className='table-meta';meta.textContent=count+' rows · PK '+key;
    button.append(title,meta);button.title='Query '+name;
    button.addEventListener('click',()=>{editor.value='SELECT * FROM '+name+';';updateLines();runEditor();});
    list.append(button);
  }
}
$('refresh-schema').addEventListener('click',refreshSchema);
$('reset-db').addEventListener('click',()=>{
  if(!confirm('Reset MiniDB to its original sample data? Your current browser database will be deleted. Download a backup first if needed.'))return;
  db.resetDatabase();seed();save();editor.value=EXAMPLES.all;updateLines();runEditor();
});
$('download-db').addEventListener('click',()=>{
  try{
    const b64=db.saveSnapshot();const binary=atob(b64);
    const arr=new Uint8Array(binary.length);for(let i=0;i<binary.length;i++)arr[i]=binary.charCodeAt(i);
    const url=URL.createObjectURL(new Blob([arr],{type:'application/octet-stream'}));
    const a=document.createElement('a');a.href=url;a.download='minidb-backup.mdb';document.body.append(a);a.click();a.remove();
    setTimeout(()=>URL.revokeObjectURL(url),1500);
  }catch(e){alert('Cannot export database: '+e.message);}
});
$('import-trigger').addEventListener('click',()=>$('import-file').click());
$('import-file').addEventListener('change',async e=>{
  const file=e.target.files?.[0];e.target.value='';if(!file)return;
  if(file.size>2*1024*1024){alert('Browser demo files must be smaller than 2 MiB.');return;}
  if(!confirm('Import '+file.name+'? This will replace the current database.'))return;
  try{
    const base64=await new Promise((resolve,reject)=>{
      const reader=new FileReader();reader.onload=()=>resolve(String(reader.result).split(',')[1]);reader.onerror=()=>reject(new Error('Cannot read file'));reader.readAsDataURL(file);
    });
    if(!db.loadSnapshot(base64))throw new Error('This is not a valid MiniDB database file.');
    save();editor.value='SHOW TABLES;';updateLines();runEditor();
  }catch(err){alert('Import failed: '+err.message);}
});
function seed(){for(const query of DEMO){const result=execute(query);if(!result.ok)throw new Error(result.message);}}

(async()=>{
  try{
    db=await createMiniDB({locateFile:path=>new URL(path,import.meta.url).href});
    if(typeof db.runSql!=='function'||typeof db.saveSnapshot!=='function'||typeof db.loadSnapshot!=='function')throw Error('WebAssembly exports missing');
    const saved=(()=>{try{return localStorage.getItem(STORAGE_KEY);}catch{return null;}})();
    if(saved){
      if(!db.loadSnapshot(saved)){
        console.warn('Saved MiniDB snapshot is invalid, loading demo data.');
        seed();save();
      }
    }else{seed();save();}
    $('engine-status').textContent='C++ ENGINE ONLINE';$('status-dot').classList.add('ready');
    for(const id of ['run-query','download-db','import-trigger','reset-db'])$(id).disabled=false;
    runEditor();
  }catch(error){
    console.error('MiniDB WebAssembly could not initialize:',error);
    $('engine-status').textContent='ENGINE UNAVAILABLE';$('status-dot').classList.add('fail');
    renderResult({ok:false,message:'The WebAssembly engine could not load. Try refreshing or check the Pages build.',columns:[],rows:[]},0,0);
  }
})();
