# SCI HID Peripheral

## Overview

This sample demonstrates a Bluetooth LE HID peripheral that implements a mouse device with Shorter Connection Intervals (SCI) support. The device advertises as a HID mouse and automatically generates circular mouse movement patterns when activated via a button press.

## Features

- **BLE HID Mouse Service**: Implements standard HID-over-GATT profile
- **SCI Support**: Negotiates connection intervals as low as 750 µs for ultra-low latency
- **Button-Controlled Transmission**: Toggle mouse report transmission with Button 0
- **High Report Rate**: Sends HID reports every 750 µs (1333 reports/second)
- **Circular Movement Pattern**: Generates a 16-step circular mouse movement

## Requirements

- **Board**: nRF54LM20 DK (nrf54lm20dk/nrf54lm20a/cpuapp)
- **SDK**: nRF Connect SDK v3.2.3 or later
- **Hardware**: Button 0 (sw0) for controlling transmission

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

1. **Power On**: Device starts advertising as "Nordic_SCI_HID" with HID mouse appearance
2. **Connection**: Wait for a central device (e.g., sci_hid_central) to connect
3. **Activation**: Press Button 0 to start sending mouse circle data
   - Log message: `Circle started`
4. **Deactivation**: Press Button 0 again to stop transmission
   - Log message: `Circle stopped`

## Technical Details

### Advertising Data

- **Primary AD**:
  - GAP Appearance: Mouse (0x03C2)
  - Flags: General Discoverable, BR/EDR Not Supported
  - UUID: HID Service (0x1812)
- **Scan Response**:
  - Complete Name: "Nordic_SCI_HID"

### HID Report Descriptor

3-byte mouse report (no report ID):
- Byte 0: Button bits [3 buttons + 5-bit padding]
- Byte 1: X movement [-127 to 127]
- Byte 2: Y movement [-127 to 127]

### SCI Configuration

- **Target Interval**: 750 µs
- **Controller Max Event Length**: 750 µs
- **Report Transmission Rate**: 750 µs period (1333 Hz)
- **PHY**: 2M (negotiated by central)
- **Frame Space**: Minimum (negotiated by central)

### Circular Movement Pattern

16-step table generating a smooth circular pattern:
```
Radius: ~10 pixels (in desktop coordinate space)
Direction: Clockwise
Step interval: 750 µs
```

## Configuration Options

### Key Kconfig Symbols

```
CONFIG_BT_DEVICE_NAME="Nordic_SCI_HID"
CONFIG_BT_DEVICE_APPEARANCE=962
CONFIG_BT_PERIPHERAL=y
CONFIG_BT_HIDS=y
CONFIG_BT_SHORTER_CONNECTION_INTERVALS=y
CONFIG_BT_CTLR_SDC_MAX_CONN_EVENT_LEN_DEFAULT=750
CONFIG_GPIO=y
```

## Logging

Key log messages:
```
I: Starting SCI HID Peripheral
I: Local minimum connection interval: 750 us
I: Advertising started
I: Connected: <address>
I: HID notification enabled
I: Circle started/stopped
I: PHY updated: TX PHY 2, RX PHY 2
I: Frame space updated: <value> us
I: Connection rate changed: interval <value> us
```

## Troubleshooting

### Device Not Discovered

- Check advertising is active
- Verify central is scanning for HIDS UUID (0x1812)
- Ensure device name appears in scan results

### No Mouse Movement

- Press Button 0 to activate transmission
- Verify "Circle started" log message
- Check HID notification is enabled
- Ensure connection is established

### Poor Latency

- Wait for SCI negotiation to complete
- Check "Connection rate changed: interval 750 us" log
- Verify PHY is set to 2M
- Ensure frame space is minimized

## References

- [Bluetooth HID over GATT Profile](https://www.bluetooth.com/specifications/specs/hid-over-gatt-profile-1-0/)
- [nRF Connect SDK Documentation](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/)
- [SoftDevice Controller - Shorter Connection Intervals](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrfxlib/softdevice_controller/doc/scheduling.html#shorter-connection-intervals)
