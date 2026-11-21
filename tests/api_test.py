#!/usr/bin/env python3
"""EventSphere API integration tests (Python standard library only).

Starts the real server on a throw-away database and exercises it over HTTP:
    python3 tests/api_test.py --server build/eventsphere --root .
Exit code 0 = everything passed. Also run by `ctest` and by the GitHub Actions workflow.
"""
import argparse, datetime, http.client, http.cookiejar, json, os, shutil, socket, sqlite3, subprocess, sys, tempfile, time
import urllib.error, urllib.request

ap = argparse.ArgumentParser()
ap.add_argument('--server', required=True)
ap.add_argument('--root', default='.')
A = ap.parse_args()
ROOT = os.path.abspath(A.root)
SERVER = os.path.abspath(A.server)
TMP = tempfile.mkdtemp(prefix='es_api_')
RESULTS = []

def check(name, cond, detail=''):
    RESULTS.append((name, bool(cond)))
    print(('  ok   ' if cond else '  FAIL ') + name + ('' if cond else '   -> ' + str(detail)))

def free_port():
    s = socket.socket(); s.bind(('127.0.0.1', 0)); p = s.getsockname()[1]; s.close(); return p

def start(port, db, extra=None):
    env = dict(os.environ, DB_PATH=db, CERT_DIR=os.path.join(TMP, 'certs'), BACKUP_DIR=os.path.join(TMP, 'backups'), BACKUP_INTERVAL_HOURS='0',
               BACKUP_KEEP='3', ADMIN_EMAIL='Admin@College.edu', ADMIN_PASSWORD='ChangeMe123', PORT=str(port), UTC_OFFSET_MIN='330', TRUST_PROXY='0',
               COLLEGE_NAME='Test College', SIGNUP_LIMIT_PER_HOUR='30')
    env.update(extra or {})
    log = open(os.path.join(TMP, 'server-%d.log' % port), 'w')
    p = subprocess.Popen([SERVER], cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    for _ in range(100):
        try:
            urllib.request.urlopen('http://127.0.0.1:%d/api/health' % port, timeout=1).read(); return p
        except Exception:
            if p.poll() is not None: break
            time.sleep(0.1)
    p.kill(); print(open(log.name).read()); raise SystemExit('server did not start')

class C:
    def __init__(s, base):
        s.base = base; s.jar = http.cookiejar.CookieJar(); s.op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(s.jar))
    def cookie(s, n):
        for c in s.jar:
            if c.name == n: return c.value
    def req(s, method, path, body=None, headers=None, csrf=True):
        h = {'Content-Type': 'application/json'}
        if method != 'GET' and csrf and s.cookie('csrf'): h['X-CSRF-Token'] = s.cookie('csrf')
        h.update(headers or {})
        r = urllib.request.Request(s.base + path, data=json.dumps(body).encode() if body is not None else None, method=method, headers=h)
        try: resp = s.op.open(r, timeout=15)
        except urllib.error.HTTPError as e: resp = e
        data = resp.read(); ct = resp.headers.get('Content-Type', '')
        return resp.status, (json.loads(data) if 'json' in ct and data else data), resp.headers
    def get(s, p, **k): return s.req('GET', p, **k)
    def post(s, p, b=None, **k): return s.req('POST', p, b if b is not None else {}, **k)

def stream_events(base_host, port, path, cookie_hdr, n, timeout=10):
    """Reads n Server-Sent Events and returns the parsed JSON payloads."""
    conn = http.client.HTTPConnection(base_host, port, timeout=timeout); conn.request('GET', path, headers={'Cookie': cookie_hdr}); resp = conn.getresponse()
    return conn, resp

def read_event(resp):
    while True:
        line = resp.readline().decode()
        if not line: return None
        if line.startswith('data:'): return json.loads(line[5:])

