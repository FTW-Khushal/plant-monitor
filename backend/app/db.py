import sqlite3
import time
from pathlib import Path
from contextlib import contextmanager

DB_PATH = Path(__file__).parent / "data" / "plant.db"
DB_PATH.parent.mkdir(parents=True, exist_ok=True)


@contextmanager
def get_conn():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    try:
        yield conn
        conn.commit()
    finally:
        conn.close()


def init_db():
    with get_conn() as conn:
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS readings (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts INTEGER NOT NULL,
                temp_c REAL,
                humidity REAL,
                soil_pct INTEGER,
                light_on INTEGER,
                source TEXT DEFAULT 'poll'
            )
            """
        )
        conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_readings_ts ON readings (ts)"
        )


def insert_reading(temp_c, humidity, soil_pct, light_on, source="poll"):
    with get_conn() as conn:
        conn.execute(
            """
            INSERT INTO readings (ts, temp_c, humidity, soil_pct, light_on, source)
            VALUES (?, ?, ?, ?, ?, ?)
            """,
            (int(time.time()), temp_c, humidity, soil_pct, int(bool(light_on)), source),
        )


def get_latest():
    with get_conn() as conn:
        row = conn.execute(
            "SELECT * FROM readings ORDER BY ts DESC LIMIT 1"
        ).fetchone()
        return dict(row) if row else None


def get_history(hours=24, limit=2000):
    cutoff = int(time.time()) - hours * 3600
    with get_conn() as conn:
        rows = conn.execute(
            """
            SELECT ts, temp_c, humidity, soil_pct, light_on
            FROM readings
            WHERE ts >= ?
            ORDER BY ts ASC
            LIMIT ?
            """,
            (cutoff, limit),
        ).fetchall()
        return [dict(r) for r in rows]


def prune_older_than(days=30):
    cutoff = int(time.time()) - days * 86400
    with get_conn() as conn:
        conn.execute("DELETE FROM readings WHERE ts < ?", (cutoff,))
