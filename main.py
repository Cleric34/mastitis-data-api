"""ESP32 Ingestion API with Instantaneous Live Display & Dashboard."""
from datetime import datetime, timezone
from typing import Literal, Optional, Union, List
import os
import logging
from fastapi import Depends, FastAPI, status, Request
from fastapi.responses import HTMLResponse
from pydantic import BaseModel, Field
from sqlalchemy import DateTime, Float, Integer, String, UniqueConstraint, create_engine, desc
from sqlalchemy.orm import DeclarativeBase, Mapped, Session, mapped_column, sessionmaker

# Configure Logging
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("mastitis-api")

DATABASE_URL = os.getenv("DATABASE_URL", "sqlite:///./mastitis_local.db")
engine = create_engine(DATABASE_URL, pool_pre_ping=True)
SessionLocal = sessionmaker(bind=engine, autocommit=False, autoflush=False)

class Base(DeclarativeBase): pass

class Cow(Base):
    __tablename__ = "cows"
    rfid_uid: Mapped[str] = mapped_column(String(64), primary_key=True)
    cow_id: Mapped[str] = mapped_column(String(64), index=True)
    device_id: Mapped[str] = mapped_column(String(64))
    registered_at_device: Mapped[Optional[str]] = mapped_column(String(64), nullable=True)
    received_at: Mapped[datetime] = mapped_column(DateTime(timezone=True))

class TestRecord(Base):
    __tablename__ = "test_records"
    __table_args__ = (UniqueConstraint("test_id", name="uq_test_record_test_id"),)
    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    test_id: Mapped[str] = mapped_column(String(96), index=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    cow_id: Mapped[str] = mapped_column(String(64), index=True)
    rfid_uid: Mapped[str] = mapped_column(String(64), index=True)
    quarter: Mapped[int] = mapped_column(Integer)
    ec: Mapped[float] = mapped_column(Float)
    ph: Mapped[float] = mapped_column(Float)
    temperature: Mapped[float] = mapped_column(Float)
    timestamp_device: Mapped[Optional[str]] = mapped_column(String(64), nullable=True)
    sequence_id: Mapped[Optional[int]] = mapped_column(Integer, nullable=True)
    time_source: Mapped[str] = mapped_column(String(16))
    delivery_mode: Mapped[str] = mapped_column(String(16))
    received_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), index=True)

class CowRegistration(BaseModel):
    device_id: str = Field(min_length=1, max_length=64)
    cow_id: str = Field(min_length=1, max_length=64)
    rfid_uid: str = Field(min_length=1, max_length=64)
    registered_at: Optional[Union[int, str]] = None

class TestMeasurement(BaseModel):
    test_id: str = Field(min_length=1, max_length=96)
    device_id: str = Field(min_length=1, max_length=64)
    cow_id: str = Field(min_length=1, max_length=64)
    rfid_uid: str = Field(min_length=1, max_length=64)
    quarter: int = Field(ge=1, le=4)
    ec: float
    ph: float
    temperature: float
    timestamp: Optional[Union[int, str]] = None
    sequence_id: Optional[int] = Field(default=None, ge=1)
    time_source: Literal["rtc", "server", "sequence"] = "sequence"
    delivery_mode: Literal["live", "sd_sync"]

def utc_now() -> datetime: return datetime.now(timezone.utc)

def db_session():
    db = SessionLocal()
    try: yield db
    finally: db.close()

app = FastAPI(title="Mastitis ESP32 Ingestion & Live Display API", version="2.1.0")

@app.on_event("startup")
def create_tables() -> None:
    Base.metadata.create_all(bind=engine)
    logger.info("Database tables verified.")

@app.get("/health")
def health() -> dict:
    return {"status": "ok", "service": "mastitis-data-api"}

