# Database migrations
Every `NNN_name.sql` file here runs exactly once, in numeric order, inside a transaction, when the server starts.
Applied versions are recorded in the `schema_migrations` table. To change the schema, add the next numbered file
(for example `004_add_event_poster.sql`) and never edit a file that has already been released.
Databases created by older EventSphere versions (before migrations existed) are upgraded automatically.
