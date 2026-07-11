# plant-monitor

[![GitHub Repository](https://img.shields.io/badge/GitHub-plant--monitor-181717?logo=github)](https://github.com/FTW-Khushal/plant-monitor)
[![Docker Compose](https://img.shields.io/badge/Container-Docker%20Compose-2496ED?logo=docker)](https://docs.docker.com/compose/)

A simple plant-monitoring project that collects sensor readings from an ESP32 device, stores them in a local database, and serves a small web dashboard for viewing status and history.

## What it does
- Polls an ESP32 device over the local network
- Stores temperature, humidity, soil moisture, and light state
- Exposes REST API endpoints for health, status, history, and light control
- Runs with Docker Compose for a quick setup

## Quick start
1. Copy [.env.example](.env.example) to .env and fill in your ESP32 details.
2. Run:
   ```bash
   docker compose up --build
   ```
3. Open the dashboard at http://localhost:8000

## Configuration
The main environment variables are defined in [.env.example](.env.example):
- ESP32_URL
- ESP32_API_KEY
- POLL_INTERVAL_SECONDS
- HISTORY_RETENTION_DAYS

## Project structure
- backend/: FastAPI service and polling logic
- esp32_plant_monitor/: ESP32 firmware source
- docker-compose.yml: container orchestration for the backend
