import can
import struct
import threading
import time
import tkinter as tk
import zlib
import logging
import socket
from collections import deque
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

class UDPBus:
    def __init__(self, ip, port):
        self.ip = ip
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.1)
        self.last_heartbeat = 0
        self._send_heartbeat()

    def _send_heartbeat(self):
        # Send a heartbeat so server knows where we are
        try:
            self.sock.sendto(struct.pack(">IB", 0, 0), (self.ip, self.port))
        except:
            pass
        self.last_heartbeat = time.time()

    def recv(self, timeout=None):
        if time.time() - self.last_heartbeat > 1.0:
            self._send_heartbeat()
            
        if timeout is not None:
            self.sock.settimeout(timeout)
        try:
            data, addr = self.sock.recvfrom(1024)
            if len(data) >= 5:
                can_id, length = struct.unpack(">IB", data[:5])
                payload = data[5:5+length]
                return can.Message(arbitration_id=can_id, data=payload, is_extended_id=True)
        except socket.timeout:
            pass
        except Exception as e:
            logger.error(f"UDPBus recv error: {e}")
        return None

    def send(self, msg):
        packet = struct.pack(">IB", msg.arbitration_id, len(msg.data)) + bytes(msg.data)
        self.sock.sendto(packet, (self.ip, self.port))
        logger.debug(f"UDPBus TX: ID=0x{msg.arbitration_id:08X} Len={len(msg.data)} to {self.ip}:{self.port}")

    def shutdown(self):
        self.sock.close()


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
        self.print_traffic = tk.BooleanVar(value=False)
        self.last_msg_time = time.time()
        self.total_bits = 0
        self.last_util_time = time.time()
        self.bus_util_str = tk.StringVar(value="Bus: 0.0% | 1s Avg: 0.0% | Max: 0.0%")
        self.util_history = deque(maxlen=10)
        self.max_bus_util = 0.0
        
        # Connection variables
        self.conn_type = tk.StringVar(value="CANable (slcan)")
        self.slcan_port = tk.StringVar(value="COM5")
        self.slcan_bitrate = tk.IntVar(value=1000000)
        self.udp_ip = tk.StringVar(value="10.TE.AM.2")
        self.udp_port = tk.IntVar(value=5900)
        
        self.running = False
        self.bus = None
        self.receive_thread = None

        self.setup_ui()
        self.update_ui()

    def connect_bus(self):
        if self.running:
            return
            
        try:
            if self.conn_type.get() == "CANable (slcan)":
                logger.info(f"Opening CAN bus on {self.slcan_port.get()} at {self.slcan_bitrate.get()}bps")
                self.bus = can.interface.Bus(bustype='slcan', channel=self.slcan_port.get(), bitrate=self.slcan_bitrate.get())
            else:
                logger.info(f"Opening UDP bus at {self.udp_ip.get()}:{self.udp_port.get()}")
                self.bus = UDPBus(self.udp_ip.get(), self.udp_port.get())
                
            self.running = True
            self.btn_connect.config(state="disabled")
            self.btn_disconnect.config(state="normal")
            
            self.receive_thread = threading.Thread(target=self.receive_can, name="ReceiveThread", daemon=True)
            self.util_history.clear()
            self.max_bus_util = 0.0
            self.receive_thread.start()
        except Exception as e:
            logger.error(f"Error opening CAN bus: {e}")
            self.running = False
            
    def disconnect_bus(self):
        self.running = False
        if self.receive_thread:
            self.receive_thread.join(timeout=1.0)
            self.receive_thread = None
        if self.bus:
            if hasattr(self.bus, 'shutdown'):
                self.bus.shutdown()
            self.bus = None
        
        self.btn_connect.config(state="normal")
        self.btn_disconnect.config(state="disabled")

    def setup_ui(self):
        style = ttk.Style()
        style.configure("Header.TLabel", font=("Arial", 12, "bold"))
        style.configure("Data.TLabel", font=("Consolas", 10))

        # Connection Section
        frame_conn = ttk.LabelFrame(self.root, text=" Connection ", padding=10)
        frame_conn.pack(fill="x", padx=10, pady=5)
        
        conn_row1 = ttk.Frame(frame_conn)
        conn_row1.pack(fill="x", pady=2)
        ttk.Radiobutton(conn_row1, text="CANable (slcan)", variable=self.conn_type, value="CANable (slcan)").pack(side="left")
        ttk.Label(conn_row1, text="Port:").pack(side="left", padx=(10,2))
        ttk.Entry(conn_row1, textvariable=self.slcan_port, width=8).pack(side="left")
        ttk.Label(conn_row1, text="Bitrate:").pack(side="left", padx=(10,2))
        ttk.Entry(conn_row1, textvariable=self.slcan_bitrate, width=10).pack(side="left")
        
        conn_row2 = ttk.Frame(frame_conn)
        conn_row2.pack(fill="x", pady=2)
        ttk.Radiobutton(conn_row2, text="UDP Server (Backporch)", variable=self.conn_type, value="UDP").pack(side="left")
        ttk.Label(conn_row2, text="IP:").pack(side="left", padx=(10,2))
        ttk.Entry(conn_row2, textvariable=self.udp_ip, width=15).pack(side="left")
        ttk.Label(conn_row2, text="Port:").pack(side="left", padx=(10,2))
        ttk.Entry(conn_row2, textvariable=self.udp_port, width=8).pack(side="left")
        
        conn_row3 = ttk.Frame(frame_conn)
        conn_row3.pack(fill="x", pady=5)
        self.btn_connect = ttk.Button(conn_row3, text="Connect", command=self.connect_bus)
        self.btn_connect.pack(side="left", padx=5)
        self.btn_disconnect = ttk.Button(conn_row3, text="Disconnect", command=self.disconnect_bus, state="disabled")
        self.btn_disconnect.pack(side="left", padx=5)

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

        # Debug Tools Section
        frame_debug = ttk.LabelFrame(self.root, text=" Debug Tools ", padding=10)
        frame_debug.pack(fill="x", padx=10, pady=5)
        ttk.Checkbutton(frame_debug, text="Log CAN Traffic to Console", variable=self.print_traffic).pack(side="left")
        ttk.Label(frame_debug, textvariable=self.bus_util_str, font=("Consolas", 10, "bold")).pack(side="right", padx=10)

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

            # CMD_START (Retry loop)
            ack_received = False
            for attempt in range(5):
                logger.info(f"Sending CMD_START to ID 0x{CTRL_ID:08X} (Attempt {attempt+1})")
                self.safe_send(can.Message(arbitration_id=CTRL_ID, data=[0x01], is_extended_id=True))
                
                # Wait for ACK (0xAA 0x00 or 0xAA 0x01)
                start_time = time.time()
                while time.time() - start_time < 0.5: # 500ms timeout per attempt
                    msg = wait_for_msg(0.1)
                    if msg:
                        logger.debug(f"Received during START wait: ID=0x{msg.arbitration_id:08X} Data={msg.data.hex()}")
                        # Match Class 1, Index 0 (Control) - ignore lower 6 bits (Device ID)
                        if (msg.arbitration_id & 0x1FFFFFC0) == (0x0A2A0400) and len(msg.data) >= 2:
                            if msg.data[0] == 0xAA and msg.data[1] == 0x00:
                                actual_dev_id = msg.arbitration_id & 0x3F
                                logger.info(f"Received ACK for START from Device ID {actual_dev_id}")
                                ack_received = True
                                break
                            elif msg.data[0] == 0xAA and msg.data[1] == 0x01:
                                logger.info(f"App detected. Transitioning to bootloader. Waiting 1.5s...")
                                self.update_status.set("Resetting device...")
                                time.sleep(1.5)
                                break # Exit inner loop to retry START command
                    if ack_received: break
                if ack_received: break
            
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
                self.safe_send(can.Message(arbitration_id=DATA_ID, data=list(chunk), is_extended_id=True))
                sent += len(chunk)
                self.progress["value"] = (sent / total_size) * 100
                if i % 512 == 0:
                    logger.debug(f"Sent {sent}/{total_size} bytes")
                    self.update_status.set(f"Sending: {sent}/{total_size} bytes")
                # Strictly pace the packets by 2.0 milliseconds (approx 500 packets/sec)
                # We use a precise busy-wait loop because time.sleep() on Windows 
                # can either yield 15ms or 0ms depending on the system clock tick rate, 
                # which causes massive UDP bursting that overflows the RoboRIO CAN TX queue.
                target_time = time.perf_counter() + 0.004
                while time.perf_counter() < target_time:
                    pass

            logger.info("All data sent. Sending COMMIT command.")
            self.update_status.set("Committing...")
            crc = self.calculate_stm32_crc(content)
            logger.debug(f"Calculated CRC32: 0x{crc:08X}")
            commit_data = [0x02] + list(struct.pack("<I", crc))
            self.safe_send(can.Message(arbitration_id=CTRL_ID, data=commit_data, is_extended_id=True))

            success = False
            start_time = time.time()
            while time.time() - start_time < 5.0:
                msg = wait_for_msg(0.1)
                if msg:
                    logger.debug(f"Received during COMMIT wait: ID=0x{msg.arbitration_id:08X} Data={msg.data.hex()}")
                    if (msg.arbitration_id & 0x1FFFFFC0) == (0x0A2A0400) and len(msg.data) >= 2:
                        if msg.data[0] == 0xAA:
                            actual_dev_id = msg.arbitration_id & 0x3F
                            if actual_dev_id != dev_id:
                                logger.warning(f"Received ACK from Device ID {actual_dev_id} while targeting {dev_id}")
                            
                            if msg.data[1] == 0x01:
                                logger.info(f"Update Successful! (from ID {actual_dev_id})")
                                success = True
                                self.update_status.set("Update Successful! Rebooting...")
                                break
                            elif msg.data[1] == 0xEE:
                                logger.error(f"Update failed: CRC Mismatch reported by device {actual_dev_id}")
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
                    # Count bits for utilization (approx 160 bits for 8-byte extended frame)
                    # Extended classic CAN: ~80 bits base + ~10 bits per data byte including stuffing
                    msg_bits = 80 + (len(msg.data) * 10)
                    self.total_bits += msg_bits

                    if self.print_traffic.get():
                        now = time.time()
                        delta = (now - self.last_msg_time) * 1000
                        logger.info(f"[+{delta:6.1f}ms] {self.format_can_msg(msg)}")
                        self.last_msg_time = now
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

        # Route based on API
        if api_id == 0x50: # Class 5, Index 0 (SW Version)
            uhash = struct.unpack("<I", msg.data[0:4])[0]
            mode_byte = msg.data[4]
            mode_str = "App" if (mode_byte & 0x01) else "Boot"
            major = (mode_byte >> 1) & 0x07
            minor = (mode_byte >> 4) & 0x0F
            build = struct.unpack("<I", msg.data[4:8])[0] >> 8
            device["data"]["UniqueID"] = uhash
            device["data"]["Version"] = {"Hash": uhash, "Type": mode_str, "Major": major, "Minor": minor, "Build": build}
            # Trigger UI update for ID if needed
            self.update_device_id_display(device, dev_id, uhash)

        elif api_id == 0x51: # Class 5, Index 1 (General)
            # Match application's pack format [I B H b] if hash is included, or just the values
            # Based on previous logic assuming [I B H b]:
            uid_hash, current, voltage, temp = struct.unpack("<IBHb", msg.data)
            device["data"]["UniqueID"] = uid_hash
            device["data"]["General"] = {"Current": current, "Voltage": voltage, "Temp": temp}
            self.update_device_id_display(device, dev_id, uid_hash)

        elif api_id == 0x52: # Class 5, Index 2 (TOF)
            status = msg.data[0]
            limits = msg.data[1]
            dist, amb, sig = struct.unpack("<HHH", msg.data[2:8])
            device["data"]["TOF"] = {"Status": status, "Limits": limits, "Distance": dist, "Ambient": amb, "Signal": sig}

        elif api_id == 0x53: # Class 5, Index 3 (Encoder)
            e1a, e1i, e2a, e2i = struct.unpack("<HhHh", msg.data)
            device["data"]["Encoder"] = {"Enc1_Abs": e1a/100, "Enc1_Inc": e1i, "Enc2_Abs": e2a/100, "Enc2_Inc": e2i}

    def create_device(self, dev_id):
        # UI Frame for this device
        frame = ttk.LabelFrame(self.devices_frame, text=f" Device ID: {dev_id} ", padding=5)
        
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
        labels["tof"] = ttk.Label(row2, text="Dist: - mm | Status: - | LS: (SDA:-, SCL:-)", style="Data.TLabel")
        labels["tof"].pack(side="left", padx=5)
        labels["enc"] = ttk.Label(row2, text="E1: -/- deg | E2: -/- deg", style="Data.TLabel")
        labels["enc"].pack(side="left", padx=20)

        # Row 3: ID Assignment
        row3 = ttk.Frame(frame)
        row3.pack(fill="x")
        ttk.Label(row3, text="New ID:").pack(side="left", padx=5)
        new_id_var = tk.IntVar(value=dev_id)
        ttk.Spinbox(row3, from_=0, to=63, textvariable=new_id_var, width=5).pack(side="left", padx=5)
        ttk.Button(row3, text="Assign", command=lambda d=dev_id, v=new_id_var: self.assign_id(d, v)).pack(side="left", padx=5)
        labels["uid"] = ttk.Label(row3, text="UID: --------", style="Data.TLabel")
        labels["uid"].pack(side="left", padx=10)

        self.devices[dev_id] = {
            "last_seen": time.time(),
            "frame": frame,
            "labels": labels,
            "data": {
                "UniqueID": 0,
                "Version": {"Type": "-", "Major": 0, "Minor": 0, "Build": 0},
                "General": {"Current": 0, "Voltage": 0},
                "TOF": {"Distance": 0, "Status": 0},
                "Encoder": {"Enc1_Abs": 0, "Enc1_Inc": 0, "Enc2_Abs": 0, "Enc2_Inc": 0}
            }
        }
        self.reorder_devices()

    def reorder_devices(self):
        """Re-sorts the device frames in the UI by their device ID."""
        sorted_ids = sorted(self.devices.keys())
        for i, dev_id in enumerate(sorted_ids):
            self.devices[dev_id]["frame"].grid(row=i, column=0, sticky="ew", pady=2)
            self.devices_frame.columnconfigure(0, weight=1)

    def update_device_id_display(self, device, dev_id, uid_hash):
        device["labels"]["uid"].config(text=f"UID: {uid_hash:08X}")

    def assign_id(self, old_dev_id, new_id_var):
        new_id = new_id_var.get() & 0x3F
        uid = self.devices[old_dev_id]["data"]["UniqueID"]
        if uid == 0:
            logger.error("Cannot assign ID: Unique ID not yet discovered")
            return
            
        # WPILib Broadcast for Assign ID: Type=10, Mfg=42, Class=0, Index=0, Dev=0
        # Arb ID: 0x0A2A0000
        ASSIGN_ID = 0x0A2A0000
        payload = list(struct.pack("<I", uid)) + [new_id]
        while len(payload) < 8: payload.append(0)
        
        logger.info(f"Assigning Device ID {new_id} to Unique ID {uid:08X}")
        self.safe_send(can.Message(arbitration_id=ASSIGN_ID, data=payload, is_extended_id=True))

    def safe_send(self, msg):
        """Sends a message and counts bits for utilization."""
        if self.bus is None:
            logger.error("Send error: Not connected")
            return
        try:
            self.bus.send(msg)
            # Count bits for utilization (approx 160 bits for 8-byte extended frame)
            msg_bits = 80 + (len(msg.data) * 10)
            self.total_bits += msg_bits
        except Exception as e:
            logger.error(f"Send error: {e}")

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

    def format_can_msg(self, msg):
        """Formats a WPILib CAN message for display."""
        arb_id = msg.arbitration_id
        # Range: | Type (5) | Mfg (8) | Class (6) | Index (4) | ID (6) |
        dev_type = (arb_id >> 24) & 0x1F
        mfg = (arb_id >> 16) & 0xFF
        api_class = (arb_id >> 10) & 0x3F
        api_index = (arb_id >> 6) & 0x0F
        dev_id = arb_id & 0x3F
        
        data_hex = " ".join(f"{b:02X}" for b in msg.data)
        return f"ID: 0x{arb_id:08X} [T:{dev_type:2} M:{mfg:3} C:{api_class:2} I:{api_index:2} D:{dev_id:2}] Data: {data_hex}"

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
            sda = "P" if t.get("Limits", 0) & 0x01 else "R"
            scl = "P" if t.get("Limits", 0) & 0x02 else "R"
            l["tof"].config(text=f"Dist: {t['Distance']} mm | Status: {t['Status']} | LS: (SDA:{sda}, SCL:{scl})")
            
            e = d["Encoder"]
            l["enc"].config(text=f"E1: {e['Enc1_Abs']:.2f}°/{e['Enc1_Inc']}c | E2: {e['Enc2_Abs']:.2f}°/{e['Enc2_Inc']}c")

        # Update Bus Utilization
        now_util = time.time()
        dt = now_util - self.last_util_time
        if dt >= 0.1: # Update every 100ms
            # Capacity in bits = bitrate * dt
            # Try to use configured bitrate or default to 1Mbps
            try:
                bitrate = self.slcan_bitrate.get()
            except:
                bitrate = 1000000
                
            capacity = bitrate * dt
            util = (self.total_bits / capacity) * 100
            
            self.util_history.append(util)
            self.max_bus_util = max(self.max_bus_util, util)
            avg_util = sum(self.util_history) / len(self.util_history) if self.util_history else 0
            
            self.bus_util_str.set(f"Bus: {util:4.1f}% | 1s Avg: {avg_util:4.1f}% | Max: {self.max_bus_util:4.1f}%")
            self.total_bits = 0
            self.last_util_time = now_util

        for dev_id in to_delete:
            logger.info(f"Removing timed out device: {dev_id}")
            self.devices[dev_id]["frame"].destroy()
            del self.devices[dev_id]
            self.reorder_devices()

        self.root.after(100, self.update_ui)

if __name__ == "__main__":
    root = tk.Tk()
    app = CanMonitorApp(root)
    root.mainloop()
