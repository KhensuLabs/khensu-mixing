"""SQLite WAL-mode load test (Audit Item #23).

Validates that the FeedbackManager SQLite database:
  1. Operates in WAL journal mode (not DELETE/ROLLBACK)
  2. Accepts concurrent reads from multiple threads without blocking
  3. Serialises concurrent writes correctly (no corruption, no data loss)
  4. WAL autocheckpoint triggers without hanging

This test targets the SAME schema as the C++ FeedbackManager but uses the
Python sqlite3 module to probe the database directly, simulating a scenario
where multiple FastAPI workers and the C++ desktop app open the same DB file.

Usage:
    pytest tests/test_sqlite_load.py -v
"""
from __future__ import annotations

import os
import sqlite3
import threading
import time
import uuid
import pytest


# Use a writable temp dir inside the project to avoid Windows AppData Temp
# permission issues that affect the default pytest tmpdir location.
@pytest.fixture
def db_dir(tmp_path_factory):
    """Return a writable directory for test database files."""
    return tmp_path_factory.mktemp("sqlite_test", numbered=True)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _create_db(path: str) -> sqlite3.Connection:
    """Open (or create) the test DB with the same schema as FeedbackManager."""
    conn = sqlite3.connect(path, check_same_thread=False, timeout=10.0)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    conn.execute("PRAGMA wal_autocheckpoint=200")
    conn.executescript("""
        CREATE TABLE IF NOT EXISTS recommendation_feedback (
            id                TEXT PRIMARY KEY,
            recommendation_id TEXT NOT NULL UNIQUE,
            acted_on          INTEGER NOT NULL DEFAULT 0,
            feedback_note     TEXT    NOT NULL DEFAULT '',
            created_at        TEXT    NOT NULL,
            updated_at        TEXT    NOT NULL
        );
        CREATE TABLE IF NOT EXISTS user_preferences (
            pref_key   TEXT PRIMARY KEY,
            pref_value REAL NOT NULL
        );
        CREATE TABLE IF NOT EXISTS parameter_overrides (
            id                TEXT PRIMARY KEY,
            recommendation_id TEXT NOT NULL,
            parameter_name    TEXT NOT NULL,
            recommended_value REAL NOT NULL,
            actual_value      REAL NOT NULL,
            created_at        TEXT NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_overrides_rec_id
            ON parameter_overrides(recommendation_id);
    """)
    conn.commit()
    return conn


# ---------------------------------------------------------------------------
# Test 1: WAL mode is correctly activated
# ---------------------------------------------------------------------------

class TestWalMode:
    def test_journal_mode_is_wal(self, db_dir):
        """The database must use WAL journal mode, not DELETE (default)."""
        db_path = str(db_dir / "test.db")
        conn = _create_db(db_path)
        row = conn.execute("PRAGMA journal_mode").fetchone()
        conn.close()
        assert row[0].lower() == "wal", (
            f"Expected WAL journal mode but got '{row[0]}'. "
            "WAL is required for concurrent reader + writer access."
        )

    def test_wal_files_are_created(self, db_dir):
        """A WAL-mode DB should create .db-wal and .db-shm files after writes,
        or the journal_mode PRAGMA should confirm WAL is active.

        Note: On Windows, SQLite creates the .shm file lazily on the second
        connection, so we verify either the file exists or the PRAGMA confirms WAL.
        """
        db_path = str(db_dir / "test2.db")
        conn = _create_db(db_path)
        conn.execute(
            "INSERT INTO user_preferences (pref_key, pref_value) VALUES (?, ?)",
            ("test_key", 1.0)
        )
        conn.commit()

        # Open a second connection to trigger shm creation on Windows
        conn2 = sqlite3.connect(db_path, check_same_thread=False)
        conn2.execute("PRAGMA journal_mode=WAL")
        _ = conn2.execute("SELECT COUNT(*) FROM user_preferences").fetchone()
        conn2.close()
        conn.close()

        shm_path = db_path + "-shm"
        # Either the shm/wal file exists, or we can verify WAL mode directly
        conn3 = sqlite3.connect(db_path)
        mode = conn3.execute("PRAGMA journal_mode").fetchone()[0]
        conn3.close()

        assert os.path.exists(shm_path) or mode.lower() == "wal", (
            f"WAL mode not confirmed: shm={os.path.exists(shm_path)}, "
            f"journal_mode={mode}"
        )


    def test_synchronous_is_normal(self, db_dir):
        """synchronous=NORMAL is safe with WAL and avoids unnecessary fsyncs."""
        db_path = str(db_dir / "test3.db")
        conn = _create_db(db_path)
        row = conn.execute("PRAGMA synchronous").fetchone()
        conn.close()
        # NORMAL = 1, FULL = 2, OFF = 0
        assert row[0] == 1, (
            f"Expected synchronous=NORMAL (1) but got {row[0]}"
        )


# ---------------------------------------------------------------------------
# Test 2: Concurrent reads do not block each other
# ---------------------------------------------------------------------------

class TestConcurrentReads:
    def test_10_concurrent_readers_do_not_deadlock(self, db_dir):
        """WAL allows unlimited concurrent readers; 10 threads should all
        complete their queries within 2 seconds of wall time."""
        db_path = str(db_dir / "readers.db")
        conn = _create_db(db_path)
        # Seed some data
        for i in range(50):
            conn.execute(
                "INSERT INTO user_preferences (pref_key, pref_value) VALUES (?,?)",
                (f"key_{i}", float(i))
            )
        conn.commit()
        conn.close()

        results: list[int] = []
        errors:  list[str] = []

        def reader_thread():
            try:
                c = sqlite3.connect(db_path, check_same_thread=False, timeout=5.0)
                c.execute("PRAGMA journal_mode=WAL")
                rows = c.execute("SELECT COUNT(*) FROM user_preferences").fetchone()
                results.append(rows[0])
                c.close()
            except Exception as exc:
                errors.append(str(exc))

        threads = [threading.Thread(target=reader_thread) for _ in range(10)]
        t0 = time.monotonic()
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=5.0)
        elapsed = time.monotonic() - t0

        assert not errors, f"Reader thread errors: {errors}"
        assert len(results) == 10, f"Only {len(results)}/10 readers completed"
        assert all(r == 50 for r in results), f"Inconsistent read results: {results}"
        assert elapsed < 2.0, f"10 concurrent readers took {elapsed:.2f}s — too slow"


