There are several industry standards and research architectures that utilize the exact same structural components we have layered into your custom protocol. When you describe your architecture to other engineers, you can point to these existing systems to validate your design choices.

Your protocol sits at the intersection of three major technical fields: **Synchronized Channel Access (TDMA)**, **Delay-Tolerant Networking (DTN)**, and **Static Header Compression**.

The systems most similar to your network architecture are organized by how they map to your specific design choices:

---

### 1. In the Commercial IoT Space: Wi-Fi HaLow RAW

Your choice of a repeating **Epoch** split into a **Common Control Window (CCW)** and **TWT slots** mirrors the native **Restricted Access Window (RAW)** mechanism defined in the official **IEEE 802.11ah (Wi-Fi HaLow)** standard.

* **How it works:** In standard 802.11ah, an Access Point uses RAW to divide thousands of dense, battery-operated edge stations into distinct groups. The AP sets aside periodic windows where only certain devices are allowed to wake up and contend or use dedicated slots, forcing everyone else into deep sleep.
* **The Similarity:** Your "Soft TDMA" logic uses the exact same time-domain strategy. The only difference is that you are decentralizing this logic so that peer nodes can negotiate these slots autonomously rather than relying on a central, always-on standard Wi-Fi Access Point.

### 2. In Protocol Standards: SCHC & 6LoWPAN

Your stateless "Pseudo-IPv6" compression approach is nearly identical to the IETF standard **SCHC (Static Context Header Compression - RFC 8724)**.

* **How it works:** SCHC was explicitly created to allow ultra-low-power microcontrollers to communicate via IPv6 over highly restricted wireless links (like LoRaWAN and Sigfox) without sending full 40-byte IP headers. It operates on the assumption that the traffic format is highly predictable. Both sides hardcode a "Static Context Rule" (your network prefix and subnet mapping). The edge node drops the IP header over the air, and a network gateway reconstructs the standard IPv6 headers state-lessly before routing it onward.
* **The Similarity:** This is the exact philosophy behind your L2-to-L3 Border Router translator daemon. You have chosen the same resource trade-off: keep the edge light, and let the gateway do the heavy arithmetic.

### 3. In Tactical/Industrial Design: WirelessHART & ISA100.11a

If you look at heavy industrial sensing (oil rigs, refineries, and deep chemical plants), the standard protocols are **WirelessHART** and **ISA100.11a**.

* **How it works:** These networks operate on an ultra-strict, microsecond-accurate TDMA mesh fabric. Nodes do not use carrier sensing (CSMA/CA) to talk; they are disciplined by highly precise timing and slot schedules. They wake up for a tiny fraction of a second, transmit their payload, and go back to sleep. They also feature native frequency-hopping to defeat environmental attenuation.
* **The Similarity:** Your use of adaptive slot mapping and a strictly regimented Epoch timeline matches the core design engine of WirelessHART.

---

### The Final Validation: Why Your Blend is Unique

While these individual standards exist, **very few technologies merge all of them into a single protocol stack** like your forest mesh proposal:

```
                  ┌──────────────────────────────┐
                  │      YOUR CUSTOM PROTOCOL    │
                  └──────────────┬───────────────┘
                                 │
       ┌─────────────────────────┼─────────────────────────┐
       ▼                         ▼                         ▼
IEEE 802.11ah RAW          IETF SCHC (RFC 8724)      WirelessHART / P2P-DTN
(Sub-GHz physical range    (Stateless IPv6 header   (Strict time-slotted,
 with gated wake cycles)    compression over air)    gateway-less sleep paths)

```

1. **Wi-Fi HaLow RAW** is designed to be centralized (Star topology, Client-to-Access Point). It struggles when forced into a multi-hop mesh.
2. **WirelessHART** handles the multi-hop sleep paths perfectly, but it relies on short-range 2.4 GHz radios (IEEE 802.15.4), meaning it would fail across the 1km sparse distances of your forest setting.
3. **Reticulum** provides the decentralized P2P and DTN routing layer natively, but it does not dictate or enforce the underlying physical radio micro-sleep schedules (TWT/TDMA) needed to prevent the MM6108 from draining the battery.

By taking the **Sub-GHz physical range** of 802.11ah, applying a **WirelessHART-style distributed TDMA structure** to enforce sleep, and using a **SCHC-style translator** to make it cloud-compatible via IPv6, you are synthesizing an incredibly tailored protocol stack. You are combining proven architectural pieces to solve a specific, harsh real-world environment.