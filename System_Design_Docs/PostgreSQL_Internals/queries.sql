-- Queries used to gather evidence for the PostgreSQL_Internals write-up.
-- Each query is paired with the captured output in results.txt.

\timing on

-- 1. Sizes and page counts
SELECT pg_size_pretty(pg_database_size('sysdesign')) AS db_size;
SELECT pg_size_pretty(pg_relation_size('students'))   AS students_heap,
       pg_relation_size('students')/8192               AS students_pages,
       pg_size_pretty(pg_total_relation_size('students')) AS total;
SELECT pg_size_pretty(pg_relation_size('enrollments')) AS enr_heap,
       pg_relation_size('enrollments')/8192            AS enr_pages;

-- 2. Multi-table join with EXPLAIN ANALYZE + BUFFERS
EXPLAIN (ANALYZE, BUFFERS)
SELECT c.name, COUNT(*) AS enrolled, AVG(s.cgpa) AS avg_cgpa
FROM enrollments e
JOIN students s ON s.id = e.student_id
JOIN courses  c ON c.id = e.course_id
WHERE s.city = 'Bangalore' AND e.term = '2025-FALL'
GROUP BY c.name
ORDER BY enrolled DESC;

-- 3. pg_statistic / pg_stats — what the planner knows
SELECT attname, n_distinct, null_frac, avg_width,
       array_length(most_common_vals, 1) AS n_mcv
FROM pg_stats
WHERE schemaname='public' AND tablename='students';

-- 4. MVCC bookkeeping — xmin / xmax / ctid on a row before/after UPDATE
BEGIN;
SELECT id, name, xmin, xmax, ctid FROM students WHERE id = 1;
UPDATE students SET cgpa = cgpa + 0.01 WHERE id = 1;
SELECT id, name, xmin, xmax, ctid FROM students WHERE id = 1;
ROLLBACK;

-- 5. Buffer cache contents
SELECT relfilenode::regclass AS rel, COUNT(*) AS pages_in_buffers
FROM pg_buffercache
WHERE relfilenode IN (SELECT oid FROM pg_class WHERE relname IN
                      ('students','enrollments','courses','idx_students_city','idx_enr_student'))
GROUP BY rel
ORDER BY pages_in_buffers DESC;

-- 6. WAL position / activity counters
SELECT pg_current_wal_lsn();

-- 7. B-tree page layout via pageinspect
SELECT type, level, live_items, dead_items, page_size, free_size
FROM bt_page_stats('idx_students_city', 1);
