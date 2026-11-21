-- Speeds up "my registrations", attendance counts, organizer event lists and certificate flushing.
CREATE INDEX IF NOT EXISTS idx_reg_student ON registrations(student_id,status);
CREATE INDEX IF NOT EXISTS idx_att_event ON attendance_records(event_id);
CREATE INDEX IF NOT EXISTS idx_events_org ON events(organizer_id);
CREATE INDEX IF NOT EXISTS idx_cert_signed ON certificates(signed);
