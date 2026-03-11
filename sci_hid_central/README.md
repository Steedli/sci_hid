# SCI HID Central

## Overview

This sample demonstrates a Bluetooth LE HID central device that scans for HID peripherals, connects to them using the HID-over-GATT Profile (HOGP), and forwards received HID mouse reports to a PC via USB HID. The application supports Shorter Connection Intervals (SCI) for ultra-low latency HID report forwarding.

## Features

- **BLE HID Central (HOGP)**: Discovers and subscribes to HID input reports from peripherals
- **USB HID Device**: Acts as a USB HID mouse to the PC
- **SCI Negotiation**: Requests connection intervals as low as 750 µs
- **Automatic Connection**: Scans and connects to devices advertising HID Service
- **Report Forwarding**: Zero-copy forwarding from BLE to USB via message queue

## Requirements

- **Board**: nRF54LM20 DK (nrf54lm20dk/nrf54lm20a/cpuapp)
- **SDK**: nRF Connect SDK v3.2.3 or later
- **USB Cable**: Connect board to PC via USB
- **Peripheral Device**: Compatible BLE HID mouse (e.g., sci_hid_peripheral)

## Building and Running

### Build

```bash
west build -b nrf54lm20dk/nrf54lm20a/cpuapp
```

### Flash

```bash
west flash
```

## Operation

1. **Power On**: Device initializes USB HID and starts BLE scanning
2. **USB Enumeration**: PC recognizes the device as "SCI HID Mouse"
3. **BLE Discovery**: Automatically connects to peripherals advertising HID Service (UUID 0x1812)
4. **Service Discovery**: Discovers HID Service and SCI service on peripheral
5. **SCI Negotiation**:
   - Updates PHY to 2M
   - Minimizes frame space
   - Requests 750 µs connection interval
6. **Report Forwarding**: Receives HID reports via HOGP notifications, forwards to USB

## Technical Details

### Scan Configuration

- **Scan Type**: Passive
- **Filter**: UUID = HID Service (0x1812)
- **Connect on Match**: Automatic
- **Scan Parameters**:
  - Interval: 0x0020 (20 ms)
  - Window: 0x0010 (10 ms)

### Connection Parameters

**Initial Connection**:
- Interval: 10 ms (8 units)

**After SCI Negotiation**:
- Interval: 750 µs (6 units of 125 µs)
- Subrating: 1 (no subrating)
- Latency: 0
- Supervision Timeout: 4000 ms
- Connection Event Length: Min/Max = 750 µs

### USB Configuration

- **Stack**: USB NEXT (CONFIG_USB_DEVICE_STACK_NEXT)
- **Class**: HID Mouse
- **Interface Protocol**: None (Report Protocol only, no Boot Protocol)
- **Vendor**: Nordic Semiconductor
- **Product**: SCI HID Mouse
- **PID**: 0xBEEF

### HID Report Descriptor

3-byte mouse report matching peripheral:
- Byte 0: Button bits [3 buttons + 5-bit padding]
- Byte 1: X movement [-127 to 127]
- Byte 2: Y movement [-127 to 127]

### Message Queue Architecture

```
BLE Thread (HOGP Notify)  →  USB HID Message Queue  →  Main Thread (USB Submit)
                              ↓
                      4 slots × 3 bytes
                      Non-blocking enqueue
                      50ms poll timeout
```

## Configuration Options

### Key Kconfig Symbols

```
CONFIG_BT_CENTRAL=y
CONFIG_BT_HOGP=y
CONFIG_BT_HOGP_REPORTS_MAX=2
CONFIG_BT_SCAN=y
CONFIG_USB_DEVICE_STACK_NEXT=y
CONFIG_USBD_HID_SUPPORT=y
CONFIG_BT_SHORTER_CONNECTION_INTERVALS=y
CONFIG_BT_CTLR_SDC_MAX_CONN_EVENT_LEN_DEFAULT=750
```

### Log Suppression (Reduced Verbosity)

```
CONFIG_UDC_DRIVER_LOG_LEVEL_WRN=y
CONFIG_USBD_LOG_LEVEL_WRN=y
CONFIG_USBD_HID_LOG_LEVEL_WRN=y
```

## Logging

Key log messages:
```
I: Starting SCI HID Central
I: USB HID device enabled
I: Bluetooth initialized
I: Local min interval: <value> us
I: Scanning started
I: Filter matched: <address>
I: Connected: <address>
I: HID service discovered
I: SCI min interval handle: 0x<handle>
I: PHY updated: TX 2 RX 2
I: Frame space: <value> us
I: Connection rate: <value> us    ← Final negotiated interval
I: HOGP ready — subscribing to input reports
I: Subscribed to report id=<id>
I: USB HID interface ready
```

## Troubleshooting

### USB Not Recognized

- Check USB cable connection
- Verify `CONFIG_USB_DEVICE_STACK_NEXT=y`
- Look for "USB HID device enabled" in logs
- Check PC Device Manager / lsusb output

### BLE Scan Fails to Find Peripheral

- Ensure peripheral is advertising
- Verify peripheral advertises HID UUID (0x1812) in primary AD
- Check "Filter matched" does not appear → peripheral UUID mismatch
- Try active scan if peripheral uses scan response for UUID

### Connection Interval Not Optimized

**Symptom**: "Connection rate: 10000 us" (stays at 10 ms)

**Causes**:
- Peripheral doesn't support SCI
- SCI service not found on peripheral
- Remote min interval > 750 µs

**Check logs for**:
```
I: Remote interval: <value> us
W: Target 750 us > remote min <value> us
```

### No Mouse Movement on PC

- Verify USB HID interface is ready
- Check "USB HID interface ready" appears in log
- Ensure peripheral is sending data (press Button 0 on sci_hid_peripheral)
- Monitor "HID notify id=<id> size=<size>" messages
- Check USB message queue not full

### High Latency / Jitter

- Verify connection interval reaches 750 µs
- Ensure PHY is 2M (not 1M)
- Check frame space is minimized
- Confirm peripheral report rate matches interval

## System Architecture

```
┌───────────────────┐
│  BLE Peripheral   │
│  (sci_hid_periph) │
│  Sends HID @750µs │
└─────────┬─────────┘
          │ BLE HID (HOGP)
          │ 750 µs interval
          ↓
┌─────────────────────────┐
│   nRF54LM20 Central     │
│ ┌─────────────────────┐ │
│ │  BLE Stack (HOGP)   │ │
│ │  Notify Callback    │ │
│ └──────────┬──────────┘ │
│            │ k_msgq     │
│ ┌──────────↓──────────┐ │
│ │   USB HID Stack     │ │
│ │   Submit Reports    │ │
│ └─────────────────────┘ │
└───────────┬─────────────┘
            │ USB HS
            ↓
    ┌───────────────┐
    │      PC       │
    │ USB HID Mouse │
    └───────────────┘
```

## Performance Metrics

- **BLE Connection Interval**: 750 µs (SCI)
- **Theoretical Max Throughput**: 1333 reports/second
- **USB Polling Rate**: Determined by host (typically 1000 Hz for HS)
- **End-to-End Latency**: < 2 ms (BLE notify → USB report available)

## References

- [HID over GATT Profile Specification](https://www.bluetooth.com/specifications/specs/hid-over-gatt-profile-1-0/)
- [nRF Connect SDK - BLE HOGP](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/libraries/bluetooth_services/services/hogp.html)
- [nRF Connect SDK - USB NEXT](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/zephyr/connectivity/usb/device_next/index.html)
- [SoftDevice Controller - SCI](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrfxlib/softdevice_controller/doc/scheduling.html#shorter-connection-intervals)
