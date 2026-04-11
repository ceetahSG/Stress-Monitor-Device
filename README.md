# Stress Monitor Device

A real-time stress monitoring system using ESP32, MAX30105 sensor, and a Flask web dashboard.

## Overview

This project measures heart rate, blood oxygen levels, and stress indicators using a wearable pulse oximeter sensor. Data is sent to a Flask server for real-time monitoring and historical analysis.

## Features

- Real-time heart rate (BPM) measurement
- Blood oxygen (SpO2) monitoring
- Heart rate variability (HRV) stress calculation
- Live web dashboard with WebSocket updates
- Historical data storage and graphs
- WiFi connectivity
- OLED display on device
- Vibration alerts for high stress

## Hardware

- ESP32 Development Board
- MAX30105 Pulse Oximeter Sensor
- 128x64 OLED Display
- Vibration Motor (GPIO 4)
- USB Cable for power

## Software Requirements

```bash
pip install Flask==2.3.3
pip install Flask-SQLAlchemy==3.0.5
pip install Flask-SocketIO==5.3.4
pip install python-socketio==5.9.0
pip install python-engineio==4.7.0