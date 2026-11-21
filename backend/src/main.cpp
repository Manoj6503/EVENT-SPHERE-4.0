#include "core.h"
#include <condition_variable>
#include <atomic>
#include <deque>
struct Up{const string*s;size_t p=0;};
size_t rdcb(char*b,size_t sz,size_t n,void*u){Up*x=(Up*)u;size_t m=min(sz*n,x->s->size()-x->p);memcpy(b,x->s->data()+x->p,m);x->p+=m;return m;}
// Real SMTP delivery through libcurl. Returns "sent", "test_mode" (no SMTP configured) or an error text.
string sendMail(const string&to,const string&name,const string&ev,const string&pdf,const string&fname){
 string url=genv("SMTP_URL"),user=genv("SMTP_USER"),pass=genv("SMTP_PASS"),from=genv("SMTP_FROM",user.c_str());
 if(url.empty())return "test_mode";
 string bd="ES"+rnd(8);
 string msg="From: EventSphere <"+from+">\r\nTo: "+to+"\r\nSubject: Your EventSphere Participation Certificate\r\nMIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=\""+bd+"\"\r\n\r\n--"+bd+"\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nHi "+name+",\r\n\r\nThank you for attending "+ev+". Your participation certificate is attached.\r\n\r\nRegards,\r\nEventSphere\r\n\r\n--"+bd+"\r\nContent-Type: application/pdf; name=\""+fname+"\"\r\nContent-Transfer-Encoding: base64\r\nContent-Disposition: attachment; filename=\""+fname+"\"\r\n\r\n"+b64(pdf)+"\r\n--"+bd+"--\r\n";
 CURL*c=curl_easy_init();if(!c)return "curl init failed";Up up{&msg};
 curl_slist*rc=curl_slist_append(nullptr,("<"+to+">").c_str());string mf="<"+from+">";
 curl_easy_setopt(c,CURLOPT_URL,url.c_str());curl_easy_setopt(c,CURLOPT_USERNAME,user.c_str());curl_easy_setopt(c,CURLOPT_PASSWORD,pass.c_str());
 curl_easy_setopt(c,CURLOPT_MAIL_FROM,mf.c_str());curl_easy_setopt(c,CURLOPT_MAIL_RCPT,rc);curl_easy_setopt(c,CURLOPT_USE_SSL,(long)CURLUSESSL_ALL);
 curl_easy_setopt(c,CURLOPT_READFUNCTION,rdcb);curl_easy_setopt(c,CURLOPT_READDATA,&up);curl_easy_setopt(c,CURLOPT_UPLOAD,1L);curl_easy_setopt(c,CURLOPT_TIMEOUT,40L);
 CURLcode r=curl_easy_perform(c);curl_slist_free_all(rc);curl_easy_cleanup(c);
 return r==CURLE_OK?"sent":string(curl_easy_strerror(r));}
