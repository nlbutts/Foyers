package com.qnibbles.hardware;

import edu.wpi.first.hal.CANData;
import edu.wpi.first.wpilibj.CAN;
import edu.wpi.first.wpilibj.Timer;

/**
 * Driver for the Foyer device using WPILib CAN.
 */
public class FoyerDevice {
    private final CAN m_can;
    private final CANData m_data = new CANData();

    public record VersionStatus(
        long uniqueID,
        boolean isAppMode,
        int major,
        int minor,
        long build,
        long timestamp
    ) {}

    public record FoyerStatus(
        long uniqueID,
        int currentMA,
        double voltageV,
        int tempC,
        long timestamp
    ) {}

    public record TOFStatus(
        int apiStatus,
        Boolean limit1,
        Boolean limit2,
        int distanceMM,
        int ambientMcps,
        int signalMcps,
        long timestamp
    ) {}

    public record EncoderStatus(
        double enc1AbsDeg,
        int enc1Inc,
        double enc2AbsDeg,
        int enc2Inc,
        long timestamp
    ) {}

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
        if (m_can.readPacketLatest(0x50, m_data)) {
            return new VersionStatus(
                ((long)m_data.data[0] & 0xFF) |
                (((long)m_data.data[1] & 0xFF) << 8) |
                (((long)m_data.data[2] & 0xFF) << 16) |
                (((long)m_data.data[3] & 0xFF) << 24),
                (m_data.data[4] & 0x01) != 0,
                (m_data.data[4] >> 1) & 0x07,
                (m_data.data[4] >> 4) & 0x0F,
                ((long)m_data.data[5] & 0xFF) |
                (((long)m_data.data[6] & 0xFF) << 8) |
                (((long)m_data.data[7] & 0xFF) << 16),
                m_data.timestamp
            );
        } else {
            return null;
        }
    }

    /**
     * Read the latest General Status message (API 0x51).
     * @return FoyerStatus object or null if no new data.
     */
    public FoyerStatus getGeneralStatus() {
        if (m_can.readPacketLatest(0x51, m_data)) {
            return new FoyerStatus(
                ((long)m_data.data[0] & 0xFF) |
                (((long)m_data.data[1] & 0xFF) << 8) |
                (((long)m_data.data[2] & 0xFF) << 16) |
                (((long)m_data.data[3] & 0xFF) << 24),
                m_data.data[4] & 0xFF,
                ((m_data.data[5] & 0xFF) | ((m_data.data[6] & 0xFF) << 8)) / 1000.0,
                m_data.data[7],
                m_data.timestamp
            );
        } else {
            return null;
        }
    }

    /**
     * Read the latest TOF Status message (API 0x52).
     * @return TOFStatus object or null if no new data.
     */
    public TOFStatus getTOFStatus() {
        if (m_can.readPacketLatest(0x52, m_data)) {
            return new TOFStatus(
                m_data.data[0] & 0xFF,
                (m_data.data[1] & 1) != 0,
                (m_data.data[1] & 2) != 0,
                (m_data.data[2] & 0xFF) | ((m_data.data[3] & 0xFF) << 8),
                (m_data.data[4] & 0xFF) | ((m_data.data[5] & 0xFF) << 8),
                (m_data.data[6] & 0xFF) | ((m_data.data[7] & 0xFF) << 8),
                m_data.timestamp
            );
        } else {
            return null;
        }
    }

    /**
     * Read the latest Encoder Status message (API 0x53).
     * @return EncoderStatus object or null if no new data.
     */
    public EncoderStatus getEncoderStatus() {
        if (m_can.readPacketLatest(0x53, m_data)) {
            return new EncoderStatus(
                ((m_data.data[0] & 0xFF) | ((m_data.data[1] & 0xFF) << 8)) / 100.0,
                (short)((m_data.data[2] & 0xFF) | ((m_data.data[3] & 0xFF) << 8)),
                ((m_data.data[4] & 0xFF) | ((m_data.data[5] & 0xFF) << 8)) / 100.0,
                (short)((m_data.data[6] & 0xFF) | ((m_data.data[7] & 0xFF) << 8)),
                m_data.timestamp
            );
        } else {
            return null;
        }
    }

    /**
     * Determines whether the TOF sensor is connected.
     * @return {@code true} if the last frame is less than 100ms old.
    */
    public boolean isTOFSensorConnected() {
        if (!m_can.readPacketLatest(0x52, m_data)) {
            return false;
        } else {
            long currentTime = (long)(Timer.getTimestamp() * 1000.0);
            return (currentTime - m_data.timestamp) < 100L;
        }
    }

    /**
     * Determines whether the through-bore encoder is connected.
     * @return {@code true} if the last frame is less than 100ms old.
    */
    public boolean isEncoderConnected() {
        if (!m_can.readPacketLatest(0x53, m_data)) {
            return false;
        } else {
            long currentTime = (long)(Timer.getTimestamp() * 1000.0);
            return (currentTime - m_data.timestamp) < 100L;
        }
    }
}
