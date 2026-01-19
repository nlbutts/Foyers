import can
import struct
import threading
import tkinter as tk
from tkinter import ttk

class CanMonitorApp:
    def __init__(self, root):
        self.root = root
        self.root.title("BackPorch CAN Monitor")
        self.root.geometry("600x500")
        
        # Data storage
        self.data = {
            "General": {"ID": 0, "Current": 0, "Voltage": 0, "Temp": 0},
            "TOF": {"Status": 0, "Distance": 0, "Ambient": 0, "Signal": 0},
            "Encoder": {"Enc1_Abs": 0, "Enc1_Inc": 0, "Enc2_Abs": 0, "Enc2_Inc": 0}
        }

        self.setup_ui()
        
        # CAN setup
        try:
            self.bus = can.interface.Bus(bustype='slcan', channel='COM5', bitrate=1000000)
            self.running = True
            self.thread = threading.Thread(target=self.receive_can, daemon=True)
            self.thread.start()
        except Exception as e:
            print(f"Error opening CAN bus: {e}")
            self.running = False

        self.update_ui()

    def setup_ui(self):
        style = ttk.Style()
        style.configure("Header.TLabel", font=("Arial", 12, "bold"))
        style.configure("Data.TLabel", font=("Consolas", 10))

        # General Status
        frame_gen = ttk.LabelFrame(self.root, text=" General Status (0xA2A1400) ", padding=10)
        frame_gen.pack(fill="x", padx=10, pady=5)
        self.lbl_id = ttk.Label(frame_gen, text="Unique ID: -", style="Data.TLabel")
        self.lbl_id.pack(side="left", padx=10)
        self.lbl_current = ttk.Label(frame_gen, text="Current: - mA", style="Data.TLabel")
        self.lbl_current.pack(side="left", padx=10)
        self.lbl_voltage = ttk.Label(frame_gen, text="Voltage: - mV", style="Data.TLabel")
        self.lbl_voltage.pack(side="left", padx=10)
        self.lbl_temp = ttk.Label(frame_gen, text="Temp: - °C", style="Data.TLabel")
        self.lbl_temp.pack(side="left", padx=10)

        # TOF Status
        frame_tof = ttk.LabelFrame(self.root, text=" TOF Status (0xA2A1440) ", padding=10)
        frame_tof.pack(fill="x", padx=10, pady=5)
        self.lbl_tof_dist = ttk.Label(frame_tof, text="Distance: - mm", style="Data.TLabel")
        self.lbl_tof_dist.pack(side="left", padx=10)
        self.lbl_tof_status = ttk.Label(frame_tof, text="Status: -", style="Data.TLabel")
        self.lbl_tof_status.pack(side="left", padx=10)
        self.lbl_tof_ambient = ttk.Label(frame_tof, text="Ambient: - Mcps", style="Data.TLabel")
        self.lbl_tof_ambient.pack(side="left", padx=10)

        # Encoder Status
        frame_enc = ttk.LabelFrame(self.root, text=" Encoder Status (0xA2A1480) ", padding=10)
        frame_enc.pack(fill="x", padx=10, pady=5)
        
        self.lbl_enc1_abs = ttk.Label(frame_enc, text="Enc1 Abs: -°", style="Data.TLabel")
        self.lbl_enc1_abs.grid(row=0, column=0, padx=10, sticky="w")
        self.lbl_enc1_inc = ttk.Label(frame_enc, text="Enc1 Inc: - counts", style="Data.TLabel")
        self.lbl_enc1_inc.grid(row=1, column=0, padx=10, sticky="w")
        
        self.lbl_enc2_abs = ttk.Label(frame_enc, text="Enc2 Abs: -°", style="Data.TLabel")
        self.lbl_enc2_abs.grid(row=0, column=1, padx=10, sticky="w")
        self.lbl_enc2_inc = ttk.Label(frame_enc, text="Enc2 Inc: -°", style="Data.TLabel")
        self.lbl_enc2_inc.grid(row=1, column=1, padx=10, sticky="w")

    def receive_can(self):
        while self.running:
            try:
                msg = self.bus.recv(0.1)
                if msg:
                    if msg.arbitration_id == 0xA2A1400:
                        # Byte 0-3: ID, Byte 4: Current, Byte 5-6: Voltage, Byte 7: Temp
                        uid, current, voltage, temp = struct.unpack("<IBHb", msg.data)
                        self.data["General"] = {"ID": uid, "Current": current, "Voltage": voltage, "Temp": temp}
                    
                    elif msg.arbitration_id == 0xA2A1440:
                        # Byte 0: Status, Byte 2-3: Distance, Byte 4-5: Ambient, Byte 6-7: Signal
                        status = msg.data[0]
                        dist, amb, sig = struct.unpack("<HHH", msg.data[2:8])
                        self.data["TOF"] = {"Status": status, "Distance": dist, "Ambient": amb, "Signal": sig}

                    elif msg.arbitration_id == 0xA2A1480:
                        # 4x uint16: Abs1, Inc1, Abs2, Inc2 (0.01 deg units)
                        e1a, e1i, e2a, e2i = struct.unpack("<HhHh", msg.data)
                        self.data["Encoder"] = {
                            "Enc1_Abs": e1a / 100.0, "Enc1_Inc": e1i,
                            "Enc2_Abs": e2a / 100.0, "Enc2_Inc": e2i / 100.0
                        }
            except Exception as e:
                print(f"Receive error: {e}")

    def update_ui(self):
        # Update General
        g = self.data["General"]
        self.lbl_id.config(text=f"Unique ID: {g['ID']:08X}")
        self.lbl_current.config(text=f"Current: {g['Current']} mA")
        self.lbl_voltage.config(text=f"Voltage: {g['Voltage']} mV")
        self.lbl_temp.config(text=f"Temp: {g['Temp']} °C")

        # Update TOF
        t = self.data["TOF"]
        self.lbl_tof_dist.config(text=f"Distance: {t['Distance']} mm")
        self.lbl_tof_status.config(text=f"Status: {t['Status']}")
        self.lbl_tof_ambient.config(text=f"Ambient: {t['Ambient']} Mcps")

        # Update Encoders
        e = self.data["Encoder"]
        self.lbl_enc1_abs.config(text=f"Enc1 Abs: {e['Enc1_Abs']:.2f}°")
        self.lbl_enc1_inc.config(text=f"Enc1 Inc: {e['Enc1_Inc']}")
        self.lbl_enc2_abs.config(text=f"Enc2 Abs: {e['Enc2_Abs']:.2f}°")
        self.lbl_enc2_inc.config(text=f"Enc2 Inc: {e['Enc2_Inc']:.2f}°")

        self.root.after(100, self.update_ui)

if __name__ == "__main__":
    root = tk.Tk()
    app = CanMonitorApp(root)
    root.mainloop()