@app.post("/api/v1/cows", status_code=status.HTTP_201_CREATED)
def register_cow(cow: CowRegistration, db: Session = Depends(db_session)) -> dict:
    row = db.get(Cow, cow.rfid_uid)
    created = row is None
    if row is None:
        db.add(Cow(rfid_uid=cow.rfid_uid, cow_id=cow.cow_id, device_id=cow.device_id,
                   registered_at_device=str(cow.registered_at) if cow.registered_at is not None else None,
                   received_at=utc_now()))
    else:
        row.cow_id, row.device_id = cow.cow_id, cow.device_id
    db.commit()

    # Instantaneous Terminal Console Print
    print("\n" + "="*50)
    print(" 🐮 [INCOMING COW REGISTRATION RECEIVED]")
    print(f"  Cow ID       : {cow.cow_id}")
    print(f"  RFID UID     : {cow.rfid_uid}")
    print(f"  Device ID    : {cow.device_id}")
    print(f"  Status       : {'CREATED NEW RECORD' if created else 'UPDATED RECORD'}")
    print("="*50 + "\n")

    return {"accepted": True, "created": created, "rfid_uid": cow.rfid_uid}

@app.post("/api/v1/tests", status_code=status.HTTP_201_CREATED)
def store_test(measurement: TestMeasurement, db: Session = Depends(db_session)) -> dict:
    existing = db.query(TestRecord).filter(TestRecord.test_id == measurement.test_id).first()
    if existing:
        print("\n" + "-"*50)
        print(f" ⚠️ [DUPLICATE TEST IGNORED] Test ID: {measurement.test_id}")
        print("-"*50 + "\n")
        return {"accepted": True, "duplicate": True, "test_id": measurement.test_id}

    db.add(TestRecord(
        test_id=measurement.test_id, device_id=measurement.device_id,
        cow_id=measurement.cow_id, rfid_uid=measurement.rfid_uid, quarter=measurement.quarter,
        ec=measurement.ec, ph=measurement.ph, temperature=measurement.temperature,
        timestamp_device=str(measurement.timestamp) if measurement.timestamp is not None else None,
        sequence_id=measurement.sequence_id, time_source=measurement.time_source,
        delivery_mode=measurement.delivery_mode, received_at=utc_now()
    ))
    db.commit()

    # Instantaneous Terminal Console Print
    print("\n" + "🚀 "*15)
    print("  [LIVE ESP32 TEST DATA RECEIVED & STORED]")
    print(f"  • Test ID      : {measurement.test_id}")
    print(f"  • Device ID    : {measurement.device_id}")
    print(f"  • Cow ID       : {measurement.cow_id} (RFID: {measurement.rfid_uid})")
    print(f"  • Quarter      : Q{measurement.quarter}")
    print(f"  • EC           : {measurement.ec:.2f} mS/cm")
    print(f"  • pH           : {measurement.ph:.2f}")
    print(f"  • Temperature  : {measurement.temperature:.1f} °C")
    print(f"  • Delivery Mode: {measurement.delivery_mode}")
    print(f"  • Device Time  : {measurement.timestamp or 'Sequence (' + str(measurement.sequence_id) + ')'}")
    print("🚀 "*15 + "\n")

    return {"accepted": True, "duplicate": False, "test_id": measurement.test_id}

@app.get("/api/v1/live", response_model=dict)
def get_live_records(db: Session = Depends(db_session)) -> dict:
    """Returns recent test records and cow registrations for auto-refresh dashboard."""
    tests = db.query(TestRecord).order_by(desc(TestRecord.id)).limit(15).all()
    cows = db.query(Cow).order_by(desc(Cow.received_at)).limit(10).all()

    return {
        "tests": [
            {
                "test_id": t.test_id,
                "cow_id": t.cow_id,
                "rfid_uid": t.rfid_uid,
                "quarter": f"Q{t.quarter}",
                "ec": t.ec,
                "ph": t.ph,
                "temperature": t.temperature,
                "delivery_mode": t.delivery_mode,
                "timestamp_device": t.timestamp_device,
                "received_at": t.received_at.strftime("%Y-%m-%d %H:%M:%S UTC") if t.received_at else ""
            } for t in tests
        ],
        "cows": [
            {
                "cow_id": c.cow_id,
                "rfid_uid": c.rfid_uid,
                "device_id": c.device_id,
                "received_at": c.received_at.strftime("%Y-%m-%d %H:%M:%S UTC") if c.received_at else ""
            } for c in cows
        ]
    }

