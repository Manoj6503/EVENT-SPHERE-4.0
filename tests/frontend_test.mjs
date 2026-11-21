// Unit tests for the offline check-in queue (Node's built-in test runner; no npm packages needed):  node --test tests/
import test from 'node:test';
import assert from 'node:assert/strict';
import {enqueue,flush,load,KEY} from '../frontend-src/src/offlineQueue.js';
const store=()=>{const m=new Map();return {getItem:k=>m.has(k)?m.get(k):null,setItem:(k,v)=>m.set(k,String(v))}};
const offline=()=>Object.assign(new Error('offline'),{offline:true});
test('enqueue keeps scans and ignores exact duplicates',()=>{const s=store();assert.equal(enqueue(s,{event_id:1,token:'a'}),1);assert.equal(enqueue(s,{event_id:1,token:'a'}),1);assert.equal(enqueue(s,{event_id:1,token:'b'}),2);assert.equal(enqueue(s,{event_id:2,token:'a'}),3)});
test('flush uploads everything in order and empties the queue',async()=>{const s=store();enqueue(s,{event_id:1,token:'a'});enqueue(s,{event_id:1,token:'b'});const sent=[];
 const r=await flush(s,async i=>{sent.push(i.token);return {ok:true}});assert.deepEqual(sent,['a','b']);assert.equal(r.done.length,2);assert.equal(r.left,0);assert.equal(load(s).length,0)});
test('flush stops at the first network failure and keeps the rest',async()=>{const s=store();for(const t of ['a','b','c'])enqueue(s,{event_id:1,token:t});
 const r=await flush(s,async i=>{if(i.token==='b')throw offline();return {}});assert.equal(r.done.length,1);assert.equal(r.left,2);assert.deepEqual(load(s).map(x=>x.token),['b','c'])});
test('flush drops scans the server refused and reports them',async()=>{const s=store();enqueue(s,{event_id:1,token:'bad'});enqueue(s,{event_id:1,token:'ok'});
 const r=await flush(s,async i=>{if(i.token==='bad')throw new Error('This QR is for a different event');return {}});assert.equal(r.failed.length,1);assert.match(r.failed[0].error,/different event/);assert.equal(r.done.length,1);assert.equal(r.left,0)});
test('a corrupted queue is ignored instead of crashing',()=>{const s=store();s.setItem(KEY,'{not json');assert.deepEqual(load(s),[]);s.setItem(KEY,'[{"x":1},null,{"event_id":3,"token":"t"}]');assert.equal(load(s).length,1)});
