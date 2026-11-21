# EventSphere (C++ / SQLite / vanilla JS)

## Build and run (Ubuntu/WSL; on Windows use WSL or MSYS2 with the same packages)
    sudo apt install g++ cmake libsqlite3-dev libssl-dev libcurl4-openssl-dev curl
    ./setup.sh                      # downloads cpp-httplib + nlohmann/json headers
    cmake -S backend -B build && cmake --build build
    ADMIN_EMAIL=admin@college.edu ADMIN_PASSWORD=ChangeMe123 ./build/eventsphere
Open http://localhost:8080 (run from the project root; the server also serves the frontend).
The admin is created at startup from the env vars (dev only; use your own password, never commit it).

## Implemented
Signup/login/logout (PBKDF2-SHA256 salted hashes, HttpOnly session cookie), role checks on the server,
event proposals -> admin approve / reject / request changes (with history), organizer edit + re-review,
public listing of published events only, search + category filter, registration with unique ID,
duplicate prevention, closed/full checks, atomic capacity (BEGIN IMMEDIATE transaction), cancellation,
My Registrations, admin stats and category chart from real DB data.

## Roles and dashboards (two dashboards only)
- **Students / public**: browse published events without logging in; click Register to open that event's own registration form. The **Student Dashboard** lists tickets and certificates and has an in-app **camera scanner**: on the event day the student taps "Scan attendance QR", points the camera at the organizer's live QR, and attendance is marked (certificate issued automatically). If the camera is blocked, a photo of the QR can be uploaded instead. Browsers allow the camera only on HTTPS or localhost (run.sh prints an https:// phone link).
- **Organizer Dashboard**: only for approved organizers (overview, my events, create event, registrations with form answers, attendance and certificates).
- **Admin Dashboard**: organizer requests (approve/reject), organizers, events review, all registrations, attendance.
- Signup always creates a student. A student submits an *event creator request*; only an admin approval changes the role to `organizer`.
  Every API call re-checks the role in the database, so URLs/APIs cannot be used to skip approval.
- Registration forms: one template per category (Technical, Workshop, Cultural, Symposium, Sports, Seminar, Hackathon, Other),
  organizers can pick another template and add up to 8 extra questions. Answers are validated on the server and stored with the event and student.
- **Certificate authority (Organizer Dashboard > Certificate Authority)**: each organizer saves the authority name, designation and a signature image (PNG/JPG/WebP; the browser converts it to JPEG).
  Certificates for that organizer's events carry exactly that block, and one organizer's authority is never printed on another's certificates.
  Until an organizer saves an authority, attendance is still recorded but certificates for their events are held (students and the organizer are told);
  saving the authority issues all waiting certificates and emails them. Already-issued certificates keep the signature they were issued with.
  Optional site-wide fallback for organizers who set nothing: `CERT_AUTHORITY_NAME` / `CERT_AUTHORITY_TITLE` environment variables (or the admin API `/api/admin/certificate-settings`).

## Also implemented
QR attendance (organizer live QR + student camera scanner + manual reg ID), PDF certificates with verification page,
SMTP email delivery (libcurl), CSV export (formula-safe).

## Level 3 (production) features
**Security**
- Sessions live in the database (stored as SHA-256 hashes), last 7 days, survive restarts, max 10 devices per user, destroyed on logout.
- CSRF protection: every POST from a logged-in browser must send the per-session token (cookie `csrf` -> header `X-CSRF-Token`; the web app does it automatically).
- Login lockout (10 wrong passwords per account -> 10 minutes) plus per-IP limits (`LOGIN_LIMIT_PER_10MIN`, default 300; `SIGNUP_LIMIT_PER_HOUR`, default 100).
  Set `TRUST_PROXY=1` behind Render/Cloudflare so the real visitor IP is used (Dockerfile and run.sh already do).
- Security headers on every response: Content-Security-Policy (override with env `CSP`, or `CSP=off` to disable), X-Frame-Options, nosniff, Referrer-Policy, Permissions-Policy (camera only for this site), HSTS behind HTTPS.
- Cookies: HttpOnly + SameSite=Strict (+ Secure behind HTTPS), constant-time password and token comparison.

**Database migrations and backups**
- `database/migrations/NNN_*.sql` run once, in order, in a transaction, and are recorded in `schema_migrations`. Add a new numbered file to change the schema (see database/migrations/README.md). Older databases upgrade automatically.
- Backups use SQLite's online backup API into `BACKUP_DIR` (default `backups/`, `/data/backups` in Docker): automatic every `BACKUP_INTERVAL_HOURS` (default 24, `0` = off), newest `BACKUP_KEEP` (default 14) kept.
  Admin Dashboard > Backups: back up now, download. **Restore:** stop the server, replace the database file (`DB_PATH`) with a downloaded backup, start again.

**Live updates** - the organizer's Attendance page opens a Server-Sent-Events stream (`/api/events/{id}/live`): present/registered counts and the latest check-in update the moment a student scans (no page refresh). Up to 30 live screens at once.

**Installable app (PWA) + offline** - open the site in Chrome/Edge/Safari and use *Install app / Add to Home Screen* (needs HTTPS or localhost).
The app shell opens without a connection. If the venue has no signal: the organizer's scans are saved on the phone and upload automatically when the connection returns,
and a student's ticket + QR pass (last loaded) still opens. Personal API data is never stored by the service worker.

**API documentation** - interactive Swagger page at `/docs/` and the spec at `/docs/openapi.yaml`. `tests/check_openapi.py` fails the build if a route and the spec ever disagree.

**Automated tests** (all run by one command)
    cmake -S backend -B build && cmake --build build && ctest --test-dir build --output-on-failure
    node --test tests/frontend_test.mjs          # offline queue
- `backend/tests/test_core.cpp` - C++ unit tests (passwords, dates, PDF builder, form validation, database wrapper, roles, backups).
- `tests/api_test.py` - starts the real server on a temporary database and tests HTTP end to end (login, CSRF, roles, events, registration, live stream, attendance, certificates, backups, restart, upgrade of an old database).
- `tests/check_openapi.py` - routes vs documentation.

**CI/CD** - `.github/workflows/ci.yml`: on every push/PR it builds, runs all tests, builds the frontend (and checks the committed `frontend/` is up to date) and builds the Docker image;
on `main` it then calls the Render deploy hook (GitHub repo > Settings > Secrets > `RENDER_DEPLOY_HOOK`). `render.yaml` is the Render blueprint (disk + health check `/api/health`).

## Not yet implemented
Waiting list, PostgreSQL (SQLite with backups is enough for one college; a move would need a rewrite of the data layer), user management and profile pages.

## Frontend (React)
The UI is a React + Vite app. Source: `frontend-src/`. The compiled site is already built into `frontend/`,
so you do NOT need Node.js to run or deploy the app. To change the UI:
    cd frontend-src && npm install && npm run build      (outputs to ../frontend)
    npm run dev                                          (live reload; proxies /api to the C++ server on :8080)
