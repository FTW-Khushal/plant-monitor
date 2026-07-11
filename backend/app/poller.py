import asyncio
import logging
import time

import httpx

from . import db

logger = logging.getLogger("poller")

_last_error = None
_last_success_ts = None
_latest_schedule = None


def get_poller_health():
    return {
        "last_success_ts": _last_success_ts,
        "last_error": _last_error,
        "seconds_since_success": (int(time.time()) - _last_success_ts)
        if _last_success_ts
        else None,
    }


def get_latest_schedule():
    return _latest_schedule


def set_latest_schedule(schedule: dict):
    global _latest_schedule
    _latest_schedule = schedule


async def poll_once(esp32_url: str, timeout: float = 5.0):
    global _last_error, _last_success_ts

    async with httpx.AsyncClient(timeout=timeout) as client:
        resp = await client.get(f"{esp32_url}/status")
        resp.raise_for_status()
        data = resp.json()

    temp = data.get("environment", {}).get("temperature")
    humidity = data.get("environment", {}).get("humidity")
    soil = data.get("soil", {}).get("percentage")
    light_on = data.get("light", {}).get("state", False)

    # ESP32 returns null (not "N/A") for failed DHT reads in the updated firmware
    temp = temp if isinstance(temp, (int, float)) else None
    humidity = humidity if isinstance(humidity, (int, float)) else None

    db.insert_reading(temp, humidity, soil, light_on, source="poll")

    schedule = data.get("schedule")
    if schedule:
        set_latest_schedule(schedule)

    _last_success_ts = int(time.time())
    _last_error = None


async def poll_loop(esp32_url: str, interval_seconds: int, stop_event: asyncio.Event):
    global _last_error

    logger.info(f"Starting poll loop: {esp32_url} every {interval_seconds}s")

    while not stop_event.is_set():
        try:
            await poll_once(esp32_url)
        except Exception as e:
            _last_error = str(e)
            logger.warning(f"Poll failed: {e}")

        try:
            await asyncio.wait_for(stop_event.wait(), timeout=interval_seconds)
        except asyncio.TimeoutError:
            pass  # normal - just means it's time to poll again
