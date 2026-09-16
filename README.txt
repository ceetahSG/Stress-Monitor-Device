STRESS MONITOR - CORRECTED PROJECT

Folder structure:

stress_monitor_stress_fixed_2s_corrected/
|
+-- app.py
+-- requirements.txt
+-- run_app.bat
+-- bpm_realtime_auto_end_2s_stress_fixed.ino
|
+-- templates/
    +-- dashboard.html
    +-- live_dashboard.html

IMPORTANT:
Flask requires dashboard.html and live_dashboard.html to be inside
the templates folder. This fixes:
jinja2.exceptions.TemplateNotFound: dashboard.html

RUN:
1. Open Command Prompt.
2. cd into this folder.
3. Run:
       python -m pip install -r requirements.txt
       python app.py
4. Open:
       http://127.0.0.1:5000

You can also double-click run_app.bat.

The simple-websocket package is included to remove the WebSocket
transport warning when supported by the installed Flask-SocketIO stack.

ESP32:
Use bpm_realtime_auto_end_2s_stress_fixed.ino for the corrected
real-time firmware with the 2-second finger-removal session end
and updated stress-score mapping.
