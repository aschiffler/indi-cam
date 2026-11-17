"""Print the board's serial log for N seconds: python tools/serial_log.py COM13 20"""
import sys, time
import serial

port, secs = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 15
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dtr = s.rts = False  # do not reset the board when opening the port
end = time.time() + secs
while time.time() < end:
    try:
        if not s.is_open:
            s.open()
        data = s.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
    except serial.SerialException:
        s.close()  # port vanishes briefly while the board reboots
        time.sleep(0.3)