int main(){
 auto env=[](const char*k,const char*d){const char*v=getenv(k);return string(v?v:d);};
 Db db(env("DB_PATH","database/eventsphere.db"));
 db.exec("PRAGMA foreign_keys=ON");db.exec("PRAGMA busy_timeout=5000");
 { // ---- versioned migrations: database/migrations/NNN_name.sql, each applied once and recorded in schema_migrations ----
  db.exec("CREATE TABLE IF NOT EXISTS schema_migrations(version INTEGER PRIMARY KEY,name TEXT NOT NULL,applied_at TEXT DEFAULT(datetime('now')))");
  string mdir=env("MIGRATIONS_DIR","database/migrations");vector<pair<int,filesystem::path>> files;
  if(filesystem::exists(mdir))for(auto&e:filesystem::directory_iterator(mdir)){string n=e.path().filename().string();if(e.path().extension()==".sql"&&!n.empty()&&isdigit((unsigned char)n[0]))files.push_back({stoi(n),e.path()});}
  sort(files.begin(),files.end());
  if(files.empty()){cerr<<"No migrations found in "<<mdir<<". Run the server from the project root folder."<<endl;return 1;}
  bool hasUsers=!db.q("SELECT name FROM sqlite_master WHERE type='table' AND name='users'").empty();
  if(hasUsers&&db.q("SELECT COUNT(*) c FROM schema_migrations")[0]["c"].get<long long>()==0){
   // database created by an older EventSphere (before migrations): add the columns it may lack; the baseline then runs harmlessly (CREATE ... IF NOT EXISTS)
   for(const char*m:{"ALTER TABLE registrations ADD COLUMN qr_token TEXT","ALTER TABLE events ADD COLUMN form_type TEXT NOT NULL DEFAULT ''","ALTER TABLE events ADD COLUMN form_fields TEXT NOT NULL DEFAULT '[]'",
    "ALTER TABLE registrations ADD COLUMN answers TEXT NOT NULL DEFAULT '[]'","ALTER TABLE certificates ADD COLUMN signed INTEGER NOT NULL DEFAULT 0"})try{db.exec(m);}catch(...){}}
  for(auto&f:files){if(!db.q("SELECT 1 x FROM schema_migrations WHERE version=?",{to_string(f.first)}).empty())continue;
   ifstream in(f.second);stringstream ss;ss<<in.rdbuf();db.exec("BEGIN IMMEDIATE");
   try{db.exec(ss.str());db.q("INSERT INTO schema_migrations(version,name) VALUES(?,?)",{to_string(f.first),f.second.filename().string()});db.exec("COMMIT");}
   catch(exception&e){try{db.exec("ROLLBACK");}catch(...){}cerr<<"Migration "<<f.second.filename().string()<<" failed: "<<e.what()<<endl;return 1;}
   cout<<"Applied migration "<<f.second.filename().string()<<endl;}}
 // organizers that existed before the approval workflow are grandfathered in as approved
 db.exec("INSERT INTO organizer_requests(user_id,event_title,status,admin_note,reviewed_at) SELECT id,'Existing organizer account','approved','Approved before the request workflow existed',datetime('now') FROM users WHERE role='organizer' AND id NOT IN (SELECT user_id FROM organizer_requests)");
 db.exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_reg_qr ON registrations(qr_token)");
 for(auto&r:db.q("SELECT id FROM registrations WHERE qr_token IS NULL"))db.q("UPDATE registrations SET qr_token=? WHERE id=?",{rnd(16),to_string(r["id"].get<long long>())});
 string ae=lowerStr(env("ADMIN_EMAIL","")),ap=env("ADMIN_PASSWORD","");
 if(!ae.empty()&&ap.size()>=8&&db.q("SELECT id FROM users WHERE lower(email)=?",{ae}).empty())
  db.q("INSERT INTO users(name,email,password_hash,role) VALUES('Administrator',?,?,'admin')",{ae,hashPw(ap,rnd(16))});
 map<string,pair<int,time_t>> loginFails;map<string,deque<time_t>> ipHits;mutex mu;Server svr;
 // live attendance: any change bumps a counter and wakes the Server-Sent-Events streams
 mutex liveMu;condition_variable liveCv;long long liveVer=0;atomic<int> liveConns{0};
 auto liveBump=[&]{{lock_guard<mutex> l(liveMu);liveVer++;}liveCv.notify_all();};
 // per-IP limiter (X-Forwarded-For is used only when TRUST_PROXY=1, i.e. behind Render/Cloudflare)
 const bool trustProxy=env("TRUST_PROXY","0")=="1";const size_t loginIpMax=max(1,atoi(env("LOGIN_LIMIT_PER_10MIN","300").c_str())),signupIpMax=max(1,atoi(env("SIGNUP_LIMIT_PER_HOUR","100").c_str())); // generous: a whole class may share one campus IP
 auto limited=[&](const Request&q,const string&bucket,size_t maxHits,int windowSec){string ip=q.remote_addr;if(trustProxy){string xf=q.get_header_value("X-Forwarded-For");if(!xf.empty())ip=xf.substr(0,xf.find(','));}
  if(ipHits.size()>5000)ipHits.clear();auto&d=ipHits[bucket+"|"+ip];time_t now=time(nullptr);while(!d.empty()&&now-d.front()>windowSec)d.pop_front();if(d.size()>=maxHits)return true;d.push_back(now);return false;};
 const string UTCOFF=to_string(atoi(env("UTC_OFFSET_MIN","330").c_str())),OFFS=(UTCOFF[0]=='-'?"":"+")+UTCOFF; // sign handled, so negative offsets (Americas) work too
 const string TODAY="date('now','"+OFFS+" minutes')";
 string certDir=env("CERT_DIR","certificates");filesystem::create_directories(certDir);curl_global_init(CURL_GLOBAL_DEFAULT);
 const string secret=rnd(32);
 // short-lived (60 s window) signed token shown in the organizer's live event QR
 auto ctok=[&](const string&eid,long long w){string d=eid+":"+to_string(w);unsigned char o[32];unsigned n=0;HMAC(EVP_sha256(),secret.data(),secret.size(),(const unsigned char*)d.data(),d.size(),o,&n);return hex(o,10);};
 // ---- in-app notifications ----
 auto notify=[&](const string&uid,const string&m){db.q("INSERT INTO notifications(user_id,message) VALUES(?,?)",{uid,m});};
 auto notifyRole=[&](const string&role,const string&m,const string&except){db.q("INSERT INTO notifications(user_id,message) SELECT id,? FROM users WHERE role=? AND id<>?",{m,role,except});};
 // ---- certificate email runs in a background thread so scanning stays instant ----
 auto deliver=[&](string cid,string to,string name,string ev,string pdf){thread([&db,&mu,cid,to,name,ev,pdf]{string st=sendMail(to,name,ev,pdf,cid+".pdf"),err;
  if(st!="sent"&&st!="test_mode"){err=st;st="failed";}lock_guard<mutex> l(mu);try{db.q("UPDATE certificates SET email_status=?,email_error=? WHERE cert_id=?",{st,err,cid});}catch(...){}}).detach();};
 auto auth=[&](const Request&q)->unique_ptr<User>{string sid=getSid(q.get_header_value("Cookie"));if(sid.empty())return nullptr;
  auto r=db.q("SELECT u.id,u.name,u.email,u.role FROM sessions s JOIN users u ON u.id=s.user_id WHERE s.token_hash=? AND s.expires_at>?",{sha256hex(sid),to_string((long long)time(nullptr))});return r.empty()?nullptr:makeUser(r[0]);};
 auto need=[&](const Request&q,function<bool(const User&)> ok){auto u=auth(q);if(!u)throw HttpErr{401,"Please log in"};if(!ok(*u))throw HttpErr{403,"You are not allowed to do this"};return u;};
 auto route=[&](bool post,const char*p,function<json(const Request&,json&)> fn){
  string path=p;auto h=[&,fn,path,post](const Request&q,Response&r){lock_guard<mutex> l(mu);
   auto fail=[&](int c,const string&m){try{db.exec("ROLLBACK");}catch(...){}r.status=c;r.set_content(json{{"error",m}}.dump(),"application/json");};
   try{if(post&&path!="/api/login"&&path!="/api/signup"){ // CSRF: a logged-in browser must echo the per-session token in X-CSRF-Token
     string sid=getSid(q.get_header_value("Cookie"));if(!sid.empty()){auto cs=db.q("SELECT csrf FROM sessions WHERE token_hash=? AND expires_at>?",{sha256hex(sid),to_string((long long)time(nullptr))});
      if(!cs.empty()){string want=cs[0]["csrf"].get<string>(),got=q.get_header_value("X-CSRF-Token");if(want.size()!=got.size()||CRYPTO_memcmp(want.data(),got.data(),want.size())!=0)throw HttpErr{403,"Security check failed. Please refresh the page and try again"};}}}
    json b=json::object();if(!q.body.empty()){b=json::parse(q.body,nullptr,false);if(b.is_discarded())throw HttpErr{400,"Invalid JSON"};}
    json o=fn(q,b);if(o.contains("_raw")){r.status=200;r.set_header("Content-Disposition",o["_name"].get<string>());r.set_content(o["_raw"].get<string>(),o.value("_ctype",string("application/pdf")));return;}r.status=o.value("_status",200);if(o.contains("_cookies"))for(auto&c:o["_cookies"])r.set_header("Set-Cookie",c.get<string>());
    o.erase("_status");o.erase("_cookies");r.set_content(o.dump(),"application/json");}
   catch(HttpErr&x){fail(x.code,x.msg);} catch(exception&x){cerr<<"error: "<<x.what()<<endl;fail(500,"Server error");}};
  if(post)svr.Post(p,h);else svr.Get(p,h);};
 auto str=[](json&b,const char*k){return b.contains(k)&&b[k].is_string()?b[k].get<string>():string();};
 const string EV="SELECT e.*,u.name organizer,(SELECT COUNT(*) FROM registrations r WHERE r.event_id=e.id AND r.status='confirmed') taken FROM events e JOIN users u ON u.id=e.organizer_id ";
 // ---- auth ----
 route(true,"/api/signup",[&](const Request&q,json&b){if(limited(q,"signup",signupIpMax,3600))throw HttpErr{429,"Too many sign-ups from this network. Try again later"};string n=str(b,"name"),e=lowerStr(str(b,"email")),p=str(b,"password"),r="student"; // role from the client is ignored: organizer status is granted only by an admin
  if(n.empty()||e.find('@')==string::npos||e.find_first_of(" \r\n<>")!=string::npos)throw HttpErr{400,"Name and a valid email are required"};
  if(n.size()>100||e.size()>150)throw HttpErr{400,"Name or email is too long"};
  if(p.size()<8)throw HttpErr{400,"Password must be at least 8 characters"};
  if(!db.q("SELECT id FROM users WHERE lower(email)=?",{e}).empty())throw HttpErr{409,"Email already registered"};
  db.q("INSERT INTO users(name,email,password_hash,role) VALUES(?,?,?,?)",{n,e,hashPw(p,rnd(16)),r});return json{{"_status",201},{"message","Account created"}};});
 route(true,"/api/login",[&](const Request&q,json&b){if(limited(q,"login",loginIpMax,600))throw HttpErr{429,"Too many attempts from this network. Try again in a few minutes"};string em=lowerStr(str(b,"email"));time_t now=time(nullptr);
  if(loginFails.size()>5000)loginFails.clear();auto&lf=loginFails[em];if(now-lf.second>600)lf={0,now};
  if(lf.first>=10)throw HttpErr{429,"Too many failed attempts. Try again in a few minutes"};
  auto r=db.q("SELECT * FROM users WHERE lower(email)=?",{em});
  if(r.empty()||!checkPw(str(b,"password"),r[0]["password_hash"])){if(lf.first==0)lf.second=now;lf.first++;throw HttpErr{401,"Invalid email or password"};}
  loginFails.erase(em);string t=rnd(32),cs=rnd(16),uid=to_string(r[0]["id"].get<long long>());bool https=q.get_header_value("X-Forwarded-Proto")=="https";
  db.q("DELETE FROM sessions WHERE expires_at<?",{to_string((long long)now)});
  db.q("INSERT INTO sessions(token_hash,user_id,csrf,expires_at) VALUES(?,?,?,?)",{sha256hex(t),uid,cs,to_string((long long)now+7*24*3600)});
  db.q("DELETE FROM sessions WHERE user_id=? AND rowid NOT IN (SELECT rowid FROM sessions WHERE user_id=? ORDER BY rowid DESC LIMIT 10)",{uid,uid}); // at most 10 devices per user
  string sec=https?"; Secure":"";
  return json{{"_cookies",json::array({"sid="+t+"; Path=/; Max-Age=604800; HttpOnly; SameSite=Strict"+sec,"csrf="+cs+"; Path=/; Max-Age=604800; SameSite=Strict"+sec})},{"user",{{"name",r[0]["name"]},{"role",r[0]["role"]}}}};});
 route(true,"/api/logout",[&](const Request&q,json&){string sid=getSid(q.get_header_value("Cookie"));if(!sid.empty())db.q("DELETE FROM sessions WHERE token_hash=?",{sha256hex(sid)});
  return json{{"_cookies",json::array({"sid=; Path=/; Max-Age=0","csrf=; Path=/; Max-Age=0"})},{"ok",true}};});
 route(false,"/api/me",[&](const Request&q,json&){auto u=auth(q);if(!u)return json{{"user",nullptr}};return json{{"user",{{"name",u->name},{"email",u->email},{"role",u->role}}}};});
 // ---- events ----
 static const set<string> cats={"Technical","Workshop","Seminar","Cultural","Sports","Hackathon","Symposium","Other"};
 auto fields=[&](json&b){vector<string> v;
  for(auto k:{"title","description","category","banner","venue","event_date","start_time","end_time","reg_open","reg_close","capacity","eligibility","contact","form_type","form_fields"})
   v.push_back(b.contains(k)?(b[k].is_string()?b[k].get<string>():b[k].dump()):"");
  if(v[0].empty()||v[4].empty())throw HttpErr{400,"Title and venue are required"};
  if(!cats.count(v[2]))throw HttpErr{400,"Invalid category"};
  if(v[0].size()>150||v[4].size()>150||v[1].size()>3000)throw HttpErr{400,"Title, venue or description is too long"};
  if(!v[3].empty()&&(v[3].size()>2000||(v[3].rfind("http://",0)!=0&&v[3].rfind("https://",0)!=0)))throw HttpErr{400,"Banner must be an http(s) image link"};
  for(int i:{5,8,9})if(!validDate(v[i]))throw HttpErr{400,"Dates must be valid and in YYYY-MM-DD format"};
  if(v[8]>v[9]||v[9]>v[5])throw HttpErr{400,"Registration window must open before it closes, and close on or before the event date"};
  int cap=atoi(v[10].c_str());if(cap<1||cap>100000)throw HttpErr{400,"Capacity must be 1-100000"};v[10]=to_string(cap);
  if(!v[13].empty()&&!formTemplates().count(v[13]))throw HttpErr{400,"Invalid registration form type"};v[14]=normalizeCustom(v[14]);return v;};
 route(false,"/api/events",[&](const Request&q,json&){string sql=EV+"WHERE e.status='published'";vector<string> p;
  if(q.has_param("category")&&!q.get_param_value("category").empty()){sql+=" AND e.category=?";p.push_back(q.get_param_value("category"));}
  if(q.has_param("q")&&!q.get_param_value("q").empty()){sql+=" AND (e.title LIKE ? ESCAPE '\\' OR e.description LIKE ? ESCAPE '\\')";string s="%";for(char ch:q.get_param_value("q")){if(ch=='%'||ch=='_'||ch=='\\')s+='\\';s+=ch;}s+="%";p.push_back(s);p.push_back(s);}
  return json{{"events",db.q(sql+" ORDER BY e.event_date",p)}};});
 route(false,"/api/form-templates",[&](const Request&,json&){json t=json::object();for(auto&kv:formTemplates())t[kv.first]=kv.second;return json{{"common",commonFields()},{"templates",t}};});
 // one event + the registration form that belongs to it. Unpublished events are visible only to their organizer and admins.
 route(false,R"(/api/events/(\d+))",[&](const Request&q,json&){string id=q.matches[1];auto e=db.q(EV+"WHERE e.id=?",{id});if(e.empty())throw HttpErr{404,"Event not found"};
  auto u=auth(q);if(e[0]["status"]!="published"&&(!u||(u->role!="admin"&&u->id!=e[0]["organizer_id"].get<long long>())))throw HttpErr{404,"Event not found"};
  json o=e[0];o["open"]=db.q("SELECT reg_open<="+TODAY+" AND reg_close>="+TODAY+" open FROM events WHERE id=?",{id})[0]["open"];
  o["form"]=formFields(o["category"],o["form_type"].is_null()?"":o["form_type"].get<string>(),o["form_fields"].is_null()?"[]":o["form_fields"].get<string>());
  o["my_registration"]=nullptr;if(u){auto r=db.q("SELECT reg_code,status FROM registrations WHERE event_id=? AND student_id=?",{id,to_string(u->id)});if(!r.empty())o["my_registration"]=r[0];}
  return json{{"event",o}};});
 route(true,"/api/events",[&](const Request&q,json&b){auto u=need(q,[](const User&x){return x.canManageEvents();});auto v=fields(b);v.insert(v.begin(),to_string(u->id));
  db.exec("BEGIN IMMEDIATE");db.q("INSERT INTO events(organizer_id,title,description,category,banner,venue,event_date,start_time,end_time,reg_open,reg_close,capacity,eligibility,contact,form_type,form_fields) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",v);
  db.q("INSERT INTO event_approval_history(event_id,actor_id,action) VALUES(?,?,'submitted')",{to_string(db.lastId()),to_string(u->id)});notifyRole("admin","New event proposal: "+v[1]+" by "+u->name,"0");db.exec("COMMIT");
  return json{{"_status",201},{"message","Proposal submitted for approval"}};});
 route(true,R"(/api/events/(\d+)/update)",[&](const Request&q,json&b){auto u=need(q,[](const User&x){return x.role=="organizer";});string id=q.matches[1];auto v=fields(b);
  if(db.q("SELECT id FROM events WHERE id=? AND organizer_id=?",{id,to_string(u->id)}).empty())throw HttpErr{404,"Event not found"};
  {long long tk=db.q("SELECT COUNT(*) c FROM registrations WHERE event_id=? AND status='confirmed'",{id})[0]["c"].get<long long>();if(atoll(v[10].c_str())<tk)throw HttpErr{409,"Capacity cannot be lower than the "+to_string(tk)+" students already registered"};}
  v.push_back(id);db.exec("BEGIN IMMEDIATE");
  db.q("UPDATE events SET title=?,description=?,category=?,banner=?,venue=?,event_date=?,start_time=?,end_time=?,reg_open=?,reg_close=?,capacity=?,eligibility=?,contact=?,form_type=?,form_fields=?,status='pending',updated_at=datetime('now') WHERE id=?",v);
  db.q("INSERT INTO event_approval_history(event_id,actor_id,action) VALUES(?,?,'resubmitted')",{id,to_string(u->id)});notifyRole("admin","Event updated for re-review: "+v[0],"0");db.exec("COMMIT");return json{{"message","Event updated and sent for re-review"}};});
 route(false,"/api/my/events",[&](const Request&q,json&){auto u=need(q,[](const User&x){return x.role=="organizer";});
  return json{{"events",db.q(EV+"WHERE e.organizer_id=? ORDER BY e.id DESC",{to_string(u->id)})}};});
 route(false,"/api/admin/events",[&](const Request&q,json&){need(q,[](const User&x){return x.canReview();});
  return json{{"events",db.q(EV+"ORDER BY (e.status='pending') DESC,e.id DESC")}};});
 route(true,R"(/api/admin/events/(\d+)/review)",[&](const Request&q,json&b){auto u=need(q,[](const User&x){return x.canReview();});string id=q.matches[1],a=str(b,"action"),why=str(b,"reason");
  static const map<string,string> st={{"approve","published"},{"reject","rejected"},{"changes","changes_requested"}};
  if(!st.count(a))throw HttpErr{400,"Action must be approve, reject or changes"};if(a!="approve"&&why.empty())throw HttpErr{400,"A reason is required"};
  auto ev=db.q("SELECT title,organizer_id,event_date FROM events WHERE id=?",{id});if(ev.empty())throw HttpErr{404,"Event not found"};
  db.exec("BEGIN IMMEDIATE");db.q("UPDATE events SET status=? WHERE id=?",{st.at(a),id});
  db.q("INSERT INTO event_approval_history(event_id,actor_id,action,reason) VALUES(?,?,?,?)",{id,to_string(u->id),a,why});{string T=ev[0]["title"],org=to_string(ev[0]["organizer_id"].get<long long>());
   notify(org,"Your event \""+T+"\" was "+(a=="approve"?string("approved and published"):a=="reject"?"rejected: "+why:"sent back for changes: "+why));
   if(a=="approve"){string m="New event: "+T+" on "+ev[0]["event_date"].get<string>();notifyRole("student",m,"0");notifyRole("organizer",m,org);}}
  db.exec("COMMIT");return json{{"message","Event "+st.at(a)}};});
 route(false,"/api/admin/stats",[&](const Request&q,json&){need(q,[](const User&x){return x.canReview();});
  return json{{"stats",db.q("SELECT (SELECT COUNT(*) FROM users) users,(SELECT COUNT(*) FROM organizer_requests WHERE status='pending') organizer_requests,(SELECT COUNT(*) FROM events WHERE status='pending') pending,(SELECT COUNT(*) FROM events WHERE status='published') published,(SELECT COUNT(*) FROM registrations WHERE status='confirmed') registrations,(SELECT COUNT(*) FROM attendance_records) attended")[0]},
   {"byCategory",db.q("SELECT e.category,COUNT(r.id) n FROM events e LEFT JOIN registrations r ON r.event_id=e.id AND r.status='confirmed' GROUP BY e.category")}};});
 // ---- registration (capacity enforced inside one IMMEDIATE transaction) ----
 route(true,R"(/api/events/(\d+)/register)",[&](const Request&q,json&b){auto u=need(q,[](const User&x){return x.canRegister();});string id=q.matches[1],uid=to_string(u->id);
  db.exec("BEGIN IMMEDIATE");
  auto e=db.q("SELECT capacity,status,organizer_id,title,category,form_type,form_fields,reg_open<="+TODAY+" AND reg_close>="+TODAY+" open FROM events WHERE id=?",{id});
  if(e.empty()||e[0]["status"]!="published")throw HttpErr{404,"Event not available"};
  if(e[0]["open"]!=1)throw HttpErr{409,"Registration is not open for this event"};
  auto ex=db.q("SELECT id,status FROM registrations WHERE event_id=? AND student_id=?",{id,uid});
  if(!ex.empty()&&ex[0]["status"]=="confirmed")throw HttpErr{409,"You are already registered"};
  if(db.q("SELECT COUNT(*) c FROM registrations WHERE event_id=? AND status='confirmed'",{id})[0]["c"].get<long long>()>=e[0]["capacity"].get<long long>())throw HttpErr{409,"This event is full"};
  // answers are validated against THIS event's form (category template + organizer's custom questions)
  string answers=validateAnswers(formFields(e[0]["category"],e[0]["form_type"].get<string>(),e[0]["form_fields"].get<string>()),b.contains("answers")?b["answers"]:json::object()).dump();
  string code;do{code="REG-"+rnd(4);for(auto&c:code)c=toupper(c);}while(!db.q("SELECT 1 x FROM registrations WHERE reg_code=?",{code}).empty());
  if(ex.empty())db.q("INSERT INTO registrations(reg_code,event_id,student_id,qr_token,answers) VALUES(?,?,?,?,?)",{code,id,uid,rnd(16),answers});
  else db.q("UPDATE registrations SET status='confirmed',reg_code=?,qr_token=?,answers=? WHERE id=?",{code,rnd(16),answers,to_string(ex[0]["id"].get<long long>())});
  notify(to_string(e[0]["organizer_id"].get<long long>()),u->name+" registered for "+e[0]["title"].get<string>());
  notify(uid,"Registration confirmed for \""+e[0]["title"].get<string>()+"\". Your registration ID is "+code+".");db.exec("COMMIT");liveBump();return json{{"_status",201},{"reg_code",code},{"message","Registration confirmed"}};});
 route(true,R"(/api/registrations/(\d+)/cancel)",[&](const Request&q,json&){auto u=need(q,[](const User&x){return x.canRegister();});
  if(db.q("SELECT id FROM registrations WHERE id=? AND student_id=? AND status='confirmed'",{q.matches[1],to_string(u->id)}).empty())throw HttpErr{404,"Registration not found"};
  if(!db.q("SELECT 1 x FROM attendance_records WHERE registration_id=?",{q.matches[1]}).empty())throw HttpErr{409,"Attendance is already marked, so this registration can no longer be cancelled"};
  db.q("UPDATE registrations SET status='cancelled' WHERE id=?",{q.matches[1]});liveBump();return json{{"message","Registration cancelled"}};});
 route(false,"/api/my/registrations",[&](const Request&q,json&){auto u=need(q,[](const User&x){return x.canRegister();});
  return json{{"registrations",db.q("SELECT r.id,r.event_id,r.reg_code,r.qr_token,r.status,e.title,e.venue,e.event_date,e.start_time,a.checked_in_at,c.cert_id,c.email_status,e.event_date="+TODAY+" is_today FROM registrations r JOIN events e ON e.id=r.event_id LEFT JOIN attendance_records a ON a.registration_id=r.id LEFT JOIN certificates c ON c.registration_id=r.id WHERE r.student_id=? ORDER BY e.event_date DESC",{to_string(u->id)})}};});
 auto owns=[&](const User&u,const string&eid){auto e=db.q("SELECT organizer_id FROM events WHERE id=?",{eid});if(e.empty())throw HttpErr{404,"Event not found"};if(u.role!="admin"&&e[0]["organizer_id"].get<long long>()!=u.id)throw HttpErr{403,"This is not your event"};};
 // ---- certificates: signed only with a configured authority (never a made-up signature) ----
 // The authority printed on an event's certificates is its ORGANIZER's own (Organizer Dashboard > Certificate Authority).
 // If that organizer has none, the optional site-wide default is used (admin API, or CERT_AUTHORITY_NAME / CERT_AUTHORITY_TITLE env vars).
 // With neither, certificates stay on hold: attendance is still recorded and the certificate is issued the moment an authority is saved.
 auto certSettings=[&]()->json{auto r=db.q("SELECT COALESCE(authority_name,'') authority_name,COALESCE(designation,'') designation,COALESCE(signature_b64,'') signature_b64 FROM certificate_settings WHERE id=1");
  if(!r.empty()&&!r[0]["authority_name"].get<string>().empty())return r[0];
  return json{{"authority_name",env("CERT_AUTHORITY_NAME","")},{"designation",env("CERT_AUTHORITY_TITLE","Authorized Signatory")},{"signature_b64",""}};};
 auto authorityFor=[&](const string&organizerId)->json{auto r=db.q("SELECT authority_name,designation,COALESCE(signature_b64,'') signature_b64 FROM organizer_certificate_settings WHERE user_id=? AND authority_name<>''",{organizerId});
  return r.empty()?certSettings():r[0];};
 auto sigReady=[&](const json&c){return !c["authority_name"].get<string>().empty();};
 using MailJob=array<string,5>;
 // Issues + stores a signed certificate for an attended registration. Returns the certificate ID, or "" when no authority is configured yet.
 auto issueCert=[&](const string&rid,vector<MailJob>&jobs)->string{
  auto d=db.q("SELECT u.id uid,u.name,u.email,e.title,e.event_date,e.organizer_id oid FROM registrations r JOIN users u ON u.id=r.student_id JOIN events e ON e.id=r.event_id WHERE r.id=?",{rid})[0];
  auto cs=authorityFor(to_string(d["oid"].get<long long>()));if(!sigReady(cs))return "";
  string cid;do{cid="CERT-"+rnd(5);for(auto&ch:cid)ch=toupper(ch);}while(!db.q("SELECT 1 x FROM certificates WHERE cert_id=?",{cid}).empty());string issued=db.q("SELECT "+TODAY+" d")[0]["d"]; // issue date in the college time zone, not UTC
  string pdf=makePdf(d["name"],d["title"],d["event_date"],cid,env("COLLEGE_NAME","Your College Name"),issued,cs["authority_name"],cs["designation"],cs["signature_b64"]),path=certDir+"/"+cid+".pdf";{ofstream f(path,ios::binary);f<<pdf;}
  db.q("INSERT INTO certificates(cert_id,registration_id,file_path,signed) VALUES(?,?,?,1)",{cid,rid,path});
  notify(to_string(d["uid"].get<long long>()),"Attendance marked for \""+d["title"].get<string>()+"\". Your certificate is ready ("+cid+").");
  jobs.push_back({cid,d["email"],d["name"],d["title"],pdf});return cid;};
 // After an authority is saved (organizerId "" = every organizer): re-render certificates created without a valid signature block, then issue the ones that were waiting.
 auto flushPending=[&](const string&organizerId,int&redone,int&issuedNow,vector<MailJob>&jobs){string flt=organizerId.empty()?"":" AND e.organizer_id=?";vector<string> prm;if(!organizerId.empty())prm.push_back(organizerId);
  for(auto&c:db.q("SELECT c.cert_id,c.file_path,c.issued_at,date(c.issued_at,'"+OFFS+" minutes') issued_day,u.name,e.title,e.event_date,e.organizer_id oid FROM certificates c JOIN registrations r ON r.id=c.registration_id JOIN users u ON u.id=r.student_id JOIN events e ON e.id=r.event_id WHERE c.signed=0"+flt,prm)){
   auto cs=authorityFor(to_string(c["oid"].get<long long>()));if(!sigReady(cs))continue;
   string pdf=makePdf(c["name"],c["title"],c["event_date"],c["cert_id"],env("COLLEGE_NAME","Your College Name"),c["issued_day"].get<string>(),cs["authority_name"],cs["designation"],cs["signature_b64"]);
   {ofstream o(c["file_path"].get<string>(),ios::binary);o<<pdf;}db.q("UPDATE certificates SET signed=1 WHERE cert_id=?",{c["cert_id"].get<string>()});redone++;}
  for(auto&r:db.q("SELECT a.registration_id id FROM attendance_records a JOIN registrations r ON r.id=a.registration_id JOIN events e ON e.id=r.event_id LEFT JOIN certificates c ON c.registration_id=a.registration_id WHERE c.id IS NULL AND r.status='confirmed'"+flt,prm))
   if(!issueCert(to_string(r["id"].get<long long>()),jobs).empty())issuedNow++;};
 // ---- QR attendance: token (or manual reg ID) is validated against the DB inside one transaction ----
 auto doCheck=[&](string eid,string t,string up,long long by)->json{
  if(db.q("SELECT 1 x FROM events WHERE id=? AND event_date="+TODAY,{eid}).empty())throw HttpErr{409,"Check-in is only open on the event date"};
  db.exec("BEGIN IMMEDIATE");
  auto r=db.q("SELECT r.id,r.event_id,r.status,r.reg_code,u.name,e.title FROM registrations r JOIN users u ON u.id=r.student_id JOIN events e ON e.id=r.event_id WHERE r.qr_token=? OR r.reg_code=?",{t,up});
  if(r.empty())throw HttpErr{404,"Invalid QR code"};
  if(to_string(r[0]["event_id"].get<long long>())!=eid)throw HttpErr{409,"This QR is for a different event"};
  if(r[0]["status"]!="confirmed")throw HttpErr{409,"Registration was cancelled"};
  string rid=to_string(r[0]["id"].get<long long>());json o={{"name",r[0]["name"]},{"reg_code",r[0]["reg_code"]},{"event",r[0]["title"]}};
  auto a=db.q("SELECT checked_in_at FROM attendance_records WHERE registration_id=?",{rid});
  bool had=!a.empty();auto cc=db.q("SELECT cert_id FROM certificates WHERE registration_id=?",{rid});
  if(had&&!cc.empty()){db.exec("COMMIT");o["result"]="already";o["time"]=a[0]["checked_in_at"];o["certificate"]=cc[0]["cert_id"];return o;}
  if(!had)db.q("INSERT INTO attendance_records(registration_id,event_id,checked_in_by) VALUES(?,?,?)",{rid,eid,to_string(by)});
  vector<MailJob> jobs;string cid=issueCert(rid,jobs);
  if(cid.empty()){auto d=db.q("SELECT u.id uid,e.title FROM registrations r JOIN users u ON u.id=r.student_id JOIN events e ON e.id=r.event_id WHERE r.id=?",{rid})[0];
   if(!had)notify(to_string(d["uid"].get<long long>()),"Attendance marked for \""+d["title"].get<string>()+"\". Your certificate will be issued as soon as the organizer sets the certificate authority.");}
  db.exec("COMMIT");liveBump();for(auto&j:jobs)deliver(j[0],j[1],j[2],j[3],j[4]);o["result"]=had?"already":"ok";if(cid.empty())o["certificate_pending"]=true;else o["certificate"]=cid;return o;};
 route(true,"/api/attendance/check",[&](const Request&q,json&b){auto u=need(q,[](const User&x){return x.canManageEvents();});
  string eid=b.contains("event_id")?(b["event_id"].is_string()?b["event_id"].get<string>():b["event_id"].dump()):"",t=str(b,"token"),up=t;for(auto&c:up)c=toupper(c);
  owns(*u,eid);if(t.empty())throw HttpErr{400,"Scan a QR code or enter a registration ID"};
  if(db.q("SELECT 1 x FROM events WHERE id=? AND event_date="+TODAY,{eid}).empty())throw HttpErr{409,"Check-in is only open on the event date"};
  return doCheck(eid,t,up,u->id);});
 route(false,R"(/api/events/(\d+)/participants)",[&](const Request&q,json&){auto u=need(q,[](const User&x){return x.canManageEvents();});string eid=q.matches[1];owns(*u,eid);
  return json{{"participants",db.q("SELECT u.name,u.email,r.id,r.student_id,r.answers,r.created_at,r.reg_code,r.status,a.checked_in_at,c.cert_id,c.email_status FROM registrations r JOIN users u ON u.id=r.student_id LEFT JOIN attendance_records a ON a.registration_id=r.id LEFT JOIN certificates c ON c.registration_id=r.id WHERE r.event_id=? ORDER BY u.name",{eid})}};});
 route(false,"/api/notifications",[&](const Request&q,json&){auto u=need(q,[](const User&){return true;});string id=to_string(u->id);
  return json{{"notifications",db.q("SELECT id,message,is_read,created_at FROM notifications WHERE user_id=? ORDER BY id DESC LIMIT 30",{id})},{"unread",db.q("SELECT COUNT(*) c FROM notifications WHERE user_id=? AND is_read=0",{id})[0]["c"]}};});
 route(true,"/api/notifications/read",[&](const Request&q,json&){auto u=need(q,[](const User&){return true;});db.q("UPDATE notifications SET is_read=1 WHERE user_id=?",{to_string(u->id)});return json{{"ok",true}};});
 route(false,"/api/certificates/verify/(.+)",[&](const Request&q,json&){string id=q.matches[1];for(auto&ch:id)ch=toupper(ch);
  auto c=db.q("SELECT c.issued_at,u.name,e.title,e.event_date FROM certificates c JOIN registrations r ON r.id=c.registration_id JOIN users u ON u.id=r.student_id JOIN events e ON e.id=r.event_id WHERE c.cert_id=?",{id});
  if(c.empty())throw HttpErr{404,"No certificate found with this ID"};
  return json{{"valid",true},{"name",c[0]["name"]},{"event",c[0]["title"]},{"date",c[0]["event_date"]},{"issued",c[0]["issued_at"]}};});
 route(false,R"(/api/certificates/([A-Za-z0-9-]+)/download)",[&](const Request&q,json&){auto u=need(q,[](const User&){return true;});string cid=q.matches[1];
  auto c=db.q("SELECT c.file_path,r.student_id,e.organizer_id FROM certificates c JOIN registrations r ON r.id=c.registration_id JOIN events e ON e.id=r.event_id WHERE c.cert_id=?",{cid});
  if(c.empty())throw HttpErr{404,"Certificate not found"};
  long long s=c[0]["student_id"],o=c[0]["organizer_id"];if(u->id!=s&&u->id!=o&&u->role!="admin")throw HttpErr{403,"Not allowed"};
  ifstream f(c[0]["file_path"].get<string>(),ios::binary);if(!f)throw HttpErr{404,"Certificate file is missing on the server"};stringstream ss;ss<<f.rdbuf();
  return json{{"_raw",ss.str()},{"_name","attachment; filename=\""+cid+".pdf\""}};});
 route(true,R"(/api/certificates/([A-Za-z0-9-]+)/resend)",[&](const Request&q,json&){auto u=need(q,[](const User&x){return x.canManageEvents();});string cid=q.matches[1];
  auto c=db.q("SELECT c.file_path,e.id eid,e.title,u.name,u.email FROM certificates c JOIN registrations r ON r.id=c.registration_id JOIN events e ON e.id=r.event_id JOIN users u ON u.id=r.student_id WHERE c.cert_id=?",{cid});
  if(c.empty())throw HttpErr{404,"Certificate not found"};owns(*u,to_string(c[0]["eid"].get<long long>()));
  ifstream f(c[0]["file_path"].get<string>(),ios::binary);if(!f)throw HttpErr{404,"Certificate file is missing on the server"};stringstream ss;ss<<f.rdbuf();
  db.q("UPDATE certificates SET email_status='pending',email_error=NULL WHERE cert_id=?",{cid});
  deliver(cid,c[0]["email"],c[0]["name"],c[0]["title"],ss.str());return json{{"message","Email retry started"}};});

 // ---- organizer approval workflow: student requests -> admin approves/rejects -> only then role becomes organizer ----
 auto trim=[](string v){size_t a=v.find_first_not_of(" \t\r\n"),z=v.find_last_not_of(" \t\r\n");return a==string::npos?string():v.substr(a,z-a+1);};
 // Shared validation for the authority form (name, designation, optional JPEG signature as a data URL). `sig` receives the base64 to store.
 auto readAuthority=[&](json&b,const string&oldSig,string&name,string&des,string&sig){name=trim(str(b,"authority_name"));des=trim(str(b,"designation"));string in=str(b,"signature");bool clear=b.contains("clear_signature")&&b["clear_signature"]==true;
  if(name.empty()||des.empty())throw HttpErr{400,"Authority name and designation are required"};
  if(name.size()>60||des.size()>80)throw HttpErr{400,"Name or designation is too long"};
  for(char ch:name+des)if((unsigned char)ch<32||(unsigned char)ch>126)throw HttpErr{400,"Use plain English letters and numbers in the name and designation"};
  sig=clear?string():oldSig;
  if(!in.empty()){size_t k=in.find("base64,");if(in.rfind("data:image/jpeg",0)!=0||k==string::npos)throw HttpErr{400,"Signature must be a JPEG image"};
   sig=in.substr(k+7);if(sig.size()>600000)throw HttpErr{400,"Signature image is too large (max about 400 KB)"};
   string raw=unb64(sig);int w,h,c;if(raw.empty()||!jpegInfo(raw,w,h,c))throw HttpErr{400,"The signature image could not be read"};}};
 route(true,"/api/organizer/request",[&](const Request&q,json&b){auto u=need(q,[](const User&){return true;});
  if(u->role=="organizer")throw HttpErr{409,"You are already an approved event organizer"};if(u->role!="student")throw HttpErr{403,"Only students can request organizer access"};
  string org=trim(str(b,"organization")),title=trim(str(b,"event_title")),cat=str(b,"event_category"),desc=trim(str(b,"event_description")),date=str(b,"expected_date"),why=trim(str(b,"reason"));
  if(org.empty()||title.empty()||desc.empty()||why.empty())throw HttpErr{400,"Organization, event title, event description and reason are required"};
  if(!cats.count(cat))throw HttpErr{400,"Invalid category"};if(!date.empty()&&date.size()!=10)throw HttpErr{400,"Date must be YYYY-MM-DD"};
  if(org.size()>120||title.size()>150||desc.size()>1000||why.size()>600)throw HttpErr{400,"One of the fields is too long"};
  db.exec("BEGIN IMMEDIATE");
  if(!db.q("SELECT id FROM organizer_requests WHERE user_id=? AND status='pending'",{to_string(u->id)}).empty())throw HttpErr{409,"You already have a request waiting for admin approval"};
  db.q("INSERT INTO organizer_requests(user_id,organization,event_title,event_category,event_description,expected_date,reason) VALUES(?,?,?,?,?,?,?)",{to_string(u->id),org,title,cat,desc,date,why});
  notifyRole("admin","New event organizer request from "+u->name+": "+title,"0");db.exec("COMMIT");
  return json{{"_status",201},{"message","Request submitted. You will get organizer access after the admin approves it."}};});
 route(false,"/api/organizer/request",[&](const Request&q,json&){auto u=need(q,[](const User&){return true;});
  auto r=db.q("SELECT id,organization,event_title,event_category,event_description,expected_date,reason,status,admin_note,reviewed_at,created_at FROM organizer_requests WHERE user_id=? ORDER BY id DESC LIMIT 1",{to_string(u->id)});
  return json{{"role",u->role},{"request",r.empty()?json(nullptr):r[0]}};});
 route(false,"/api/admin/organizer-requests",[&](const Request&q,json&){need(q,[](const User&x){return x.canReview();});
  return json{{"requests",db.q("SELECT q.id,q.user_id,u.name,u.email,u.role,q.organization,q.event_title,q.event_category,q.event_description,q.expected_date,q.reason,q.status,q.admin_note,q.reviewed_at,q.created_at FROM organizer_requests q JOIN users u ON u.id=q.user_id ORDER BY (q.status='pending') DESC,q.id DESC")}};});
 route(true,R"(/api/admin/organizer-requests/(\d+)/review)",[&](const Request&q,json&b){auto u=need(q,[](const User&x){return x.canReview();});string id=q.matches[1],a=str(b,"action"),why=trim(str(b,"reason"));
  if(a!="approve"&&a!="reject")throw HttpErr{400,"Action must be approve or reject"};if(a=="reject"&&why.empty())throw HttpErr{400,"A reason is required when rejecting"};
  db.exec("BEGIN IMMEDIATE");auto r=db.q("SELECT user_id,event_title,status FROM organizer_requests WHERE id=?",{id});if(r.empty())throw HttpErr{404,"Request not found"};
  if(r[0]["status"]!="pending")throw HttpErr{409,"This request was already reviewed"};string uid=to_string(r[0]["user_id"].get<long long>()),T=r[0]["event_title"];
  db.q("UPDATE organizer_requests SET status=?,admin_note=?,reviewed_by=?,reviewed_at=datetime('now') WHERE id=?",{a=="approve"?"approved":"rejected",why,to_string(u->id),id});
  if(a=="approve"){db.q("UPDATE users SET role='organizer' WHERE id=? AND role='student'",{uid});notify(uid,"Your event organizer request was approved. You can now use the Organizer Dashboard to create and manage events.");}
  else notify(uid,"Your event organizer request (\""+T+"\") was rejected: "+why);
  db.exec("COMMIT");return json{{"message",a=="approve"?"Approved: the user is now an event organizer":"Request rejected"}};});
 route(false,"/api/admin/organizers",[&](const Request&q,json&){need(q,[](const User&x){return x.canReview();});
  return json{{"organizers",db.q("SELECT u.id,u.name,u.email,u.created_at,(SELECT COUNT(*) FROM events e WHERE e.organizer_id=u.id) events,(SELECT COUNT(*) FROM registrations r JOIN events e ON e.id=r.event_id WHERE e.organizer_id=u.id AND r.status='confirmed') registrations FROM users u WHERE u.role='organizer' ORDER BY u.name")}};});
 route(true,R"(/api/admin/organizers/(\d+)/revoke)",[&](const Request&q,json&b){auto u=need(q,[](const User&x){return x.canReview();});string id=q.matches[1],why=trim(str(b,"reason"));
  db.exec("BEGIN IMMEDIATE");if(db.q("SELECT id FROM users WHERE id=? AND role='organizer'",{id}).empty())throw HttpErr{404,"Organizer not found"};
  db.q("UPDATE users SET role='student' WHERE id=?",{id});
  db.q("UPDATE organizer_requests SET status='revoked',admin_note=?,reviewed_by=?,reviewed_at=datetime('now') WHERE id=(SELECT MAX(id) FROM organizer_requests WHERE user_id=? AND status='approved')",{why,to_string(u->id),id});
  notify(id,"Your event organizer access was removed by the admin."+(why.empty()?string():" Reason: "+why));db.exec("COMMIT");return json{{"message","Organizer access removed"}};});
 route(false,"/api/admin/registrations",[&](const Request&q,json&){need(q,[](const User&x){return x.canReview();});
  return json{{"registrations",db.q("SELECT r.id,r.reg_code,r.status,r.answers,r.created_at,e.id event_id,e.title event,u.name,u.email,a.checked_in_at,c.cert_id FROM registrations r JOIN events e ON e.id=r.event_id JOIN users u ON u.id=r.student_id LEFT JOIN attendance_records a ON a.registration_id=r.id LEFT JOIN certificates c ON c.registration_id=r.id ORDER BY r.id DESC LIMIT 1000")}};});
 // ---- certificate authority ----
 // Organizer Dashboard > Certificate Authority: each organizer saves the name, designation and signature image printed on the certificates of THEIR events.
 route(false,"/api/organizer/certificate-authority",[&](const Request&q,json&){auto u=need(q,[](const User&x){return x.canManageEvents();});string id=to_string(u->id);
  auto r=db.q("SELECT authority_name,designation,COALESCE(signature_b64,'') signature_b64 FROM organizer_certificate_settings WHERE user_id=?",{id});auto eff=authorityFor(id);
  bool own=!r.empty()&&!r[0]["authority_name"].get<string>().empty();string nm=own?r[0]["authority_name"].get<string>():"",ds=own?r[0]["designation"].get<string>():"",sg=own?r[0]["signature_b64"].get<string>():"";
  return json{{"authority_name",nm},{"designation",ds},{"signature",sg.empty()?string():"data:image/jpeg;base64,"+sg},{"configured",sigReady(eff)},{"source",own?"own":sigReady(eff)?"default":"none"}};});
 route(true,"/api/organizer/certificate-authority",[&](const Request&q,json&b){auto u=need(q,[](const User&x){return x.canManageEvents();});string id=to_string(u->id);
  auto old=db.q("SELECT COALESCE(signature_b64,'') s FROM organizer_certificate_settings WHERE user_id=?",{id});string name,des,sig;readAuthority(b,old.empty()?string():old[0]["s"].get<string>(),name,des,sig);
  db.exec("BEGIN IMMEDIATE");
  db.q("INSERT INTO organizer_certificate_settings(user_id,authority_name,designation,signature_b64,updated_at) VALUES(?,?,?,?,datetime('now')) ON CONFLICT(user_id) DO UPDATE SET authority_name=excluded.authority_name,designation=excluded.designation,signature_b64=excluded.signature_b64,updated_at=excluded.updated_at",{id,name,des,sig});
  int redone=0,issuedNow=0;vector<MailJob> jobs;flushPending(id,redone,issuedNow,jobs);db.exec("COMMIT");for(auto&j:jobs)deliver(j[0],j[1],j[2],j[3],j[4]);
  return json{{"message","Certificate authority saved"},{"reissued",redone},{"issued",issuedNow}};});
 // Optional site-wide default (admin API only): used for events whose organizer has not saved an authority of their own.
 route(false,"/api/admin/certificate-settings",[&](const Request&q,json&){need(q,[](const User&x){return x.canReview();});auto c=certSettings();string sg=c["signature_b64"];
  return json{{"authority_name",c["authority_name"]},{"designation",c["designation"]},{"signature",sg.empty()?string():"data:image/jpeg;base64,"+sg},{"configured",sigReady(c)}};});
 route(true,"/api/admin/certificate-settings",[&](const Request&q,json&b){auto u=need(q,[](const User&x){return x.canReview();});
  string name,des,sig;readAuthority(b,certSettings()["signature_b64"].get<string>(),name,des,sig);
  db.exec("BEGIN IMMEDIATE");
  db.q("INSERT INTO certificate_settings(id,authority_name,designation,signature_b64,updated_by,updated_at) VALUES(1,?,?,?,?,datetime('now')) ON CONFLICT(id) DO UPDATE SET authority_name=excluded.authority_name,designation=excluded.designation,signature_b64=excluded.signature_b64,updated_by=excluded.updated_by,updated_at=excluded.updated_at",{name,des,sig,to_string(u->id)});
  int redone=0,issuedNow=0;vector<MailJob> jobs;flushPending("",redone,issuedNow,jobs);db.exec("COMMIT");for(auto&j:jobs)deliver(j[0],j[1],j[2],j[3],j[4]);
  return json{{"message","Certificate authority saved"},{"reissued",redone},{"issued",issuedNow}};});
 route(false,"/api/public/stats",[&](const Request&,json&){return json{{"stats",db.q("SELECT (SELECT COUNT(*) FROM events WHERE status='published') events,(SELECT COUNT(*) FROM registrations WHERE status='confirmed') registrations,(SELECT COUNT(*) FROM certificates) certificates")[0]}};});
 route(false,R"(/api/events/(\d+)/checkin-qr)",[&](const Request&q,json&){auto u=need(q,[](const User&x){return x.canManageEvents();});string eid=q.matches[1];owns(*u,eid);
  auto ev=db.q("SELECT title,event_date,event_date="+TODAY+" is_today FROM events WHERE id=?",{eid});
  return json{{"token",ctok(eid,(long long)time(nullptr)/300)},{"title",ev[0]["title"]},{"event_date",ev[0]["event_date"]},{"is_today",ev[0]["is_today"]}};});
 route(true,"/api/attendance/self",[&](const Request&q,json&b){auto u=need(q,[](const User&x){return x.canRegister();});
  string eid=b.contains("event_id")?(b["event_id"].is_string()?b["event_id"].get<string>():b["event_id"].dump()):"",t=str(b,"token");long long w=(long long)time(nullptr)/300;
  if(t.empty()||(ctok(eid,w)!=t&&ctok(eid,w-1)!=t))throw HttpErr{400,"This QR has expired. Scan the organizer's current QR again"};
  auto r=db.q("SELECT qr_token FROM registrations WHERE event_id=? AND student_id=? AND status='confirmed'",{eid,to_string(u->id)});
  if(r.empty())throw HttpErr{409,"You are not registered for this event"};
  string tok=r[0]["qr_token"];return doCheck(eid,tok,tok,u->id);});
 // ---- health check (used by Render / uptime monitors) ----
 route(false,"/api/health",[&](const Request&,json&){return json{{"ok",true},{"schema_version",db.q("SELECT MAX(version) v FROM schema_migrations")[0]["v"]}};});
 // ---- backups: SQLite online backup into BACKUP_DIR; automatic every BACKUP_INTERVAL_HOURS (0 = off), newest BACKUP_KEEP files are kept ----
 const string bdir=env("BACKUP_DIR","backups");const int bevery=atoi(env("BACKUP_INTERVAL_HOURS","24").c_str()),bkeep=max(1,atoi(env("BACKUP_KEEP","14").c_str()));
 auto listBackups=[&]{vector<filesystem::path> all;error_code ec;if(filesystem::exists(bdir,ec))for(auto&e:filesystem::directory_iterator(bdir)){string n=e.path().filename().string();if(n.rfind("eventsphere-",0)==0&&e.path().extension()==".db")all.push_back(e.path());}sort(all.begin(),all.end());return all;};
 auto doBackup=[&]()->string{filesystem::create_directories(bdir);char ts[32];time_t t=time(nullptr);tm g;gmtime_r(&t,&g);strftime(ts,32,"%Y%m%d-%H%M%S",&g);
  string path=bdir+"/eventsphere-"+ts+".db";db.backupTo(path);auto all=listBackups();while((int)all.size()>bkeep){filesystem::remove(all.front());all.erase(all.begin());}return path;};
 route(true,"/api/admin/backup",[&](const Request&q,json&){need(q,[](const User&x){return x.canReview();});string f;try{f=doBackup();}catch(exception&e){cerr<<"backup failed: "<<e.what()<<endl;throw HttpErr{500,"Backup failed"};}
  return json{{"message","Backup created"},{"file",filesystem::path(f).filename().string()}};});
 route(false,"/api/admin/backups",[&](const Request&q,json&){need(q,[](const User&x){return x.canReview();});json a=json::array();auto all=listBackups();
  for(auto it=all.rbegin();it!=all.rend();++it)a.push_back({{"name",it->filename().string()},{"size",(long long)filesystem::file_size(*it)}});return json{{"backups",a},{"every_hours",bevery},{"keep",bkeep}};});
 route(false,R"(/api/admin/backups/(eventsphere-[0-9-]+\.db)/download)",[&](const Request&q,json&){need(q,[](const User&x){return x.canReview();});string n=q.matches[1];
  ifstream f(bdir+"/"+n,ios::binary);if(!f)throw HttpErr{404,"Backup not found"};stringstream ss;ss<<f.rdbuf();return json{{"_raw",ss.str()},{"_name","attachment; filename=\""+n+"\""},{"_ctype","application/octet-stream"}};});
 if(bevery>0)thread([&,bevery]{this_thread::sleep_for(chrono::seconds(30));while(true){{lock_guard<mutex> l(mu);try{cout<<"Backup saved: "<<doBackup()<<endl;}catch(exception&e){cerr<<"Scheduled backup failed: "<<e.what()<<endl;}}this_thread::sleep_for(chrono::hours(bevery));}}).detach();
 // ---- live attendance stream (Server-Sent Events): the organizer's screen updates the moment someone checks in ----
 svr.Get(R"(/api/events/(\d+)/live)",[&](const Request&q,Response&r){string eid=q.matches[1];
  try{lock_guard<mutex> l(mu);auto u=need(q,[](const User&x){return x.canManageEvents();});owns(*u,eid);}
  catch(HttpErr&x){r.status=x.code;r.set_content(json{{"error",x.msg}}.dump(),"application/json");return;}catch(exception&){r.status=500;return;}
  if(liveConns>=30){r.status=503;r.set_content(R"({"error":"Too many live connections"})","application/json");return;}
  liveConns++;auto seen=make_shared<long long>(-1);r.set_header("Cache-Control","no-cache");r.set_header("X-Accel-Buffering","no");
  r.set_chunked_content_provider("text/event-stream",[&liveMu,&liveCv,&liveVer,&mu,&db,eid,seen](size_t,DataSink&sink){
    {unique_lock<mutex> lk(liveMu);if(*seen>=0)liveCv.wait_for(lk,chrono::seconds(15),[&]{return liveVer!=*seen;});*seen=liveVer;}
    string msg;try{lock_guard<mutex> l(mu);auto c=db.q("SELECT (SELECT COUNT(*) FROM registrations WHERE event_id=? AND status='confirmed') registered,(SELECT COUNT(*) FROM attendance_records WHERE event_id=?) present",{eid,eid})[0];
     auto rc=db.q("SELECT u.name,a.checked_in_at FROM attendance_records a JOIN registrations r ON r.id=a.registration_id JOIN users u ON u.id=r.student_id WHERE a.event_id=? ORDER BY a.id DESC LIMIT 5",{eid});
     msg="data: "+json{{"registered",c["registered"]},{"present",c["present"]},{"recent",rc}}.dump()+"\n\n";}catch(...){msg=": error\n\n";}
    return sink.write(msg.data(),msg.size());},[&liveConns](bool){liveConns--;});});
 // ---- security headers on every response ----
 const string csp=env("CSP","default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src https://fonts.gstatic.com; img-src 'self' data: blob: https: http:; connect-src 'self'; media-src 'self' blob:; worker-src 'self'; manifest-src 'self'; frame-ancestors 'none'; base-uri 'self'; form-action 'self'");
 svr.set_post_routing_handler([csp](const Request&q,Response&r){r.set_header("X-Content-Type-Options","nosniff");r.set_header("Referrer-Policy","same-origin");r.set_header("Permissions-Policy","camera=(self), microphone=(), geolocation=()");
  if(q.get_header_value("X-Forwarded-Proto")=="https")r.set_header("Strict-Transport-Security","max-age=31536000");
  if(q.path.rfind("/docs",0)!=0){r.set_header("X-Frame-Options","DENY");if(csp!="off")r.set_header("Content-Security-Policy",csp);}
  if(q.path=="/"||q.path=="/index.html"||q.path=="/sw.js"||q.path=="/manifest.webmanifest")r.set_header("Cache-Control","no-cache");
  else if(q.path.rfind("/api/",0)==0&&!r.has_header("Cache-Control"))r.set_header("Cache-Control","no-store");});
 svr.set_file_extension_and_mimetype_mapping("webmanifest","application/manifest+json");svr.set_file_extension_and_mimetype_mapping("yaml","text/yaml");
 svr.new_task_queue=[]{return new ThreadPool(64);}; // long-lived live streams must not starve normal requests
 svr.set_payload_max_length(2*1024*1024);svr.set_mount_point("/docs",env("DOCS_DIR","docs"));svr.set_mount_point("/",env("FRONTEND_DIR","frontend"));
 int port=stoi(env("PORT","8080"));cout<<"EventSphere running on port "<<port<<endl;if(!svr.listen("0.0.0.0",port)){cerr<<"Could not start on port "<<port<<" (already in use?)"<<endl;return 1;}}
