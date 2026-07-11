import asyncio
import logging
import os

import httpx
from fastapi import FastAPI, HTTPException
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse

from . import db
from . import poller

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("main")

ESP32_URL = os.environ.get("ESP32_URL", "http://192.168.1.50")
ESP32_API_KEY = os.environ.get("ESP32_API_KEY", "")
POLL_INTERVAL_SECONDS = int(os.environ.get("POLL_INTERVAL_SECONDS", "30"))
HISTORY_RETENTION_DAYS = int(os.environ.get("HISTORY_RETENTION_DAYS", "30"))

app = FastAPI(title="Plant Monitor")

_stop_event = asyncio.Event()
_poller_task = None


@app.on_event("startup")
async def startup():
    db.init_db()
    global _poller_task
    _poller_task = asyncio.create_task(
        poller.poll_loop(ESP32_URL, POLL_INTERVAL_SECONDS, _stop_event)
    )
    logger.info("App started, poller task launched")


@app.on_event("shutdown")
async def shutdown():
    _stop_event.set()
    if _poller_task:
        await _poller_task


# =========================
# API routes
# =========================

@app.get("/api/health")
async def health():
    return {
        "status": "ok",
        "poller": poller.get_poller_health(),
    }


@app.get("/api/status")
async def status():
    """Latest reading from the local database (fast, no live ESP32 call)."""
    latest = db.get_latest()
    if not latest:
        raise HTTPException(status_code=404, detail="No readings yet")
    return latest


@app.get("/api/status/live")
async def status_live():
    """Bypasses the DB and hits the ESP32 directly for a fresh reading.
    Use sparingly - this is a live LAN call, not from cache."""
    try:
        async with httpx.AsyncClient(timeout=5.0) as client:
            resp = await client.get(f"{ESP32_URL}/status")
            resp.raise_for_status()
            return resp.json()
    except httpx.HTTPError as e:
        raise HTTPException(status_code=502, detail=f"ESP32 unreachable: {e}")


@app.get("/api/history")
async def history(hours: int = 24):
    if hours <= 0 or hours > 24 * 30:
        raise HTTPException(status_code=400, detail="hours must be between 1 and 720")
    return {"readings": db.get_history(hours=hours)}


@app.post("/api/light")
async def set_light(state: str):
    if state not in ("on", "off"):
        raise HTTPException(status_code=400, detail="state must be 'on' or 'off'")

    try:
        async with httpx.AsyncClient(timeout=5.0) as client:
            # Disable the ESP32's automatic schedule first, so this manual
            # toggle doesn't just get reverted at the device's next
            # schedule check (~60s). Best-effort: if this call fails we
            # still proceed with the toggle, but log it since the light
            # may flip back on its own shortly after.
            try:
                sched_resp = await client.post(
                    f"{ESP32_URL}/schedule",
                    params={"enabled": "false", "key": ESP32_API_KEY},
                )
                sched_resp.raise_for_status()
                sched_data = sched_resp.json()
                poller.set_latest_schedule(
                    {**(poller.get_latest_schedule() or {}), "enabled": sched_data.get("enabled", False)}
                )
            except httpx.HTTPError as e:
                logger.warning(f"Could not disable ESP32 schedule before manual toggle: {e}")

            resp = await client.post(
                f"{ESP32_URL}/light",
                params={"state": state, "key": ESP32_API_KEY},
            )
            resp.raise_for_status()
            result = resp.json()
    except httpx.HTTPError as e:
        raise HTTPException(status_code=502, detail=f"ESP32 unreachable: {e}")

    # Record this as an immediate reading so the dashboard/history reflects
    # the change right away instead of waiting for the next poll cycle.
    latest = db.get_latest()
    db.insert_reading(
        temp_c=latest["temp_c"] if latest else None,
        humidity=latest["humidity"] if latest else None,
        soil_pct=latest["soil_pct"] if latest else None,
        light_on=result.get("state", state == "on"),
        source="manual",
    )

    return result


@app.get("/api/schedule")
async def get_schedule():
    """Latest known schedule state, from the most recent poll cycle."""
    schedule = poller.get_latest_schedule()
    if schedule is None:
        raise HTTPException(status_code=404, detail="No schedule info yet - waiting on first poll")
    return schedule


@app.post("/api/schedule/resume")
async def resume_schedule():
    """Re-enables the ESP32's automatic light schedule after a manual override."""
    try:
        async with httpx.AsyncClient(timeout=5.0) as client:
            resp = await client.post(
                f"{ESP32_URL}/schedule",
                params={"enabled": "true", "key": ESP32_API_KEY},
            )
            resp.raise_for_status()
            result = resp.json()
    except httpx.HTTPError as e:
        raise HTTPException(status_code=502, detail=f"ESP32 unreachable: {e}")

    poller.set_latest_schedule(
        {**(poller.get_latest_schedule() or {}), "enabled": result.get("enabled", True)}
    )
    return result


# =========================
# Static frontend
# =========================

STATIC_DIR = os.path.join(os.path.dirname(__file__), "static")
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


@app.get("/")
async def index():
    return FileResponse(os.path.join(STATIC_DIR, "index.html"))
