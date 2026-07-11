# Plant Monitor

A small self-hosted dashboard for an ESP32 + soil moisture + DHT11 + grow light
relay setup. The backend polls the ESP32 on your LAN, stores history in
SQLite, and serves a dashboard you can reach remotely through a Cloudflare
Tunnel.

```
[Phone / Internet] --Cloudflare Tunnel--> [This backend, Docker] --LAN--> [ESP32]
```

The ESP32 itself is never exposed to the internet - only this backend is,
and only this backend knows the ESP32's API key.

## 1. Flash the ESP32

Use the firmware from earlier in this conversation. Make sure:
- `API_KEY` in the firmware matches `ESP32_API_KEY` below
- Note the IP address it prints over serial on boot (or set a DHCP
  reservation for it in your router so the IP never changes)

## 2. Configure environment

```bash
cp .env.example .env
```

Edit `.env`:
- `ESP32_URL` - e.g. `http://192.168.1.50` (no trailing slash)
- `ESP32_API_KEY` - must match the ESP32 firmware's `API_KEY`
- Leave `CLOUDFLARE_TUNNEL_TOKEN` blank for now - set it in step 4

## 3. Run locally first (confirm it works on your LAN before tunneling)

```bash
docker compose up -d backend
```

Visit `http://<your-server-ip>:8000` from a browser on your LAN. You should
see live readings within ~30 seconds (the first poll cycle). Check logs if
not:

```bash
docker compose logs -f backend
```

Common issue: `ESP32 unreachable` in `/api/health` - double check the ESP32's
IP hasn't changed and that the Docker host can reach it (same LAN/VLAN).

## 4. Set up Cloudflare Tunnel + Access (remote, login-gated access)

This part happens in the Cloudflare dashboard, not in this repo:

1. Go to **Cloudflare Zero Trust dashboard** → **Networks** → **Tunnels** →
   **Create a tunnel**. Choose "Cloudflare-managed" and name it e.g.
   `plant-monitor`.
2. On the "Install and run a connector" step, copy the **token** (a long
   string, not a URL) - paste it into `.env` as `CLOUDFLARE_TUNNEL_TOKEN`.
   You don't need to run the install command Cloudflare shows you - the
   `cloudflared` service in `docker-compose.yml` handles that.
3. Add a **Public hostname**:
   - Subdomain: something like `plants`
   - Domain: a domain you have in Cloudflare (or use a free
     `*.trycloudflare.com` if you don't own one - check current Cloudflare
     docs for the quick-tunnel flow if so)
   - Service: `HTTP` → `backend:8000` (the docker service name + port,
     since cloudflared reaches it over the internal Docker network)
4. Go to **Access** → **Applications** → **Add an application** → **Self-hosted**.
   - Point it at the same public hostname from step 3
   - Add a policy, e.g. "Allow" if email matches your email address (or set
     up a one-time-PIN / identity provider login)
   - This is what puts a login screen in front of your dashboard before
     anyone reaches the backend at all

5. Start the tunnel:

```bash
docker compose up -d
```

Visit your public hostname (e.g. `https://plants.yourdomain.com`) - you
should hit a Cloudflare Access login page first, then the dashboard after
authenticating.

## Notes

- **Data persistence**: SQLite lives in the `plant-data` Docker volume, so
  it survives container rebuolds/restarts. Back it up with
  `docker run --rm -v plant-monitor_plant-data:/data -v $(pwd):/backup alpine tar czf /backup/plant-data-backup.tar.gz /data`
  if you want an occasional snapshot.
- **Polling vs. live**: `/api/status` reads from the local database (fast,
  always responds even if the ESP32 is briefly offline). `/api/status/live`
  bypasses the cache and calls the ESP32 directly - useful for debugging,
  not used by the dashboard itself.
- **Changing poll interval**: edit `POLL_INTERVAL_SECONDS` in `.env`, then
  `docker compose up -d --force-recreate backend`.
- **The `8000:8000` port in docker-compose.yml** is only for local LAN
  testing/debugging. You can remove it once the tunnel is working if you
  don't want the dashboard reachable on the LAN outside of Docker's internal
  network.
