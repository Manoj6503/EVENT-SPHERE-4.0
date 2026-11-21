import {useState,useEffect,useContext,useRef,useCallback} from 'react';import {QRCodeSVG} from 'qrcode.react';import {Html5Qrcode} from 'html5-qrcode';
import {api} from '../api.js';import {Ctx} from '../ctx.js';import {downloadCsv} from './Shared.jsx';import {enqueue,flush,load as loadQueue} from '../offlineQueue.js';

function Camera({onScan,onError}){const cb=useRef(onScan);cb.current=onScan;
 useEffect(()=>{const s=new Html5Qrcode('cam');let busy=false;
  const p=s.start({facingMode:'environment'},{fps:10,qrbox:240},t=>{if(busy)return;busy=true;cb.current(t).finally(()=>setTimeout(()=>{busy=false},2000))}).catch(e=>onError('Camera blocked, or the page is not HTTPS: '+e));
  return()=>{p.then(()=>s.stop()).catch(()=>{})}},[]);
 return <div id="cam" style={{maxWidth:360,margin:'12px auto'}}/>}

export default function Attendance(){const {me,toast,go}=useContext(Ctx);const [auth,setAuth]=useState(null);
 const [events,setEvents]=useState(null),[eid,setEid]=useState(''),[mode,setMode]=useState('qr'),[qr,setQr]=useState(null),[parts,setParts]=useState([]),[res,setRes]=useState(null),[manual,setManual]=useState(''),[live,setLive]=useState(null),[pending,setPending]=useState(()=>loadQueue(localStorage).length);
 useEffect(()=>{if(me.role==='organizer')api('/organizer/certificate-authority').then(setAuth).catch(()=>{})},[]);
 useEffect(()=>{const pick=ev=>{setEvents(ev);if(ev[0])setEid(String(ev[0].id))};api(me.role==='admin'?'/admin/events':'/my/events').then(r=>{const ev=r.events.filter(e=>e.status==='published');try{localStorage.setItem('es_events',JSON.stringify(ev))}catch{}pick(ev)})
  .catch(e=>{let c=null;try{c=JSON.parse(localStorage.getItem('es_events')||'null')}catch{}if(e.offline&&c){pick(c);toast('Offline: using your saved event list')}else toast(e.message,1)})},[]);
 const loadParts=useCallback(()=>{if(eid)api(`/events/${eid}/participants`).then(r=>setParts(r.participants)).catch(()=>{})},[eid]);
 const loadQr=useCallback(()=>{if(eid)api(`/events/${eid}/checkin-qr`).then(setQr).catch(e=>{if(!e.offline)toast(e.message,1)})},[eid]);
 useEffect(()=>{loadParts();loadQr();const a=setInterval(loadParts,30000),b=setInterval(loadQr,60000);return()=>{clearInterval(a);clearInterval(b)}},[eid]); // 30 s polling is only the fallback; the live stream below updates instantly
 // live attendance: the server pushes a message the moment anyone checks in, registers or cancels (Server-Sent Events)
 useEffect(()=>{if(!eid||!window.EventSource)return;setLive(null);const es=new EventSource(`/api/events/${eid}/live`);es.onmessage=m=>{try{setLive(JSON.parse(m.data))}catch{}loadParts()};return()=>es.close()},[eid]);
 // offline check-ins are uploaded automatically as soon as the connection is back
 const sync=useCallback(async()=>{if(!loadQueue(localStorage).length)return;const r=await flush(localStorage,item=>api('/attendance/check','POST',item));setPending(r.left);
  if(r.done.length){toast(`${r.done.length} offline check-in${r.done.length>1?'s':''} uploaded`);loadParts()}if(r.failed.length)toast(`${r.failed.length} offline check-in${r.failed.length>1?'s were':' was'} rejected: ${r.failed[0].error}`,1)},[loadParts]);
 useEffect(()=>{sync();addEventListener('online',sync);const t=setInterval(sync,15000);return()=>{removeEventListener('online',sync);clearInterval(t)}},[sync]);
 const check=async t=>{try{const j=await api('/attendance/check','POST',{event_id:+eid,token:t.trim()});const ok=j.result==='ok';
  setRes({kind:ok?'ok':'warn',text:`${ok?'✅ Checked in':'⚠️ Already checked in'}: ${j.name} (${j.reg_code})${j.certificate&&ok?' — certificate issued':''}${j.certificate_pending?' — certificate will be issued once you set the certificate authority':''}`});loadParts()}catch(e){if(e.offline&&t.trim()){setPending(enqueue(localStorage,{event_id:+eid,token:t.trim()}));setRes({kind:'warn',text:'📴 No connection. This check-in is saved on this phone and will upload automatically.'})}else setRes({kind:'err',text:'❌ '+e.message})}};
 const resend=async id=>{try{toast((await api(`/certificates/${id}/resend`,'POST')).message);setTimeout(loadParts,3000)}catch(e){toast(e.message,1)}};
 const csv=()=>{const rows=[['Name','Email','RegID','Status','CheckedIn'],...parts.map(x=>[x.name,x.email,x.reg_code,x.status,x.checked_in_at||''])];
  downloadCsv('attendance.csv',rows)};
 if(!events)return <main><div className="sk"/></main>;
 if(!events.length)return <main><h2>Attendance</h2><div className="empty">No published events yet. Events appear here after the admin approves them.</div></main>;
 const colors={ok:'#dcfce7;#166534',warn:'#fef9c3;#854d0e',err:'#ffe4e6;#9f1239'};
 return <main><h2>Attendance</h2>{pending>0&&<div className="notice warn" role="status">📴 {pending} check-in{pending>1?'s':''} waiting to upload (no connection). They upload automatically when you are back online.</div>}{auth&&!auth.configured&&<div className="notice warn" role="alert">Certificates are on hold: no certificate authority is set up yet. <button className="o" onClick={()=>go('dash',{tab:'authority'})}>Set up certificate authority</button></div>}<div className="panel" style={{maxWidth:'none',textAlign:'center'}}>
  <label style={{textAlign:'left'}}>Event<select value={eid} onChange={e=>{setEid(e.target.value);setQr(null)}}>{events.map(e=><option key={e.id} value={e.id}>{e.title} ({e.event_date})</option>)}</select></label>
  <div style={{margin:'14px auto'}}>{mode==='qr'?(qr?<><h3>{qr.title}</h3><div className="qrbox"><QRCodeSVG value={`${location.origin}/#in=${eid}.${qr.token}`} size={280}/></div>
   <p><b>Students:</b> scan this QR with your phone camera and log in. Attendance and your certificate are issued instantly.</p>
   {!qr.is_today&&<p style={{color:'#be123c',fontWeight:700}}>Attendance counts only on the event date ({qr.event_date}).</p>}</>:<div className="sk"/>)
   :<Camera onScan={check} onError={m=>{toast(m,1);setMode('qr')}}/>}</div>
  <button onClick={()=>setMode('qr')}>Show attendance QR</button> <button className="o" onClick={()=>setMode('cam')}>Scan student passes with camera</button>
  {res&&<div className="res" style={{background:colors[res.kind].split(';')[0],color:colors[res.kind].split(';')[1]}}>{res.text}</div>}
  <div className="two" style={{marginTop:14}}><input placeholder="Or type registration ID (REG-...)" value={manual} onChange={e=>setManual(e.target.value)}/><button onClick={()=>check(manual)}>Check in</button></div></div>
  <h2>Participants</h2>{live&&<div className="meta">🟢 Live · {live.present} present of {live.registered} registered{live.recent[0]&&<> · latest: {live.recent[0].name}</>}</div>}{parts.length?<><p>{parts.filter(x=>x.status==='confirmed').length} registered · {parts.filter(x=>x.checked_in_at).length} present <button className="o" onClick={csv}>Export CSV</button></p>
  <table><thead><tr><th>Name</th><th>Reg ID</th><th>Status</th><th>Attendance</th><th>Certificate</th></tr></thead><tbody>{parts.map(x=><tr key={x.reg_code}><td>{x.name}</td><td>{x.reg_code}</td><td className="st">{x.status}</td><td>{x.checked_in_at?'Present '+x.checked_in_at:'-'}</td>
   <td>{x.cert_id?<>{x.cert_id} ({x.email_status}) {x.email_status!=='sent'&&<button className="o" onClick={()=>resend(x.cert_id)}>Retry email</button>}</>:x.checked_in_at?<small>On hold: set the certificate authority</small>:'-'}</td></tr>)}</tbody></table></>:<div className="empty">No registrations yet.</div>}</main>}
