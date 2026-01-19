import can
import struct
import threading
import time
import tkinter as tk
import zlib
import logging
from tkinter import ttk, filedialog

# Configure logging
logging.basicConfig(
    level=logging.DEBUG,
    format='%(asctime)s [%(levelname)s] %(threadName)s: %(message)s',
    handlers=[
        logging.FileHandler("can_monitor.log"),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

class CanMonitorApp:
    def __init__(self, root):
        self.root = root
        self.root.title("BackPorch CAN Monitor & Bootloader")
        self.root.geometry("600x750")
        
        logger.info("Initializing CanMonitorApp")
        # Data storage
        self.data = {
            "Version": {"Hash": 0, "Type": "Unknown", "Major": 0, "Minor": 0, "Build": 0},
            "General": {"Current": 0, "Voltage": 0, "Temp": 0},
            "TOF": {"Status": 0, "Distance": 0, "Ambient": 0, "Signal": 0},
            "Encoder": {"Enc1_Abs": 0, "Enc1_Inc": 0, "Enc2_Abs": 0, "Enc2_Inc": 0}
        }
        
        self.bin_path = tk.StringVar(value="")
        self.update_status = tk.StringVar(value="Idle")
        self.device_id = tk.IntVar(value=0)
        self.updating = False

        self.setup_ui()
        
        # CAN setup
        try:
            logger.info("Opening CAN bus on COM5 at 1Mbps")
            self.bus = can.interface.Bus(bustype='slcan', channel='COM5', bitrate=1000000)
            self.running = True
            
            self.receive_thread = threading.Thread(target=self.receive_can, name="ReceiveThread", daemon=True)
            self.receive_thread.start()
            
            self.heartbeat_thread = threading.Thread(target=self.send_heartbeat, name="HeartbeatThread", daemon=True)
            self.heartbeat_thread.start()
        except Exception as e:
            logger.error(f"Error opening CAN bus: {e}")
            self.running = False

        self.update_ui()

    def setup_ui(self):
        style = ttk.Style()
        style.configure("Header.TLabel", font=("Arial", 12, "bold"))
        style.configure("Data.TLabel", font=("Consolas", 10))

        # Bootloader Section
        frame_boot = ttk.LabelFrame(self.root, text=" Bootloader ", padding=10)
        frame_boot.pack(fill="x", padx=10, pady=5)

        file_row = ttk.Frame(frame_boot)
        file_row.pack(fill="x", pady=2)
        ttk.Label(file_row, text="File:").pack(side="left")
        ttk.Entry(file_row, textvariable=self.bin_path).pack(side="left", fill="x", expand=True, padx=5)
        ttk.Button(file_row, text="Browse", command=self.browse_file).pack(side="left")

        ctrl_row = ttk.Frame(frame_boot)
        ctrl_row.pack(fill="x", pady=2)
        ttk.Label(ctrl_row, text="Device ID:").pack(side="left")
        ttk.Spinbox(ctrl_row, from_=0, to=63, textvariable=self.device_id, width=5).pack(side="left", padx=5)
        self.btn_update = ttk.Button(ctrl_row, text="Start Update", command=self.start_update_thread)
        self.btn_update.pack(side="left", padx=10)

        self.progress = ttk.Progressbar(frame_boot, orient="horizontal", length=100, mode="determinate")
        self.progress.pack(fill="x", pady=5)
        
        ttk.Label(frame_boot, textvariable=self.update_status, foreground="blue").pack()

        # Software Version
        frame_ver = ttk.LabelFrame(self.root, text=" Software Version (0xA2A1400) ", padding=10)
        frame_ver.pack(fill="x", padx=10, pady=5)
        self.lbl_ver_hash = ttk.Label(frame_ver, text="UID Hash: -", style="Data.TLabel")
        self.lbl_ver_hash.pack(side="left", padx=10)
        self.lbl_ver_type = ttk.Label(frame_ver, text="Type: -", style="Data.TLabel")
        self.lbl_ver_type.pack(side="left", padx=10)
        self.lbl_ver_num = ttk.Label(frame_ver, text="Ver: -.-", style="Data.TLabel")
        self.lbl_ver_num.pack(side="left", padx=10)
        self.lbl_ver_build = ttk.Label(frame_ver, text="Build: -", style="Data.TLabel")
        self.lbl_ver_build.pack(side="left", padx=10)

        # General Status
        frame_gen = ttk.LabelFrame(self.root, text=" General Status (0xA2A1440) ", padding=10)
        frame_gen.pack(fill="x", padx=10, pady=5)
        self.lbl_current = ttk.Label(frame_gen, text="Current: - mA", style="Data.TLabel")
        self.lbl_current.pack(side="left", padx=10)
        self.lbl_voltage = ttk.Label(frame_gen, text="Voltage: - mV", style="Data.TLabel")
        self.lbl_voltage.pack(side="left", padx=10)
        self.lbl_temp = ttk.Label(frame_gen, text="Temp: - °C", style="Data.TLabel")
        self.lbl_temp.pack(side="left", padx=10)

        # TOF Status
        frame_tof = ttk.LabelFrame(self.root, text=" TOF Status (0xA2A1480) ", padding=10)
        frame_tof.pack(fill="x", padx=10, pady=5)
        self.lbl_tof_dist = ttk.Label(frame_tof, text="Distance: - mm", style="Data.TLabel")
        self.lbl_tof_dist.pack(side="left", padx=10)
        self.lbl_tof_status = ttk.Label(frame_tof, text="Status: -", style="Data.TLabel")
        self.lbl_tof_status.pack(side="left", padx=10)
        self.lbl_tof_ambient = ttk.Label(frame_tof, text="Ambient: - Mcps", style="Data.TLabel")
        self.lbl_tof_ambient.pack(side="left", padx=10)

        # Encoder Status
        frame_enc = ttk.LabelFrame(self.root, text=" Encoder Status (0xA2A14C0) ", padding=10)
        frame_enc.pack(fill="x", padx=10, pady=5)
        
        self.lbl_enc1_abs = ttk.Label(frame_enc, text="Enc1 Abs: -°", style="Data.TLabel")
        self.lbl_enc1_abs.grid(row=0, column=0, padx=10, sticky="w")
        self.lbl_enc1_inc = ttk.Label(frame_enc, text="Enc1 Inc: - counts", style="Data.TLabel")
        self.lbl_enc1_inc.grid(row=1, column=0, padx=10, sticky="w")
        
        self.lbl_enc2_abs = ttk.Label(frame_enc, text="Enc2 Abs: -°", style="Data.TLabel")
        self.lbl_enc2_abs.grid(row=0, column=1, padx=10, sticky="w")
        self.lbl_enc2_inc = ttk.Label(frame_enc, text="Enc2 Inc: -°", style="Data.TLabel")
        self.lbl_enc2_inc.grid(row=1, column=1, padx=10, sticky="w")

    def browse_file(self):
        path = filedialog.askopenfilename(filetypes=[("Binary files", "*.bin"), ("All files", "*.*")])
        if path:
            self.bin_path.set(path)

    def start_update_thread(self):
        if not self.bin_path.get():
            logger.warning("Update started without file selected")
            self.update_status.set("Error: No file selected")
            return
        
        logger.info(f"Starting bootloader update thread with file: {self.bin_path.get()}")
        self.btn_update.config(state="disabled")
        threading.Thread(target=self.run_bootloader_update, name="BootloaderThread", daemon=True).start()

    def run_bootloader_update(self):
        dev_id = self.device_id.get() & 0x3F
        CTRL_ID = 0x0A2A0400 | dev_id
        DATA_ID = 0x0A2A0800 | dev_id

        logger.info(f"Bootloader update targeting Device ID: {dev_id}")
        self.running = False
        time.sleep(0.2)  # Allow receive thread to pause

        self.updating = True
        try:
            with open(self.bin_path.get(), "rb") as f:
                content = f.read()

            total_size = len(content)
            logger.debug(f"Read binary file: {total_size} bytes")
            self.update_status.set("Starting Session...")
            self.progress["value"] = 0
            
            # Flush any old messages
            logger.debug("Flushing CAN receive queue")
            while self.bus.recv(0.01): pass

            # CMD_START
            logger.info(f"Sending CMD_START to ID 0x{CTRL_ID:08X}")
            self.bus.send(can.Message(arbitration_id=CTRL_ID, data=[0x01], is_extended_id=True))
            
            # Wait for ACK (0xAA 0x00)
            ack_received = False
            start_time = time.time()
            while time.time() - start_time < 2.0:
                msg = self.bus.recv(0.1)
                if msg:
                    logger.debug(f"Received during START wait: ID=0x{msg.arbitration_id:08X} Data={msg.data.hex()}")
                    if msg.arbitration_id == CTRL_ID and len(msg.data) >= 2:
                        if msg.data[0] == 0xAA and msg.data[1] == 0x00:
                            logger.info("Received ACK for START")
                            ack_received = True
                            break
                    else:
                        self.process_msg(msg)
            
            if not ack_received:
                logger.error("Failed to receive ACK for START command")
                self.update_status.set("Error: No ACK from device")
                self.btn_update.config(state="normal")
                self.updating = False
                return

            logger.info("Beginning data streaming")
            self.update_status.set("Sending Data...")
            sent = 0
            
            for i in range(0, total_size, 8):
                chunk = content[i:i+8]
                self.bus.send(can.Message(arbitration_id=DATA_ID, data=list(chunk), is_extended_id=True))
                sent += len(chunk)
                self.progress["value"] = (sent / total_size) * 100
                if i % 512 == 0:
                    logger.debug(f"Sent {sent}/{total_size} bytes")
                    self.update_status.set(f"Sending: {sent}/{total_size} bytes")
                    msg = self.bus.recv(0.001)
                    if msg: self.process_msg(msg)
                time.sleep(0.0005)

            logger.info("All data sent. Sending COMMIT command.")
            self.update_status.set("Committing...")
            crc = self.calculate_stm32_crc(content)
            logger.debug(f"Calculated CRC32: 0x{crc:08X}")
            commit_data = [0x02] + list(struct.pack("<I", crc))
            self.bus.send(can.Message(arbitration_id=CTRL_ID, data=commit_data, is_extended_id=True))

            success = False
            start_time = time.time()
            while time.time() - start_time < 5.0:
                msg = self.bus.recv(0.1)
                if msg:
                    logger.debug(f"Received during COMMIT wait: ID=0x{msg.arbitration_id:08X} Data={msg.data.hex()}")
                    if msg.arbitration_id == CTRL_ID and len(msg.data) >= 2:
                        if msg.data[0] == 0xAA:
                            if msg.data[1] == 0x01:
                                logger.info("Update Successful! Device is rebooting.")
                                success = True
                                self.update_status.set("Update Successful! Rebooting...")
                                break
                            elif msg.data[1] == 0xEE:
                                logger.error("Update failed: CRC Mismatch reported by device")
                                self.update_status.set("Error: CRC Mismatch")
                                break
                    else:
                        self.process_msg(msg)

            if not success and "Error" not in self.update_status.get():
                logger.error("Commit command timed out")
                self.update_status.set("Error: Commit Timeout")

        except Exception as e:
            logger.exception("Unexpected error during bootloader update")
            self.update_status.set(f"Error: {str(e)}")
        
        self.updating = False
        self.btn_update.config(state="normal")

    def receive_can(self):
        while self.running:
            try:
                if hasattr(self, 'updating') and self.updating:
                    time.sleep(0.1)
                    continue
                msg = self.bus.recv(0.1)
                if msg:
                    logger.debug(f"RX: ID=0x{msg.arbitration_id:08X} Data={msg.data.hex()}")
                    self.process_msg(msg)
            except Exception as e:
                logger.error(f"Receive error: {e}")
        logger.info("Receive thread exiting.")

    def process_msg(self, msg):
        if msg.arbitration_id == 0xA2A1400:
            # Byte 0-3: Hash, Byte 4: Mode/Ver, Byte 5-7: Build
            uhash = struct.unpack("<I", msg.data[0:4])[0]
            mode_byte = msg.data[4]
            mode_str = "App" if (mode_byte & 0x01) else "Boot"
            major = (mode_byte >> 1) & 0x07
            minor = (mode_byte >> 4) & 0x0F
            build = struct.unpack("<I", msg.data[4:8])[0] >> 8 # 24-bit build number
            self.data["Version"] = {
                "Hash": uhash, "Type": mode_str, 
                "Major": major, "Minor": minor, "Build": build
            }

        elif msg.arbitration_id == 0xA2A1440:
            # Byte 0-3: [Removed ID], Byte 4: Current, Byte 5-6: Voltage, Byte 7: Temp
            # Note: The C code actually changed the format. Let's look at main.c again.
            # Byte 4 is Current (idx 4 if assuming it's same buffer as before, but let's assume it's compact now)
            # Actually looking at main.c:
            # Byte 0-3: Hash, Byte 4: Current, Byte 5-6: Voltage, Byte 7: Temp
            uid, current, voltage, temp = struct.unpack("<IBHb", msg.data)
            self.data["General"] = {"Current": current, "Voltage": voltage, "Temp": temp}
        
        elif msg.arbitration_id == 0xA2A1480:
            # TOF Status
            status = msg.data[0]
            dist, amb, sig = struct.unpack("<HHH", msg.data[2:8])
            self.data["TOF"] = {"Status": status, "Distance": dist, "Ambient": amb, "Signal": sig}
        
        elif msg.arbitration_id == 0xA2A14C0:
            # Encoder Status
            e1a, e1i, e2a, e2i = struct.unpack("<HhHh", msg.data)
            self.data["Encoder"] = {
                "Enc1_Abs": e1a / 100.0, "Enc1_Inc": e1i,
                "Enc2_Abs": e2a / 100.0, "Enc2_Inc": e2i / 100.0
            }

    def calculate_stm32_crc(self, content):
        """
        Calculates CRC32 using the standard STM32 algorithm:
        - Polynomial: 0x04C11DB7
        - Initial Value: 0xFFFFFFFF
        - Input: 32-bit words (Little Endian)
        - No reflection
        - No final XOR (or XOR with 0)
        """
        crc = 0xFFFFFFFF
        # Process in 32-bit chunks, truncating any remainder bytes just like the Bootloader C code
        length_words = len(content) // 4
        
        for i in range(length_words):
            # Extract 4 bytes
            chunk = content[i*4 : (i+1)*4]
            # Convert to 32-bit int (Little Endian)
            val = struct.unpack('<I', chunk)[0]
            
            crc ^= val
            for _ in range(32):
                if crc & 0x80000000:
                    crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
                else:
                    crc = (crc << 1) & 0xFFFFFFFF
        return crc

    def send_heartbeat(self):
        """Sends the WPILib Heartbeat message every 20ms in a dedicated thread."""
        # Arb ID 0x01011840: Robot Controller Heartbeat
        # Data [1, 0, 0, 0, 0, 0, 0, 0]: Enabled, Red 1, Teleop
        msg = can.Message(
            arbitration_id=0x01011840,
            data=[1, 0, 0, 0, 0, 0, 0, 0],
            is_extended_id=True
        )
        
        while self.running:
            try:
                self.bus.send(msg)
            except Exception as e:
                logger.error(f"Send heartbeat error: {e}")
            
            time.sleep(0.02) # 20 ms interval

    def update_ui(self):
        # Update Version
        v = self.data["Version"]
        self.lbl_ver_hash.config(text=f"UID Hash: {v['Hash']:08X}")
        self.lbl_ver_type.config(text=f"Type: {v['Type']}")
        self.lbl_ver_num.config(text=f"Ver: {v['Major']}.{v['Minor']}")
        self.lbl_ver_build.config(text=f"Build: {v['Build']}")

        # Update General
        g = self.data["General"]
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
