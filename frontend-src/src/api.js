// Every call goes to the C++ REST API; the session cookie is sent automatically.
// Non-GET calls also echo the per-session CSRF token (cookie "csrf") in the X-CSRF-Token header.
const csrf=()=>{const m=document.cookie.match(/(?:^|;\s*)csrf=([^;]+)/);return m?m[1]:''};
export async function api(path,method='GET',body){
 const h={'Content-Type':'application/json'};if(method!=='GET'){const t=csrf();if(t)h['X-CSRF-Token']=t}
 let r;try{r=await fetch('/api'+path,{method,headers:h,body:body?JSON.stringify(body):undefined})}
 catch{throw Object.assign(new Error('You are offline or the server cannot be reached'),{offline:true})}
 const j=await r.json().catch(()=>({}));if(!r.ok)throw new Error(j.error||'Request failed');return j}
