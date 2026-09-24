# ESP32 raw-data ingestion contract

This service does **not** calculate risk or send results back to the ESP32. It stores raw records in PostgreSQL. The ML team and dashboard use those records separately.

## Workflow

1. ESP32 always saves a cow registration locally first. It sends `POST /api/v1/cows` immediately if online; otherwise it retries the local queued registration later.
2. For every test the ESP32 creates a permanent `test_id`, reads EC, pH, temperature, and quarter.
3. If online, it sends the record to `POST /api/v1/tests` with `delivery_mode: "live"`.
4. If offline or the request does not receive a successful response in time, it writes the raw record to SD.
5. During a later sync it POSTs that exact record to the same endpoint with `delivery_mode: "sd_sync"`. It deletes the SD file only after a successful `2xx` response.

No local baseline or local risk formula remains in this new firmware design.

## Test JSON

```json
{
  "test_id": "SENSOR_NODE_04_000001",
  "device_id": "SENSOR_NODE_04",
  "cow_id": "8492",
  "rfid_uid": "A1:B2:C3:D4",
  "quarter": 2,
  "ec": 5.42,
  "ph": 6.65,
  "temperature": 38.5,
  "timestamp": null,
  "sequence_id": 1,
  "time_source": "sequence",
  "delivery_mode": "live"
}
```

`timestamp` can become a Unix timestamp when RTC time is available. Until then it stays `null`; cloud `received_at` is authoritative.

## Success reply

```json
{ "accepted": true, "duplicate": false, "test_id": "SENSOR_NODE_04_000001" }
```

Repeated uploads with the same `test_id` are safe: the API replies with `duplicate: true` and does not add another row.

## ML/dashboard handoff

Use PostgreSQL table `test_records`. The raw ML features are `ec`, `ph`, `temperature`, and `quarter`; identifiers and timing fields allow per-cow and per-quarter history. The dashboard/ML service may write separate prediction tables, but it must not depend on an ESP32 response.