@app.get("/", response_class=HTMLResponse)
def live_dashboard():
    """Instantaneous Live Monitoring Web Dashboard."""
    return """
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Mastitis Detector - Live Monitor</title>
    <style>
        :root {
            --bg-color: #0f172a;
            --card-bg: #1e293b;
            --accent-green: #10b981;
            --accent-blue: #3b82f6;
            --accent-amber: #f59e0b;
            --text-main: #f8fafc;
            --text-sub: #94a3b8;
        }
        body {
            font-family: system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background-color: var(--bg-color);
            color: var(--text-main);
            margin: 0;
            padding: 24px;
        }
        .header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding-bottom: 16px;
            border-bottom: 1px solid #334155;
            margin-bottom: 24px;
        }
        .status-badge {
            background-color: rgba(16, 185, 129, 0.2);
            color: var(--accent-green);
            padding: 6px 16px;
            border-radius: 9999px;
            font-weight: 600;
            font-size: 0.875rem;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        .pulse-dot {
            width: 8px;
            height: 8px;
            background-color: var(--accent-green);
            border-radius: 50%;
            animation: pulse 1.5s infinite;
        }
        @keyframes pulse {
            0% { transform: scale(0.95); box-shadow: 0 0 0 0 rgba(16, 185, 129, 0.7); }
            70% { transform: scale(1); box-shadow: 0 0 0 8px rgba(16, 185, 129, 0); }
            100% { transform: scale(0.95); box-shadow: 0 0 0 0 rgba(16, 185, 129, 0); }
        }
        .hero-card {
            background: linear-gradient(135deg, #1e293b 0%, #0f172a 100%);
            border: 1px solid #334155;
            border-radius: 12px;
            padding: 20px;
            margin-bottom: 24px;
            box-shadow: 0 10px 15px -3px rgba(0, 0, 0, 0.3);
        }
        .hero-title {
            color: var(--text-sub);
            font-size: 0.875rem;
            text-transform: uppercase;
            letter-spacing: 0.05em;
            margin-bottom: 12px;
        }
        .metrics-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
            gap: 16px;
        }
        .metric-box {
            background-color: #0f172a;
            padding: 16px;
            border-radius: 8px;
            border: 1px solid #334155;
        }
        .metric-label { font-size: 0.75rem; color: var(--text-sub); margin-bottom: 4px; }
        .metric-value { font-size: 1.5rem; font-weight: 700; color: #ffffff; }
        table {
            width: 100%;
            border-collapse: collapse;
            background-color: var(--card-bg);
            border-radius: 12px;
            overflow: hidden;
            box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.2);
        }
        th, td {
            padding: 12px 16px;
            text-align: left;
        }
        th {
            background-color: #0f172a;
            color: var(--text-sub);
            font-size: 0.75rem;
            text-transform: uppercase;
            letter-spacing: 0.05em;
        }
        tr:nth-child(even) { background-color: #1a2332; }
        tr:hover { background-color: #263346; }
        .badge {
            padding: 4px 10px;
            border-radius: 6px;
            font-size: 0.75rem;
            font-weight: 600;
        }
        .badge-live { background-color: rgba(59, 130, 246, 0.2); color: var(--accent-blue); }
        .badge-sync { background-color: rgba(245, 158, 11, 0.2); color: var(--accent-amber); }
    </style>
</head>
<body>
    <div class="header">
        <div>
            <h1 style="margin:0; font-size: 1.5rem;">ESP32 Mastitis Data Monitor</h1>
            <p style="margin:4px 0 0 0; color: var(--text-sub); font-size: 0.875rem;">Instantaneous Data Ingestion Verification</p>
        </div>
        <div class="status-badge">
            <div class="pulse-dot"></div>
            LISTENING FOR ESP32 DATA
        </div>
    </div>

    <div class="hero-card">
        <div class="hero-title">Latest Received Sensor Data</div>
        <div class="metrics-grid">
            <div class="metric-box">
                <div class="metric-label">Cow ID</div>
                <div class="metric-value" id="latest-cow">--</div>
            </div>
            <div class="metric-box">
                <div class="metric-label">Teat Quarter</div>
                <div class="metric-value" id="latest-quarter">--</div>
            </div>
            <div class="metric-box">
                <div class="metric-label">Electrical Conductivity</div>
                <div class="metric-value" style="color:#60a5fa;" id="latest-ec">-- mS/cm</div>
            </div>
            <div class="metric-box">
                <div class="metric-label">pH Level</div>
                <div class="metric-value" style="color:#34d399;" id="latest-ph">--</div>
            </div>
            <div class="metric-box">
                <div class="metric-label">Temperature</div>
                <div class="metric-value" style="color:#f43f5e;" id="latest-temp">-- °C</div>
            </div>
            <div class="metric-box">
                <div class="metric-label">Delivery Mode</div>
                <div class="metric-value" id="latest-mode">--</div>
            </div>
        </div>
    </div>

    <h2>Incoming Test Data Log</h2>
    <table>
        <thead>
            <tr>
                <th>Test ID</th>
                <th>Cow ID</th>
                <th>RFID UID</th>
                <th>Quarter</th>
                <th>EC (mS/cm)</th>
                <th>pH</th>
                <th>Temp (°C)</th>
                <th>Mode</th>
                <th>Server Received Time</th>
            </tr>
        </thead>
        <tbody id="test-table-body">
            <tr><td colspan="9" style="text-align:center; color: var(--text-sub);">Waiting for ESP32 data...</td></tr>
        </tbody>
    </table>

    <script>
        async function fetchLiveRecords() {
            try {
                const res = await fetch('/api/v1/live');
                const data = await res.json();
                
                if (data.tests && data.tests.length > 0) {
                    const latest = data.tests[0];
                    document.getElementById('latest-cow').innerText = latest.cow_id;
                    document.getElementById('latest-quarter').innerText = latest.quarter;
                    document.getElementById('latest-ec').innerText = latest.ec.toFixed(2) + ' mS/cm';
                    document.getElementById('latest-ph').innerText = latest.ph.toFixed(2);
                    document.getElementById('latest-temp').innerText = latest.temperature.toFixed(1) + ' °C';
                    document.getElementById('latest-mode').innerText = latest.delivery_mode.toUpperCase();

                    const tbody = document.getElementById('test-table-body');
                    tbody.innerHTML = data.tests.map(t => `
                        <tr>
                            <td style="font-family: monospace; font-weight:600;">${t.test_id}</td>
                            <td><strong>${t.cow_id}</strong></td>
                            <td style="color: var(--text-sub);">${t.rfid_uid}</td>
                            <td>${t.quarter}</td>
                            <td style="color:#60a5fa; font-weight:600;">${t.ec.toFixed(2)}</td>
                            <td style="color:#34d399; font-weight:600;">${t.ph.toFixed(2)}</td>
                            <td style="color:#f43f5e; font-weight:600;">${t.temperature.toFixed(1)}</td>
                            <td><span class="badge ${t.delivery_mode === 'live' ? 'badge-live' : 'badge-sync'}">${t.delivery_mode.toUpperCase()}</span></td>
                            <td style="color: var(--text-sub);">${t.received_at}</td>
                        </tr>
                    `).join('');
                }
            } catch (err) {
                console.error("Failed to fetch live records:", err);
            }
        }

        // Auto-refresh every 2 seconds
        setInterval(fetchLiveRecords, 2000);
        fetchLiveRecords();
    </script>
</body>
</html>
    """