def main():
    port = free_port(); base = 'http://127.0.0.1:%d' % port; db = os.path.join(TMP, 'test.db')
    srv = start(port, db)
    ist = (datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(minutes=330)).date()
    day = lambda n: str(ist + datetime.timedelta(days=n))
    try:
        print('Health, headers, static files')
        c = C(base)
        st, j, h = c.get('/api/health'); check('health ok with schema version 3', st == 200 and j['ok'] and j['schema_version'] == 3, j)
        check('API responses are not cacheable', 'no-store' in h.get('Cache-Control', ''))
        st, body, h = c.get('/')
        check('SPA index served with CSP', st == 200 and b'<div id="root">' in body and "default-src 'self'" in h.get('Content-Security-Policy', ''), h.get('Content-Security-Policy'))
        check('security headers present', h.get('X-Content-Type-Options') == 'nosniff' and h.get('X-Frame-Options') == 'DENY' and 'camera=(self)' in h.get('Permissions-Policy', ''))
        check('index is revalidated, not cached', 'no-cache' in h.get('Cache-Control', ''))
        check('index links the PWA manifest', b'manifest.webmanifest' in body)
        st, body, h = c.get('/manifest.webmanifest'); check('manifest served as JSON manifest', st == 200 and 'manifest+json' in h.get('Content-Type', '') and (body if isinstance(body, dict) else json.loads(body))['display'] == 'standalone')
        st, body, h = c.get('/sw.js'); check('service worker served', st == 200 and b'addEventListener' in body and 'no-cache' in h.get('Cache-Control', ''))
        st, body, h = c.get('/docs/openapi.yaml'); check('OpenAPI spec served', st == 200 and b'openapi:' in body)
        st, body, h = c.get('/docs/'); check('API docs page served without the app CSP', st == 200 and 'Content-Security-Policy' not in h)

        print('Accounts, CSRF and sessions')
        admin, stu, org, stu2 = C(base), C(base), C(base), C(base)
        st, j, h = admin.post('/api/login', {'email': 'admin@college.edu', 'password': 'ChangeMe123'}); check('admin can log in (email case-insensitive)', st == 200 and j['user']['role'] == 'admin', j)
        cookies = h.get_all('Set-Cookie') or []
        check('sid cookie is HttpOnly+SameSite, csrf cookie readable by JS', any(x.startswith('sid=') and 'HttpOnly' in x and 'SameSite=Strict' in x for x in cookies) and any(x.startswith('csrf=') and 'HttpOnly' not in x for x in cookies), cookies)
        st, _, h = C(base).post('/api/login', {'email': 'x@y.z', 'password': 'nope'}); check('wrong password rejected', st == 401)
        st, _, h = C(base).req('POST', '/api/login', {'email': 'admin@college.edu', 'password': 'ChangeMe123'}, headers={'X-Forwarded-Proto': 'https'})
        check('Secure flag added behind HTTPS proxy', any('Secure' in x for x in h.get_all('Set-Cookie')))
        for cl, n, e in ((stu, 'Stu', 'Stu@Test.com'), (org, 'Org', 'org@test.com'), (stu2, 'Stu Two', 'stu2@test.com')):
            st, j, _ = cl.post('/api/signup', {'name': n, 'email': e, 'password': 'password1'}); check('signup ' + n, st == 201, j)
        st, j, _ = C(base).post('/api/signup', {'name': 'D', 'email': 'STU@test.com', 'password': 'password1'}); check('duplicate email (different case) rejected', st == 409)
        st, j, _ = C(base).post('/api/signup', {'name': 'D', 'email': 'd@test.com', 'password': 'short'}); check('short password rejected', st == 400)
        for cl, e in ((stu, 'stu@test.com'), (org, 'ORG@test.com'), (stu2, 'stu2@test.com')): cl.post('/api/login', {'email': e, 'password': 'password1'})
        check('student logged in', stu.get('/api/me')[1]['user']['role'] == 'student')
        st, j, _ = stu.req('POST', '/api/organizer/request', {}, csrf=False); check('POST without CSRF token is refused (403)', st == 403, j)
        st, j, _ = stu.req('POST', '/api/organizer/request', {}, headers={'X-CSRF-Token': 'wrong'}); check('POST with a wrong CSRF token is refused', st == 403)
        st, j, _ = stu.post('/api/organizer/request', {}); check('POST with the right token passes the CSRF check (then validates)', st == 400, (st, j))
        old = 'sid=' + stu.cookie('sid')
        st, j, _ = stu.post('/api/logout'); check('logout works', st == 200 and stu.get('/api/me')[1]['user'] is None)
        st, j, _ = C(base).get('/api/me', headers={'Cookie': old}); check('old session cookie is dead after logout', j['user'] is None)
        stu.post('/api/login', {'email': 'stu@test.com', 'password': 'password1'})
        for i in range(10): lk = C(base).post('/api/login', {'email': 'lock@y.z', 'password': 'bad'})
        st, j, _ = C(base).post('/api/login', {'email': 'lock@y.z', 'password': 'bad'}); check('login locked after 10 failures for one account', st == 429, st)

        print('Roles and organizer approval')
        st, _, _ = C(base).get('/api/my/events'); check('anonymous gets 401', st == 401)
        ev = dict(title='Hack Day', description='d', category='Seminar', venue='Hall', event_date=day(0), reg_open=day(-2), reg_close=day(0), capacity=5, banner='')
        st, j, _ = stu.post('/api/events', ev); check('student cannot create events (403)', st == 403)
        st, j, _ = org.post('/api/organizer/request', {'organization': 'CSE', 'event_title': 'T', 'event_category': 'Technical', 'event_description': 'd', 'reason': 'r'}); check('organizer request submitted', st == 201, j)
        st, j, _ = admin.post('/api/admin/organizer-requests/1/review', {'action': 'approve'}); check('admin approves request', st == 200, j)
        check('role changes to organizer', org.get('/api/me')[1]['user']['role'] == 'organizer')

        print('Events and validation')
        for bad, why in (({'event_date': '2026-13-45'}, 'impossible date'), ({'banner': 'javascript:alert(1)'}, 'script banner'), ({'capacity': 0}, 'zero capacity'),
                         ({'reg_close': day(5)}, 'reg closes after event'), ({'title': ''}, 'empty title')):
            st, j, _ = org.post('/api/events', {**ev, **bad}); check('rejects ' + why, st == 400, (st, j))
        st, j, _ = org.post('/api/events', ev); check('organizer creates event', st == 201, j)
        check('unpublished event hidden from public list', len(C(base).get('/api/events')[1]['events']) == 0)
        st, j, _ = admin.post('/api/admin/events/1/review', {'action': 'reject'}); check('rejecting needs a reason', st == 400)
        st, j, _ = admin.post('/api/admin/events/1/review', {'action': 'approve'}); check('admin publishes event', st == 200)
        check('published event is listed', len(C(base).get('/api/events')[1]['events']) == 1)
        check('search for "%" matches nothing (wildcards escaped)', C(base).get('/api/events?q=%25')[1]['events'] == [])
        check('search by title works', len(C(base).get('/api/events?q=Hack')[1]['events']) == 1)

        print('Registration')
        ans = {'roll_no': '1', 'department': 'CSE', 'year': '1st year', 'phone': '9876543210', 'declaration': True}
        st, j, _ = stu.post('/api/events/1/register', {'answers': {**ans, 'phone': 'abc'}}); check('invalid phone rejected', st == 400, j)
        st, j, _ = stu.post('/api/events/1/register', {'answers': ans}); check('student registers', st == 201 and j['reg_code'].startswith('REG-'), j); code = j['reg_code']
        st, j, _ = stu.post('/api/events/1/register', {'answers': ans}); check('double registration refused', st == 409)
        st, j, _ = org.post('/api/events/1/register', {'answers': ans}); check('organizer cannot register as student', st == 403)
        st, j, _ = stu2.post('/api/events/1/register', {'answers': ans}); check('second student registers', st == 201)
        st, j, _ = org.post('/api/events/1/update', {**ev, 'capacity': 1}); check('capacity below registered count refused', st == 409, j)
        st, j, _ = org.post('/api/events/1/update', {**ev, 'capacity': 5}); check('valid update accepted (goes back to review)', st == 200)
        admin.post('/api/admin/events/1/review', {'action': 'approve'})
        ev2 = {**ev, 'title': 'Tiny', 'event_date': day(5), 'reg_close': day(3), 'capacity': 1}
        org.post('/api/events', ev2); admin.post('/api/admin/events/2/review', {'action': 'approve'})
        stu.post('/api/events/2/register', {'answers': ans}); st, j, _ = stu2.post('/api/events/2/register', {'answers': ans}); check('full event refuses registration', st == 409 and 'full' in j['error'], j)
        st, j, _ = stu2.get('/api/events/1/participants'); check('student cannot read participants', st == 403)

        print('Live attendance stream (Server-Sent Events)')
        st, j, _ = stu.get('/api/events/1/live'); check('students cannot open the live stream', st == 403)
        st, j, _ = C(base).get('/api/events/1/live'); check('anonymous cannot open the live stream', st == 401)
        ck = 'sid=%s' % org.cookie('sid'); conn, resp = stream_events('127.0.0.1', port, '/api/events/1/live', ck, 2)
        check('stream content type', resp.status == 200 and 'text/event-stream' in resp.getheader('Content-Type', ''))
        e1 = read_event(resp); check('first event is an immediate snapshot', e1 and e1['registered'] == 2 and e1['present'] == 0, e1)

        print('Attendance, certificates, access control')
        st, j, _ = org.post('/api/attendance/check', {'event_id': 1, 'token': code}); check('check-in recorded; certificate waits for authority', st == 200 and j['result'] == 'ok' and j.get('certificate_pending'), j)
        e2 = read_event(resp); check('live stream pushed the check-in instantly', e2 and e2['present'] == 1 and e2['recent'][0]['name'] == 'Stu', e2)
        stu2.post('/api/events/1/register', {'answers': ans}); conn.close()
        st, j, _ = org.post('/api/attendance/check', {'event_id': 1, 'token': code}); check('second scan reports already checked in', j['result'] == 'already')
        st, j, _ = org.post('/api/attendance/check', {'event_id': 2, 'token': code}); check('QR for a different event is refused', st in (404, 409), (st, j))
        st, j, _ = stu.post('/api/registrations/1/cancel'); check('cannot cancel after attendance', st == 409, j)
        st, j, _ = org.post('/api/organizer/certificate-authority', {'authority_name': 'Dr Kumar', 'designation': 'HOD'}); check('authority saved, waiting certificate issued', st == 200 and j['issued'] == 1, j)
        regs = stu.get('/api/my/registrations')[1]['registrations']; cid = [r for r in regs if r['cert_id']][0]['cert_id']
        st, pdf, h = stu.get('/api/certificates/%s/download' % cid); check('student downloads own certificate PDF', st == 200 and pdf[:8] == b'%PDF-1.4')
        st, _, _ = stu2.get('/api/certificates/%s/download' % cid); check("other student cannot download it (403)", st == 403)
        st, _, _ = org.get('/api/certificates/%s/download' % cid); check('organizer can download it', st == 200)
        st, j, _ = C(base).get('/api/certificates/verify/' + cid.lower()); check('public verification works (case-insensitive)', st == 200 and j['valid'] and j['name'] == 'Stu')
        st, j, _ = C(base).get('/api/certificates/verify/CERT-NOPE'); check('unknown certificate -> 404', st == 404)
        check('certificate shows the issue date in the college time zone', ('(%s)' % ist.isoformat()).encode() in pdf)

        print('Backups')
        st, _, _ = stu.post('/api/admin/backup'); check('students cannot back up (403)', st == 403)
        for i in range(4):
            st, j, _ = admin.post('/api/admin/backup'); check('backup %d created' % (i + 1), st == 200, j); time.sleep(1.1)
        st, j, _ = admin.get('/api/admin/backups'); check('retention keeps only the newest 3', st == 200 and len(j['backups']) == 3, j)
        st, data, h = admin.get('/api/admin/backups/%s/download' % j['backups'][0]['name']); check('backup downloads as a real SQLite file', st == 200 and data[:15] == b'SQLite format 3')
        bk = os.path.join(TMP, 'dl.db'); open(bk, 'wb').write(data); n = sqlite3.connect(bk).execute('select count(*) from users').fetchone()[0]; check('backup contains the data', n >= 4, n)
        st, _, _ = admin.get('/api/admin/backups/..%2f..%2fetc%2fpasswd/download'); check('backup download blocks path tricks', st in (400, 404))

        print('Sessions survive a server restart')
        srv.terminate(); srv.wait(); srv = start(port, db)
        st, j, _ = stu2.get('/api/me'); check('still logged in after restart (sessions are in the database)', j['user'] and j['user']['name'] == 'Stu Two', j)
        st, j, _ = stu2.post('/api/logout'); check('CSRF token also survives restart', st == 200)
        raw = sqlite3.connect(db).execute('select token_hash from sessions limit 1').fetchone()[0]; check('only hashed session tokens are stored', len(raw) == 64 and raw != org.cookie('sid'))

        print('Per-IP sign-up limit')
        got = None
        for i in range(45):
            got, _, _ = C(base).post('/api/signup', {'name': 'Bot', 'email': 'bot%d@t.com' % i, 'password': 'password1'})
            if got == 429: break
        check('mass sign-ups from one address are throttled (429)', got == 429, got)
    finally:
        srv.terminate(); srv.wait()

    print('Upgrading an old database (before migrations existed)')
    legacy = os.path.join(TMP, 'legacy.db'); sql = open(os.path.join(ROOT, 'database/migrations/001_baseline.sql')).read()
    for col in (",form_type TEXT NOT NULL DEFAULT''", ",form_fields TEXT NOT NULL DEFAULT'[]'", ",qr_token TEXT", ",answers TEXT NOT NULL DEFAULT'[]'", ",signed INTEGER NOT NULL DEFAULT 0"):
        assert col in sql, col; sql = sql.replace(col, '')
    con = sqlite3.connect(legacy); con.executescript(sql)
    con.execute("insert into users(name,email,password_hash,role) values('Old Org','old@x.com','x$y','organizer')")
    con.execute("insert into users(name,email,password_hash,role) values('Old Stu','os@x.com','x$y','student')")
    con.execute("insert into events(organizer_id,title,category,venue,event_date,reg_open,reg_close,capacity,status) values(1,'Old','Other','V','2026-01-01','2025-12-01','2025-12-30',5,'published')")
    con.execute("insert into registrations(reg_code,event_id,student_id) values('REG-OLD1',1,2)"); con.commit(); con.close()
    p2 = free_port(); s2 = start(p2, legacy)
    try:
        st, j, _ = C('http://127.0.0.1:%d' % p2).get('/api/health'); check('old database started and migrated', st == 200 and j['schema_version'] == 3, j)
    finally:
        s2.terminate(); s2.wait()
    con = sqlite3.connect(legacy)
    check('missing columns were added and old rows kept', con.execute("select count(*) from registrations where reg_code='REG-OLD1'").fetchone()[0] == 1 and con.execute("select form_type,answers from events e join registrations r on r.event_id=e.id limit 1").fetchone() is not None)
    check('old registration received a QR token', len(con.execute("select qr_token from registrations").fetchone()[0] or '') == 32)
    check('all 3 migrations recorded', [r[0] for r in con.execute('select version from schema_migrations order by version')] == [1, 2, 3])
    check('existing organizer grandfathered as approved', con.execute("select status from organizer_requests where user_id=1").fetchone()[0] == 'approved')
    con.close()

    ok = sum(1 for _, r in RESULTS if r); print('\n%d/%d checks passed' % (ok, len(RESULTS)))
    shutil.rmtree(TMP, ignore_errors=True)
    return 0 if ok == len(RESULTS) else 1

if __name__ == '__main__':
    sys.exit(main())
