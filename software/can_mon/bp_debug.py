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
        # Data storage and Multicast CAN
        self.devices = {} # device_id -> device_data
        self.can_handlers = [] # list of functions(msg)
        self.can_handlers.append(self.dispatch_to_devices)
        
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

        # Device Status Container
        self.devices_frame = ttk.Frame(self.root)
        self.devices_frame.pack(fill="both", expand=True, padx=10, pady=5)
        
        # We will dynamically create frames inside devices_frame

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

        # Use a queue to capture messages for this update session
        response_queue = []
        def update_handler(msg):
            response_queue.append(msg)
        
        self.can_handlers.append(update_handler)

        logger.info(f"Bootloader update targeting Device ID: {dev_id}")
        self.updating = True
        try:
            with open(self.bin_path.get(), "rb") as f:
                content = f.read()

            total_size = len(content)
            logger.debug(f"Read binary file: {total_size} bytes")
            self.update_status.set("Starting Session...")
            self.progress["value"] = 0
            
            # Flush queue
            response_queue.clear()

            # Constant waiting logic
            def wait_for_msg(timeout=2.0):
                start = time.time()
                while time.time() - start < timeout:
                    if response_queue:
                        return response_queue.pop(0)
                    time.sleep(0.01)
                return None

            # CMD_START
            logger.info(f"Sending CMD_START to ID 0x{CTRL_ID:08X}")
            self.bus.send(can.Message(arbitration_id=CTRL_ID, data=[0x01], is_extended_id=True))
            
            # Wait for ACK (0xAA 0x00)
            ack_received = False
            start_time = time.time()
            while time.time() - start_time < 2.0:
                msg = wait_for_msg(0.1)
                if msg:
                    logger.debug(f"Received during START wait: ID=0x{msg.arbitration_id:08X} Data={msg.data.hex()}")
                    if msg.arbitration_id == CTRL_ID and len(msg.data) >= 2:
                        if msg.data[0] == 0xAA and msg.data[1] == 0x00:
                            logger.info("Received ACK for START")
                            ack_received = True
                            break
            
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
                msg = wait_for_msg(0.1)
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

            if not success and "Error" not in self.update_status.get():
                logger.error("Commit command timed out")
                self.update_status.set("Error: Commit Timeout")

        except Exception as e:
            logger.exception("Unexpected error during bootloader update")
            self.update_status.set(f"Error: {str(e)}")
        
        self.updating = False
        self.can_handlers.remove(update_handler)
        self.btn_update.config(state="normal")

    def receive_can(self):
        while self.running:
            try:
                msg = self.bus.recv(0.1)
                if msg:
                    # Pump to all interested handlers
                    for handler in list(self.can_handlers): # list() to prevent mutation issues
                        try:
                            handler(msg)
                        except Exception as e:
                            logger.error(f"Handler error: {e}")
            except Exception as e:
                logger.error(f"Receive error: {e}")
        logger.info("Receive thread exiting.")

    def dispatch_to_devices(self, msg):
        """Dispatcher that breaks down the WPILib Frame and routes to device objects."""
        # WPILib Frame: 
        # Bits 24-28: Device Type (10 for us)
        # Bits 16-23: Mfg (42 for us)
        # Bits 10-15: API Class
        # Bits 6-9: API Index
        # Bits 0-5: Device ID
        
        # Simple check for our Mfg/Type
        if (msg.arbitration_id & 0x1FFF0000) != 0x0A2A0000:
            return
           
        dev_id = msg.arbitration_id & 0x3F
        api_id = (msg.arbitration_id >> 6) & 0x3FF # Class + Index
        
        if dev_id not in self.devices:
            if len(self.devices) >= 10: return # Cap at 10 devices
            logger.info(f"New device discovered: {dev_id}")
            self.create_device(dev_id)
            
        device = self.devices[dev_id]
        device["last_seen"] = time.time()

        logger.info(f"msg: {msg} api_id: {api_id}")

        # Route based on API
        if api_id == 0x50: # Class 5, Index 0 (SW Version)
            uhash = struct.unpack("<I", msg.data[0:4])[0]
            mode_byte = msg.data[4]
            mode_str = "App" if (mode_byte & 0x01) else "Boot"
            major = (mode_byte >> 1) & 0x07
            minor = (mode_byte >> 4) & 0x0F
            build = struct.unpack("<I", msg.data[4:8])[0] >> 8
            device["data"]["Version"] = {"Hash": uhash, "Type": mode_str, "Major": major, "Minor": minor, "Build": build}

        elif api_id == 0x51: # Class 5, Index 1 (General)
            # Match application's pack format [I B H b] if hash is included, or just the values
            # Based on previous logic assuming [I B H b]:
            uid_hash, current, voltage, temp = struct.unpack("<IBHb", msg.data)
            device["data"]["General"] = {"Current": current, "Voltage": voltage, "Temp": temp}

        elif api_id == 0x52: # Class 5, Index 2 (TOF)
            status = msg.data[0]
            dist, amb, sig = struct.unpack("<HHH", msg.data[2:8])
            device["data"]["TOF"] = {"Status": status, "Distance": dist, "Ambient": amb, "Signal": sig}

        elif api_id == 0x53: # Class 5, Index 3 (Encoder)
            e1a, e1i, e2a, e2i = struct.unpack("<HhHh", msg.data)
            device["data"]["Encoder"] = {"Enc1_Abs": e1a/100, "Enc1_Inc": e1i, "Enc2_Abs": e2a/100, "Enc2_Inc": e2i/100}

    def create_device(self, dev_id):
        # UI Frame for this device
        frame = ttk.LabelFrame(self.devices_frame, text=f" Device ID: {dev_id} ", padding=5)
        frame.pack(fill="x", pady=2)
        
        # Create labels and store them
        labels = {}
        row1 = ttk.Frame(frame)
        row1.pack(fill="x")
        labels["ver"] = ttk.Label(row1, text="Ver: -.- (Boot) Build: -", style="Data.TLabel")
        labels["ver"].pack(side="left", padx=5)
        labels["gen"] = ttk.Label(row1, text="Current: - mA | Volt: - mV", style="Data.TLabel")
        labels["gen"].pack(side="left", padx=20)
        
        row2 = ttk.Frame(frame)
        row2.pack(fill="x")
        labels["tof"] = ttk.Label(row2, text="Dist: - mm | Status: -", style="Data.TLabel")
        labels["tof"].pack(side="left", padx=5)
        labels["enc"] = ttk.Label(row2, text="E1: - deg | E2: - deg", style="Data.TLabel")
        labels["enc"].pack(side="left", padx=20)

        self.devices[dev_id] = {
            "last_seen": time.time(),
            "frame": frame,
            "labels": labels,
            "data": {
                "Version": {"Type": "-", "Major": 0, "Minor": 0, "Build": 0},
                "General": {"Current": 0, "Voltage": 0},
                "TOF": {"Distance": 0, "Status": 0},
                "Encoder": {"Enc1_Abs": 0, "Enc2_Abs": 0}
            }
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
        now = time.time()
        to_delete = []

        for dev_id, dev in self.devices.items():
            # Check for timeout
            if now - dev["last_seen"] > 1.0:
                to_delete.append(dev_id)
                continue
                
            # Update labels
            d = dev["data"]
            l = dev["labels"]
            
            v = d["Version"]
            l["ver"].config(text=f"Ver: {v['Major']}.{v['Minor']} ({v['Type']}) Build: {v['Build']}")
            
            g = d["General"]
            l["gen"].config(text=f"Current: {g['Current']} mA | Volt: {g['Voltage']} mV")
            
            t = d["TOF"]
            l["tof"].config(text=f"Dist: {t['Distance']} mm | Status: {t['Status']}")
            
            e = d["Encoder"]
            l["enc"].config(text=f"E1: {e['Enc1_Abs']:.2f}° | E2: {e['Enc2_Abs']:.2f}°")

        for dev_id in to_delete:
            logger.info(f"Removing timed out device: {dev_id}")
            self.devices[dev_id]["frame"].destroy()
            del self.devices[dev_id]

        self.root.after(100, self.update_ui)

if __name__ == "__main__":
    root = tk.Tk()
    app = CanMonitorApp(root)
    root.mainloop()
