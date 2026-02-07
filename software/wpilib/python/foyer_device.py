import wpilib
import struct

class FoyerDevice:
    """
    Driver for the Foyer device using RobotPy WPILib CAN.
    """
    def __init__(self, device_id):
        # Manufacturer: 42, Device Type: 10
        self.can = wpilib.CAN(device_id, 42, 10)

    def get_version_status(self):
        """
        Read the latest Software Version message (API 0x50).
        :return: Dict with data or None
        """
        data, timestamp = self.can.readPacketNewer(0x50)
        if data:
            uid = struct.unpack("<I", data[0:4])[0]
            mode_byte = data[4]
            is_app = (mode_byte & 0x01) != 0
            major = (mode_byte >> 1) & 0x07
            minor = (mode_byte >> 4) & 0x0F
            build = struct.unpack("<I", data[4:8])[0] >> 8
            return {
                "uid": uid,
                "is_app_mode": is_app,
                "major": major,
                "minor": minor,
                "build": build,
                "timestamp": timestamp
            }
        return None

    def get_general_status(self):
        """
        Read the latest General Status message (API 0x51).
        :return: Dict with data or None
        """
        data, timestamp = self.can.readPacketNewer(0x51)
        if data:
            uid, current, voltage, temp = struct.unpack("<IBHb", data)
            return {
                "uid": uid,
                "current_ma": current,
                "voltage_v": voltage / 1000.0,
                "temp_c": temp,
                "timestamp": timestamp
            }
        return None

    def get_tof_status(self):
        """
        Read the latest TOF Status message (API 0x52).
        :return: Dict with data or None
        """
        data, timestamp = self.can.readPacketNewer(0x52)
        if data:
            # Bytes: [status, reserved, dist_low, dist_high, amb_low, amb_high, sig_low, sig_high]
            status = data[0]
            # Skip byte 1
            dist, amb, sig = struct.unpack("<HHH", data[2:8])
            return {
                "status": status,
                "distance_mm": dist,
                "ambient_mcps": amb,
                "signal_mcps": sig,
                "timestamp": timestamp
            }
        return None

    def get_encoder_status(self):
        """
        Read the latest Encoder Status message (API 0x53).
        :return: Dict with data or None
        """
        data, timestamp = self.can.readPacketNewer(0x53)
        if data:
            # Bytes: [e1a_low, e1a_high, e1i_low, e1i_high, e2a_low, e2a_high, e2i_low, e2i_high]
            e1a, e1i, e2a, e2i = struct.unpack("<HhHh", data)
            return {
                "enc1_abs_deg": e1a / 100.0,
                "enc1_inc": e1i,
                "enc2_abs_deg": e2a / 100.0,
                "enc2_inc": e2i,
                "timestamp": timestamp
            }
        return None
