// Unit tests for backend/src/core.h. No framework needed: run ./core_tests (or `ctest`). Exit code 0 = all passed.
#include "../src/core.h"
static int checks=0,fails=0;
#define CHECK(c) do{checks++;if(!(c)){fails++;cerr<<"FAIL "<<__FILE__<<":"<<__LINE__<<"  "<<#c<<endl;}}while(0)
#define THROWS(expr,code_,needle) do{checks++;bool ok=false;try{expr;}catch(HttpErr&e){ok=e.code==code_&&e.msg.find(needle)!=string::npos;if(!ok)cerr<<"  got "<<e.code<<": "<<e.msg<<endl;}if(!ok){fails++;cerr<<"FAIL "<<__FILE__<<":"<<__LINE__<<"  "<<#expr<<endl;}}while(0)
static string bytes(initializer_list<int> v){string s;for(int x:v)s+=(char)x;return s;}

static void test_security(){
 string h=hashPw("secret123","abcd");
 CHECK(h.rfind("abcd$",0)==0);CHECK(h.size()==4+1+64);
 CHECK(checkPw("secret123",h));CHECK(!checkPw("secret124",h));CHECK(!checkPw("",h));
 CHECK(hashPw("x","salt1")!=hashPw("x","salt2"));
 CHECK(rnd(16).size()==32);CHECK(rnd(16)!=rnd(16));
 CHECK(sha256hex("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");CHECK(sha256hex("a")!=sha256hex("b"));
 CHECK(lowerStr("MiXeD@Mail.COM")=="mixed@mail.com");
 CHECK(getSid("a=1; sid=abc123; b=2")=="abc123");CHECK(getSid("sid=abc123")=="abc123");
 CHECK(getSid("xsid=zzz").empty());CHECK(getSid("").empty());CHECK(getSid("a=1; mysid=q; sid=ok")=="ok");
}
static void test_dates(){
 for(const char*d:{"2026-02-28","2024-02-29","2026-12-31","2000-01-01"})CHECK(validDate(d));
 for(const char*d:{"2026-02-29","2026-13-01","2026-00-10","2026-04-31","26-01-01","2026-1-011","abcd-ef-gh","1999-12-31","2026/01/01",""})CHECK(!validDate(d));
}
static void test_base64_pdf_text(){
 CHECK(unb64(b64("")).empty());
 for(int n=1;n<60;n++){string s;for(int i=0;i<n;i++)s+=(char)(i*37+11);CHECK(unb64(b64(s))==s);}
 CHECK(unb64("!!!").empty());CHECK(unb64("abc").empty()); // bad length
 CHECK(pdfEsc("a(b)c\\d")=="a\\(b\\)c\\\\d");CHECK(pdfEsc(string("caf")+(char)0xC3+(char)0xA9)=="caf??");
 CHECK(textW("Hello",false)>0);CHECK(textW("HELLO",true)>textW("HELLO",false));CHECK(textW("",false)==0);
}
static void test_jpeg_and_pdf(){
 string j=bytes({0xFF,0xD8,0xFF,0xC0,0x00,0x11,0x08,0x00,0x40,0x00,0x80,0x03,0x01,0x22,0x00,0x02,0x11,0x01,0x03,0x11,0x01,0xFF,0xD9});
 int w=0,h=0,c=0;CHECK(jpegInfo(j,w,h,c));CHECK(w==128&&h==64&&c==3);
 CHECK(!jpegInfo("not a jpeg at all!",w,h,c));CHECK(!jpegInfo("",w,h,c));
 string png=bytes({0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A,0,0,0,0});CHECK(!jpegInfo(png,w,h,c));
 string pdf=makePdf("Stu Dent","Hack Day","2026-11-21","CERT-ABC","My College","2026-11-21","Dr Kumar","HOD","");
 CHECK(pdf.rfind("%PDF-1.4",0)==0);CHECK(pdf.find("(Stu Dent)")!=string::npos);CHECK(pdf.find("CERT-ABC")!=string::npos);
 CHECK(pdf.find("/Im1")==string::npos);CHECK(pdf.find("%%EOF")!=string::npos);
 string pdf2=makePdf("A","B","d","C","c","i","n","t",b64(j));CHECK(pdf2.find("/Im1 7 0 R")!=string::npos);CHECK(pdf2.find("DCTDecode")!=string::npos);
 // startxref must point at the real xref table
 size_t sx=pdf.rfind("startxref\n")+10;size_t off=stoul(pdf.substr(sx));CHECK(pdf.compare(off,4,"xref")==0);
}
static void test_forms(){
 auto f=formFields("Seminar","","[]");CHECK(f.is_array()&&f.size()>=6);CHECK(f.front()["key"]=="roll_no");CHECK(f.back()["key"]=="declaration");
 auto o=formFields("NoSuchCategory","","[]");bool hasNotes=false;for(auto&x:o)if(x["key"]=="notes")hasNotes=true;CHECK(hasNotes);
 auto h=formFields("Technical","Hackathon","[]");bool hasTeam=false;for(auto&x:h)if(x["key"]=="team_members")hasTeam=true;CHECK(hasTeam); // chosen template beats category
 json ok={{"roll_no","1"},{"department","CSE"},{"year","1st year"},{"phone","98765 43210"},{"declaration",true}};
 auto out=validateAnswers(f,ok);CHECK(out.size()==5);CHECK(out.back()["value"]=="Yes");
 json bad=ok;bad.erase("roll_no");THROWS(validateAnswers(f,bad),400,"Roll");
 bad=ok;bad["year"]="9th year";THROWS(validateAnswers(f,bad),400,"listed options");
 bad=ok;bad["phone"]="12ab";THROWS(validateAnswers(f,bad),400,"phone");
 bad=ok;bad["phone"]="123";THROWS(validateAnswers(f,bad),400,"phone");
 bad=ok;bad["declaration"]=false;THROWS(validateAnswers(f,bad),400,"confirm");
 bad=ok;bad["roll_no"]=string(200,'x');THROWS(validateAnswers(f,bad),400,"too long");
 auto hk=formFields("Hackathon","","[]");json hs=ok;hs["team_name"]="T";hs["team_members"]="a,b";hs["hack_experience"]="First hackathon";hs["team_size"]="3";
 CHECK(validateAnswers(hk,hs).size()>=8);hs["team_size"]="9";THROWS(validateAnswers(hk,hs),400,"between 1 and 6");hs["team_size"]="2.5";THROWS(validateAnswers(hk,hs),400,"whole number");
 json trimmed=ok;trimmed["roll_no"]="  42  ";CHECK(validateAnswers(f,trimmed)[0]["value"]=="42");
 // custom questions
 string c=normalizeCustom(R"([{"label":"Diet","type":"select","options":["Veg","Non-veg"],"required":true},{"label":"Notes","type":"text"}])");
 json cj=json::parse(c);CHECK(cj.size()==2);CHECK(cj[0]["key"]=="x1"&&cj[1]["key"]=="x2");CHECK(cj[0]["required"]==true);
 CHECK(json::parse(normalizeCustom("")).empty());CHECK(json::parse(normalizeCustom(R"([{"label":"   "}])")).empty());
 THROWS(normalizeCustom("not json"),400,"Invalid");
 THROWS(normalizeCustom(R"([{"label":"q","type":"select","options":["only one"]}])"),400,"2-12");
 THROWS(normalizeCustom(R"([{"label":"q","type":"video"}])"),400,"Invalid custom question type");
 json many=json::array();for(int i=0;i<9;i++)many.push_back({{"label","q"+to_string(i)},{"type","text"}});THROWS(normalizeCustom(many.dump()),400,"At most 8");
}
static void test_db_and_users(){
 Db db(":memory:");db.exec("CREATE TABLE t(id INTEGER PRIMARY KEY,name TEXT,n INTEGER)");
 db.q("INSERT INTO t(name,n) VALUES(?,?)",{"a'; DROP TABLE t;--","5"});CHECK(db.lastId()==1);
 auto r=db.q("SELECT * FROM t WHERE name=?",{"a'; DROP TABLE t;--"});CHECK(r.size()==1&&r[0]["n"]==5&&r[0]["name"].is_string()); // parameters, not string-building
 db.q("INSERT INTO t(name) VALUES(NULL)");CHECK(db.q("SELECT name FROM t WHERE id=2")[0]["name"].is_null());
 bool threw=false;try{db.q("SELEC nonsense");}catch(runtime_error&){threw=true;}CHECK(threw);
 CHECK(db.q("SELECT COUNT(*) c FROM t")[0]["c"]==2);
 string f="/tmp/es_unit_backup.db";remove(f.c_str());db.backupTo(f);Db copy(f);CHECK(copy.q("SELECT COUNT(*) c FROM t")[0]["c"]==2);remove(f.c_str());
 auto mk=[](const char*role){return makeUser(json{{"id",1},{"name","n"},{"email","e"},{"role",role}});};
 auto s=mk("student"),o=mk("organizer"),a=mk("admin"),x=mk("something-else");
 CHECK(s->canRegister()&&!s->canManageEvents()&&!s->canReview());
 CHECK(o->canManageEvents()&&!o->canRegister()&&!o->canReview());
 CHECK(a->canReview()&&a->canManageEvents()&&!a->canRegister());
 CHECK(x->canRegister()&&!x->canReview()); // unknown roles fall back to the least-privileged type
}
int main(){test_security();test_dates();test_base64_pdf_text();test_jpeg_and_pdf();test_forms();test_db_and_users();
 cout<<checks<<" checks, "<<fails<<" failed"<<endl;return fails?1:0;}
