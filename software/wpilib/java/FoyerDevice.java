package frc.robot;

import edu.wpi.first.hal.CANData;
import edu.wpi.first.wpilibj.CAN;

/**
 * Driver for the Foyer device using WPILib CAN.
 */
public class FoyerDevice {
    private final CAN m_can;
    private final CANData m_data = new CANData();

    public static class VersionStatus {
        public long uniqueID;
        public boolean isAppMode;
        public int major;
        public int minor;
        public long build;
        public long timestamp;
    }

    public static class FoyerStatus {
        public long uniqueID;
        public int currentMA;
        public double voltageV;
        public int tempC;
        public long timestamp;
    }

    public static class TOFStatus {
        public int apiStatus;
        public int distanceMM;
        public int ambientMcps;
        public int signalMcps;
        public long timestamp;
    }

    public static class EncoderStatus {
        public double enc1AbsDeg;
        public int enc1Inc;
        public double enc2AbsDeg;
        public int enc2Inc;
        public long timestamp;
    }

    /**
     * @param deviceId The CAN ID configured on the device (0-63).
     */
    public FoyerDevice(int deviceId) {
        // Manufacturer: 42, Device Type: 10
        m_can = new CAN(deviceId, 42, 10);
    }

    /**
     * Read the latest Software Version message (API 0x50).
     * @return VersionStatus object or null if no new data.
     */
    public VersionStatus getVersionStatus() {
        if (m_can.readPacketNew(0x50, m_data)) {
            VersionStatus s = new VersionStatus();
            s.uniqueID = ((long)m_data.data[0] & 0xFF) | 
                         (((long)m_data.data[1] & 0xFF) << 8) | 
                         (((long)m_data.data[2] & 0xFF) << 16) | 
                         (((long)m_data.data[3] & 0xFF) << 24);
            s.isAppMode = (m_data.data[4] & 0x01) != 0;
            s.major = (m_data.data[4] >> 1) & 0x07;
            s.minor = (m_data.data[4] >> 4) & 0x0F;
            s.build = ((long)m_data.data[5] & 0xFF) | 
                      (((long)m_data.data[6] & 0xFF) << 8) | 
                      (((long)m_data.data[7] & 0xFF) << 16);
            s.timestamp = m_data.timestamp;
            return s;
        }
        return null;
    }

    /**
     * Read the latest General Status message (API 0x51).
     * @return FoyerStatus object or null if no new data.
     */
    public FoyerStatus getGeneralStatus() {
        if (m_can.readPacketNew(0x51, m_data)) {
            FoyerStatus s = new FoyerStatus();
            s.uniqueID = ((long)m_data.data[0] & 0xFF) | 
                         (((long)m_data.data[1] & 0xFF) << 8) | 
                         (((long)m_data.data[2] & 0xFF) << 16) | 
                         (((long)m_data.data[3] & 0xFF) << 24);
            s.currentMA = m_data.data[4] & 0xFF;
            s.voltageV = ((m_data.data[5] & 0xFF) | ((m_data.data[6] & 0xFF) << 8)) / 1000.0;
            s.tempC = m_data.data[7];
            s.timestamp = m_data.timestamp;
            return s;
        }
        return null;
    }

    /**
     * Read the latest TOF Status message (API 0x52).
     * @return TOFStatus object or null if no new data.
     */
    public TOFStatus getTOFStatus() {
        if (m_can.readPacketNew(0x52, m_data)) {
            TOFStatus s = new TOFStatus();
            s.apiStatus = m_data.data[0] & 0xFF;
            s.distanceMM = (m_data.data[2] & 0xFF) | ((m_data.data[3] & 0xFF) << 8);
            s.ambientMcps = (m_data.data[4] & 0xFF) | ((m_data.data[5] & 0xFF) << 8);
            s.signalMcps = (m_data.data[6] & 0xFF) | ((m_data.data[7] & 0xFF) << 8);
            s.timestamp = m_data.timestamp;
            return s;
        }
        return null;
    }

    /**
     * Read the latest Encoder Status message (API 0x53).
     * @return EncoderStatus object or null if no new data.
     */
    public EncoderStatus getEncoderStatus() {
        if (m_can.readPacketNew(0x53, m_data)) {
            EncoderStatus s = new EncoderStatus();
            s.enc1AbsDeg = ((m_data.data[0] & 0xFF) | ((m_data.data[1] & 0xFF) << 8)) / 100.0;
            s.enc1Inc = (short)((m_data.data[2] & 0xFF) | ((m_data.data[3] & 0xFF) << 8));
            s.enc2AbsDeg = ((m_data.data[4] & 0xFF) | ((m_data.data[5] & 0xFF) << 8)) / 100.0;
            s.enc2Inc = (short)((m_data.data[6] & 0xFF) | ((m_data.data[7] & 0xFF) << 8));
            s.timestamp = m_data.timestamp;
            return s;
        }
        return null;
    }
}
