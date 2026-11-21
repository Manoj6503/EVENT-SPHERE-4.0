// Offline check-in queue for the organizer's scanner: when the venue has no signal, scans are kept on the phone and uploaded later.
// Storage is injected (localStorage in the browser, a Map in tests), so this file has no browser dependencies.
export const KEY='es_checkin_queue';
export function load(st){try{const a=JSON.parse(st.getItem(KEY)||'[]');return Array.isArray(a)?a.filter(x=>x&&x.event_id&&x.token):[]}catch{return []}}
function save(st,a){try{st.setItem(KEY,JSON.stringify(a))}catch{}}
export function enqueue(st,item){const a=load(st);if(!a.some(x=>x.event_id===item.event_id&&x.token===item.token)){a.push({event_id:item.event_id,token:item.token,at:Date.now()});save(st,a)}return a.length}
// send(item) resolves on success, rejects with err.offline=true when the network is down (keep the rest), or with any other error when the server refused it (drop and report).
export async function flush(st,send){const a=load(st),keep=[],done=[],failed=[];
 for(let i=0;i<a.length;i++){try{done.push({item:a[i],result:await send(a[i])})}catch(e){if(e&&e.offline){keep.push(...a.slice(i));break}failed.push({item:a[i],error:e&&e.message||'rejected'})}}
 save(st,keep);return {done,failed,left:keep.length}}
