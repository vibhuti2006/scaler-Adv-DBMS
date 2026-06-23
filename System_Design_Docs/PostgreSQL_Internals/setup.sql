-- Setup for PostgreSQL_Internals experiments
-- Schema: students + enrollments + courses (good for a multi-table join)

DROP TABLE IF EXISTS enrollments;
DROP TABLE IF EXISTS students;
DROP TABLE IF EXISTS courses;

CREATE TABLE students (
    id      BIGINT PRIMARY KEY,
    name    TEXT NOT NULL,
    email   TEXT NOT NULL,
    city    TEXT NOT NULL,
    cgpa    NUMERIC(3,2),
    joined  DATE
);

CREATE TABLE courses (
    id     BIGINT PRIMARY KEY,
    name   TEXT NOT NULL,
    credits INT NOT NULL
);

CREATE TABLE enrollments (
    id         BIGSERIAL PRIMARY KEY,
    student_id BIGINT NOT NULL REFERENCES students(id),
    course_id  BIGINT NOT NULL REFERENCES courses(id),
    grade      CHAR(2),
    term       TEXT
);

INSERT INTO students(id, name, email, city, cgpa, joined)
SELECT g,
       'Student_' || g,
       'student' || g || '@scaler.com',
       (ARRAY['Bangalore','Mumbai','Delhi','Chennai','Hyderabad','Pune','Kolkata'])[1 + (g % 7)],
       6.0 + ((g % 400)::numeric / 100.0),
       DATE '2022-01-01' + ((g % 1000) || ' days')::INTERVAL
FROM generate_series(1, 50000) g;

INSERT INTO courses(id, name, credits) VALUES
 (1, 'ADBMS',          4),
 (2, 'OS',             4),
 (3, 'DSA',            3),
 (4, 'Computer Networks', 3),
 (5, 'ML',             4),
 (6, 'Compilers',      3),
 (7, 'Distributed Systems', 4);

INSERT INTO enrollments(student_id, course_id, grade, term)
SELECT 1 + (g % 50000),
       1 + (g % 7),
       (ARRAY['A','A-','B+','B','B-','C+','C'])[1 + (g % 7)],
       (ARRAY['2024-FALL','2024-SPRING','2025-FALL','2025-SPRING'])[1 + (g % 4)]
FROM generate_series(1, 200000) g;

CREATE INDEX idx_students_city ON students(city);
CREATE INDEX idx_students_cgpa ON students(cgpa);
CREATE INDEX idx_enr_student   ON enrollments(student_id);
CREATE INDEX idx_enr_course    ON enrollments(course_id);

ANALYZE students;
ANALYZE courses;
ANALYZE enrollments;
