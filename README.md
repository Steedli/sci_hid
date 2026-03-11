# SCI HID Demo

A Bluetooth HID (Human Interface Device) demonstration project based on Nordic nRF54LM20 development boards, showcasing ultra-low latency BLE HID communication using **SCI (Shorter Connection Intervals)** technology.

## Overview

This project consists of two applications demonstrating a complete BLE HID communication chain:

### 1. sci_hid_peripheral
- Implements a Bluetooth HID mouse peripheral
- Supports SCI connection interval negotiation (as low as 750 µs)
- Button-controlled automatic circular mouse movement generation
- High report rate: 1333 reports/second

### 2. sci_hid_central
- Implements a Bluetooth HID central device
- Automatically scans and connects to HID peripherals
- Forwards Bluetooth mouse data to PC via USB HID
- Zero-copy data forwarding for minimal latency

## Key Features

- **Ultra-Low Latency**: Achieves 750 µs connection intervals via SCI technology
- **HOGP Support**: Full implementation of HID-over-GATT Profile
- **Automatic Connection**: Central device auto-discovers and connects to peripherals
- **PHY Optimization**: Uses 2M PHY for improved data rates
- **USB Bridge**: Seamlessly forwards BLE HID data to PC

## Hardware Requirements

- **Development Boards**: 2× nRF54LM20 DK (or nRF54H20 DK)
- **SDK**: nRF Connect SDK v3.2.3 or later
- **USB Cable**: For powering and communicating with central device

## Quick Start

### Building and Flashing

**Peripheral:**
```bash
cd sci_hid_peripheral
west build -b nrf54lm20dk/nrf54lm20a/cpuapp
west flash
```

**Central:**
```bash
cd sci_hid_central
west build -b nrf54lm20dk/nrf54lm20a/cpuapp
west flash
```

### Operation Steps

1. Power on the peripheral board - device starts advertising ("Nordic_SCI_HID")
2. Connect the central board to PC via USB
3. PC recognizes a new HID mouse device
4. Central automatically scans and connects to peripheral
5. Press Button 0 on peripheral to start circular mouse movement
6. Observe the mouse cursor moving in a circular pattern on PC

## Technical Highlights

- **SCI Negotiation**: Automatic negotiation of optimal connection parameters
- **Real-time Performance**: End-to-end latency from peripheral to PC under 1ms
- **Standards Compliant**: Fully compliant with Bluetooth SIG HID specifications

## Use Cases

- Low-latency gaming mice/keyboards
- Industrial control HID devices
- VR/AR controllers
- High-precision input devices

## License

This project is based on Nordic Semiconductor example code.

## More Information

For detailed technical documentation, refer to the README.md files in each subdirectory:
- [sci_hid_peripheral](./sci_hid_peripheral/README.md)
- [sci_hid_central](./sci_hid_central/README.md)
