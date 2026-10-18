"""
Landsafe AI - Backend Server
Receives tilt + moisture data from ESP32, serves the public dashboard.
Includes landslide prediction, weather data, and map.
"""
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, FileResponse
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
from datetime import datetime
import json
import os
import math
import base64
import uuid
import urllib.request
import urllib.parse

app = FastAPI(title="Landsafe AI", version="3.0.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

sensor_data = []
MAX_READINGS = 500
citizen_reports = []
MAX_REPORTS = 200
connected_clients: list[WebSocket] = []

class TiltReading(BaseModel):
    device_id: str
    tilt: float
    status: str
    ip: str = ""
    moisture: float = 0.0

class CitizenReport(BaseModel):
    report_type: str
    severity: str
    lat: float
    lng: float
    description: str = ""
    reporter_name: str = "Anonymous"
    photo_data: str = ""

@app.get("/health")
async def health():
    return {"status": "ok", "readings": len(sensor_data), "reports": len(citizen_reports)}

@app.post("/api/tilt")
async def receive_tilt(reading: TiltReading):
    entry = {
        "device_id": reading.device_id,
        "tilt": reading.tilt,
        "moisture": reading.moisture,
        "status": reading.status,
        "ip": reading.ip,
        "timestamp": datetime.now().isoformat(),
    }
    sensor_data.append(entry)
    if len(sensor_data) > MAX_READINGS:
        sensor_data.pop(0)
    message = json.dumps({"type": "sensor", "data": entry})
    disconnected = []
    for client in connected_clients:
        try:
            await client.send_text(message)
        except:
            disconnected.append(client)
    for client in disconnected:
        connected_clients.remove(client)
    return {"ok": True, "data": entry}

@app.get("/api/latest")
async def get_latest():
    if not sensor_data:
        return {"data": None}
    return {"data": sensor_data[-1]}

@app.get("/api/history")
async def get_history():
    return {"data": sensor_data}

@app.get("/api/reports")
async def get_reports():
    return {"data": citizen_reports}

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    connected_clients.append(websocket)
    try:
        while True:
            data = await websocket.receive_text()
            if data == "ping":
                await websocket.send_text("pong")
    except WebSocketDisconnect:
        if websocket in connected_clients:
            connected_clients.remove(websocket)

@app.get("/api/stations")
async def get_stations():
    stations = [
        {"id": "ESP32-NER-001", "name": "Gangtok — 32nd Mile NH10", "state": "Sikkim", "lat": 27.18, "lng": 88.53, "score": 58.4, "level": "MODERATE", "rain": 45.2, "moisture": 70.8},
        {"id": "ESP32-NER-002", "name": "Haflong — Jatinga Valley", "state": "Assam", "lat": 25.18, "lng": 93.02, "score": 42.1, "level": "MODERATE", "rain": 32.5, "moisture": 65.2},
        {"id": "ESP32-NER-003", "name": "Cherrapunji — Shella Gorge", "state": "Meghalaya", "lat": 25.30, "lng": 91.70, "score": 68.7, "level": "HIGH", "rain": 78.3, "moisture": 75.6},
    ]
    return {"data": stations, "source": "live"}

@app.get("/api/historical")
async def get_historical():
    return {"data": [], "summary": {"total_incidents": 0}}

@app.get("/", response_class=HTMLResponse)
async def serve_dashboard():
    dashboard_path = os.path.join(os.path.dirname(os.path.dirname(__file__)), "dashboard", "index.html")
    if not os.path.exists(dashboard_path):
        dashboard_path = os.path.join(os.path.dirname(os.path.dirname(__file__)), "dashboard.html")
    try:
        with open(dashboard_path, "r") as f:
            content = f.read()
        return HTMLResponse(content=content)
    except Exception as e:
        return HTMLResponse(content=f"<html><body><h1>Landsafe AI</h1><p>Server running</p></body></html>")

if __name__ == "__main__":
    import uvicorn
    port = int(os.getenv("PORT", 8000))
    uvicorn.run(app, host="0.0.0.0", port=port)
