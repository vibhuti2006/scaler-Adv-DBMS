-- SQLite-side schema mirroring System_Design_Docs/PostgreSQL_Internals/setup.sql.
-- Run with:   sqlite3 sysdesign.db < sqlite_setup.sql

DROP TABLE IF EXISTS enrollments;
DROP TABLE IF EXISTS students;
DROP TABLE IF EXISTS courses;

CREATE TABLE students (
    id     INTEGER PRIMARY KEY,
    name   TEXT NOT NULL,
    email  TEXT NOT NULL,
    city   TEXT NOT NULL,
    cgpa   REAL,
    joined TEXT
);
CREATE TABLE courses (
    id      INTEGER PRIMARY KEY,
    name    TEXT NOT NULL,
    credits INT  NOT NULL
);
CREATE TABLE enrollments (
    id         INTEGER PRIMARY KEY,
    student_id INTEGER NOT NULL,
    course_id  INTEGER NOT NULL,
    grade      TEXT,
    term       TEXT
);

-- Recursive CTE seeds the data without dialect-specific generators.
WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n < 50000)
INSERT INTO students(id, name, email, city, cgpa, joined)
SELECT n,
       'Student_' || n,
       'student' || n || '@scaler.com',
       (CASE n%7 WHEN 0 THEN 'Bangalore' WHEN 1 THEN 'Mumbai' WHEN 2 THEN 'Delhi'
                 WHEN 3 THEN 'Chennai'  WHEN 4 THEN 'Hyderabad' WHEN 5 THEN 'Pune' ELSE 'Kolkata' END),
       6.0 + ((n % 400) / 100.0),
       date('2022-01-01', '+' || (n % 1000) || ' days')
FROM seq;

CREATE INDEX idx_students_city ON students(city);
CREATE INDEX idx_students_cgpa ON students(cgpa);
