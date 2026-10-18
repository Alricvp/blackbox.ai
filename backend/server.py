"""
Landsafe AI - Backend Server (Dashboard Compatible)
"""
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
from datetime import datetime
import json
import os
import time

app = FastAPI(title="Landsafe AI", version="3.0.0")
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

sensor_data = []
MAX_READINGS = 500
nodes = {}  # node_id -> node state
connected_clients: list[WebSocket] = []

class TiltReading(BaseModel):
    device_id: str = ""
    id: str = ""
    tilt: float = 0.0
    moisture: float = 0.0
    moisture_pct: int = 0
    status: str = "safe"
    ip: str = ""
    vib: float = 0.0
    rms_vib: float = 0.0
    sound_db: float = 0.0
    leak: int = 0

@app.get("/health")
async def health():
    return {"status": "ok", "readings": len(sensor_data), "nodes": len(nodes)}

@app.post("/api/tilt")
async def receive_tilt(reading: dict):
    rec = dict(reading)
    device_id = rec.get("device_id") or rec.get("id") or "unknown"
    rec["id"] = device_id
    rec["_recv_t"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    rec["time"] = time.time()
    
    # Map to dashboard fields
    if "rms_vib" in rec and "vib" not in rec:
        rec["vib"] = rec["rms_vib"]
    if "moisture_pct" in rec and "moisture" not in rec:
        rec["moisture"] = rec["moisture_pct"]
    if "sound_db" in rec:
        rec["sound"] = rec["sound_db"]
    
    sensor_data.append(rec)
    if len(sensor_data) > MAX_READINGS:
        sensor_data.pop(0)
    
    # Build node state for dashboard
    nodes[device_id] = {
        "id": device_id,
        "online": True,
        "last_seen": rec["_recv_t"],
        "latest": {
            "vib": rec.get("vib", rec.get("rms_vib", 0)),
            "freq": rec.get("freq", 0),
            "tilt": rec.get("tilt", 0),
            "sound": rec.get("sound", rec.get("sound_db", 0)),
            "leak": rec.get("leak", 0) if rec.get("leak") is not None else rec.get("moisture", 0),
        },
        "findings": []
    }
    
    # Broadcast frame in dashboard format
    frame = {
        "time": time.time(),
        "nodes": nodes
    }
    message = json.dumps(frame)
    disconnected = []
    for client in connected_clients:
        try:
            await client.send_text(message)
        except:
            disconnected.append(client)
    for c in disconnected:
        connected_clients.remove(c)
    
    return {"ok": True}

@app.get("/api/latest")
async def get_latest():
    return {"ok": True, "nodes": nodes}

@app.get("/api/history")
async def get_history(n: int = 50):
    return {"ok": True, "count": len(sensor_data[-n:]), "data": sensor_data[-n:]}

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    connected_clients.append(websocket)
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        if websocket in connected_clients:
            connected_clients.remove(websocket)

@app.get("/", response_class=HTMLResponse)
async def serve_dashboard():
    dashboard_path = os.path.join(os.path.dirname(os.path.dirname(__file__)), "dashboard", "index.html")
    try:
        with open(dashboard_path, "r") as f:
            content = f.read()
        return HTMLResponse(content=content)
    except Exception as e:
        return HTMLResponse(content="<html><body><h1>Landsafe AI</h1></body></html>")

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=int(os.getenv("PORT", 8000)))
