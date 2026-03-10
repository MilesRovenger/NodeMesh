# mesh_node — ESP32 Custom Mesh Protocol

A hand-rolled mesh networking stack built on ESP-NOW and ESP-IDF (C).  
No ESP-MESH. No abstractions you didn't write. Every routing decision is yours.

---

## Project Stages

| Stage | Description | Status |
|-------|-------------|--------|
| 1 | Two-node ESP-NOW baseline + flooding skeleton | ✅ **Current** |
| 2 | Packet framing, deduplication, TTL enforcement | ✅ Included in Stage 1 code |
| 3 | 3+ node flooding demo | 🔜 |
| 4 | Neighbor discovery (HELLO beacons, RSSI table) | 🔜 |
| 5 | Unicast routing (distance-vector next-hop) | 🔜 |
| 6 | OLED display + serial visualizer | 🔜 |

---

## Hardware

- 2–5× ESP32 DevKit V1 (38-pin)
- USB cables for flashing + serial monitor
- Optional: SSD1306 128×64 OLED (I2C) for Stage 6

---

## Build & Flash

### Prerequisites

```bash
# Install ESP-IDF v5.x (https://docs.espressif.com/projects/esp-idf/en/latest/)
. $IDF_PATH/export.sh
```

### Build

```bash
cd mesh_node
idf.py build
```

### Flash

```bash
# Replace /dev/ttyUSB0 with your port (Linux) or COM3 (Windows)
idf.py -p /dev/ttyUSB0 flash monitor
```

Flash the same binary to all nodes — each board has a unique MAC, so they self-identify automatically.

---

## What to Watch in the Serial Monitor

```
I (342)  mesh_protocol: node MAC: AA:BB:CC:DD:EE:FF
I (350)  mesh_protocol: mesh_protocol ready
I (3350) main:           TX broadcast id=0 ttl=7 len=20
I (3360) mesh_protocol: RX id=0 ttl=7 type=0x01 len=20 from 11:22:... (rssi=-55 dBm)
I (3361) main:           APP RX | from 11:22:... | id=0 | msg: "hello from node #0"
I (3362) mesh_protocol: relayed id=0 with ttl=6
```

Key things to note:
- **TTL decrements** on each relay hop — watch it count down across nodes
- **Dedup cache** prevents a packet from being relayed twice by the same node
- **RSSI** is logged per-packet — useful for Stage 4 neighbor tables

---

## Code Structure

```
main/
├── main.c            — app_main, receive handler, periodic sender task
├── mesh_protocol.h   — packet struct (on-wire format), public API
├── mesh_protocol.c   — ESP-NOW init, send, recv, flooding relay
├── dedup_cache.h     — deduplication cache API
└── dedup_cache.c     — ring buffer implementation (thread-safe)
```

### Key Design Decisions

**Packed struct for on-wire format** (`__attribute__((packed))`)  
Prevents compiler padding from silently breaking compatibility between nodes.

**Separate dedup cache module**  
Flooding without dedup = infinite loops. The cache tracks `(src_mac, msg_id)` pairs and evicts oldest-first when full.

**Only send `MESH_HEADER_SIZE + payload_len` bytes**  
We don't transmit the unused tail of the 200-byte payload array. Keeps frames small.

**Self-MAC filtering in recv callback**  
ESP-NOW can deliver a node's own broadcast back to itself. We drop these by comparing `src_mac` to our own MAC.

---

## Concepts Demonstrated

- ESP-NOW peer-to-peer and broadcast communication
- FreeRTOS tasks and semaphore-protected shared state  
- On-wire protocol design with C packed structs
- Flooding with TTL and deduplication
- Layered separation: `main.c` (app) → `mesh_protocol` (transport) → `dedup_cache` (utility)
