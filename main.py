"""ESP32 ingestion API: raw registrations and test data, no scoring."""
from datetime import datetime, timezone
from typing import Literal, Optional, Union
import os
from fastapi import Depends, FastAPI, status
from pydantic import BaseModel, Field
from sqlalchemy import DateTime, Float, Integer, String, UniqueConstraint, create_engine
from sqlalchemy.orm import DeclarativeBase, Mapped, Session, mapped_column, sessionmaker

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

app = FastAPI(title="Mastitis ESP32 Data API", version="2.0.0")
@app.on_event("startup")
def create_tables() -> None: Base.metadata.create_all(bind=engine)
@app.get("/health")
def health() -> dict: return {"status": "ok", "service": "mastitis-data-api"}

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
    return {"accepted": True, "created": created, "rfid_uid": cow.rfid_uid}

@app.post("/api/v1/tests", status_code=status.HTTP_201_CREATED)
def store_test(measurement: TestMeasurement, db: Session = Depends(db_session)) -> dict:
    existing = db.query(TestRecord).filter(TestRecord.test_id == measurement.test_id).first()
    if existing: return {"accepted": True, "duplicate": True, "test_id": measurement.test_id}
    db.add(TestRecord(test_id=measurement.test_id, device_id=measurement.device_id,
        cow_id=measurement.cow_id, rfid_uid=measurement.rfid_uid, quarter=measurement.quarter,
        ec=measurement.ec, ph=measurement.ph, temperature=measurement.temperature,
        timestamp_device=str(measurement.timestamp) if measurement.timestamp is not None else None,
        sequence_id=measurement.sequence_id, time_source=measurement.time_source,
        delivery_mode=measurement.delivery_mode, received_at=utc_now()))
    db.commit()
    return {"accepted": True, "duplicate": False, "test_id": measurement.test_id}