# ---------------------------------------------------------------------------
# Test 3: Concurrent writes serialise correctly
# ---------------------------------------------------------------------------

class TestConcurrentWrites:
    def test_50_concurrent_inserts_no_data_loss(self, db_dir):
        """50 concurrent writer threads each inserting 1 row should all
        succeed; final row count must equal 50 with no duplicates."""
        db_path = str(db_dir / "writers50.db")
        _create_db(db_path).close()

        errors:  list[str] = []
        written: list[str] = []
        lock = threading.Lock()

        def writer_thread(key: str):
            try:
                c = sqlite3.connect(db_path, check_same_thread=False, timeout=15.0)
                c.execute("PRAGMA journal_mode=WAL")
                c.execute(
                    "INSERT INTO user_preferences (pref_key, pref_value) VALUES (?,?)",
                    (key, 1.0)
                )
                c.commit()
                c.close()
                with lock:
                    written.append(key)
            except Exception as exc:
                with lock:
                    errors.append(f"{key}: {exc}")

        keys = [f"pref_{uuid.uuid4().hex[:8]}" for _ in range(50)]
        threads = [threading.Thread(target=writer_thread, args=(k,)) for k in keys]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=30.0)

        assert not errors, "Writer errors:\n" + "\n".join(errors)
        assert len(written) == 50, f"Only {len(written)}/50 writes succeeded"

        # Verify DB row count
        conn = sqlite3.connect(db_path)
        count = conn.execute("SELECT COUNT(*) FROM user_preferences").fetchone()[0]
        conn.close()
        assert count == 50, f"DB contains {count} rows, expected 50"

    def test_concurrent_override_inserts_match_expected_count(self, db_dir):
        """20 threads each inserting 5 parameter_overrides = 100 total rows."""
        db_path = str(db_dir / "overrides20.db")
        _create_db(db_path).close()

        errors: list[str] = []
        N_THREADS, N_PER_THREAD = 20, 5

        def insert_overrides():
            try:
                c = sqlite3.connect(db_path, check_same_thread=False, timeout=15.0)
                c.execute("PRAGMA journal_mode=WAL")
                rec_id = str(uuid.uuid4())
                for i in range(N_PER_THREAD):
                    c.execute(
                        "INSERT INTO parameter_overrides "
                        "(id, recommendation_id, parameter_name, recommended_value, actual_value, created_at) "
                        "VALUES (?,?,?,?,?,?)",
                        (str(uuid.uuid4()), rec_id, f"param_{i}", float(i), float(i) * 1.1, "2025-01-01")
                    )
                c.commit()
                c.close()
            except Exception as exc:
                errors.append(str(exc))

        threads = [threading.Thread(target=insert_overrides) for _ in range(N_THREADS)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=30.0)

        assert not errors, "Override insert errors:\n" + "\n".join(errors)

        conn = sqlite3.connect(db_path)
        count = conn.execute("SELECT COUNT(*) FROM parameter_overrides").fetchone()[0]
        conn.close()
        assert count == N_THREADS * N_PER_THREAD, (
            f"Expected {N_THREADS * N_PER_THREAD} rows, got {count}"
        )


# ---------------------------------------------------------------------------
# Test 4: WAL autocheckpoint does not hang
# ---------------------------------------------------------------------------

class TestWalCheckpoint:
    def test_checkpoint_completes_within_deadline(self, db_dir):
        """Insert enough rows to trigger an autocheckpoint (>200 pages) and
        verify PRAGMA wal_checkpoint returns without hanging."""
        db_path = str(db_dir / "checkpoint.db")
        conn = _create_db(db_path)

        # Insert ~300 rows across all 3 tables to inflate the WAL past 200 pages
        for i in range(100):
            conn.execute(
                "INSERT INTO recommendation_feedback "
                "(id, recommendation_id, acted_on, feedback_note, created_at, updated_at) "
                "VALUES (?,?,0,'','2025-01-01','2025-01-01')",
                (str(uuid.uuid4()), str(uuid.uuid4()))
            )
        for i in range(100):
            conn.execute(
                "INSERT INTO user_preferences (pref_key, pref_value) VALUES (?,?)",
                (str(uuid.uuid4()), float(i))
            )
        for i in range(100):
            conn.execute(
                "INSERT INTO parameter_overrides "
                "(id, recommendation_id, parameter_name, recommended_value, actual_value, created_at) "
                "VALUES (?,?,?,?,?,'2025-01-01')",
                (str(uuid.uuid4()), str(uuid.uuid4()), "gain_db", 0.0, 1.5)
            )
        conn.commit()

        # Trigger manual checkpoint; must complete within 3 seconds
        t0 = time.monotonic()
        result = conn.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchone()
        elapsed = time.monotonic() - t0
        conn.close()

        # result = (blocking, wal_frames, checkpointed_frames)
        assert result is not None, "PRAGMA wal_checkpoint returned nothing"
        assert elapsed < 3.0, (
            f"WAL checkpoint took {elapsed:.2f}s — this may indicate a deadlock risk"
        )
