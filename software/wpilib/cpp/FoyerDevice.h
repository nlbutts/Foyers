#pragma once

#include <frc/CAN.h>
#include <stdint.h>
#include <vector>

namespace foyer {

/**
 * Structure for Software Version message (API Class 5, Index 0)
 */
struct VersionStatus {
  uint32_t uniqueID;
  bool isAppMode;
  uint8_t major;
  uint8_t minor;
  uint32_t build;
  uint32_t timestamp;
};

/**
 * Structure for General Status message (API Class 5, Index 1)
 */
struct FoyerStatus {
  uint32_t uniqueID;
  uint8_t currentMA;
  float voltageV;
  int8_t tempC;
  uint32_t timestamp;
};

/**
 * Structure for TOF Status message (API Class 5, Index 2)
 */
struct TOFStatus {
  uint8_t apiStatus;
  uint16_t distanceMM;
  uint16_t ambientMcps;
  uint16_t signalMcps;
  uint32_t timestamp;
};

/**
 * Structure for Encoder Status message (API Class 5, Index 3)
 */
struct EncoderStatus {
  float enc1AbsDeg;
  int16_t enc1Inc;
  float enc2AbsDeg;
  int16_t enc2Inc;
  uint32_t timestamp;
};

/**
 * Driver for the Foyer device using WPILib CAN.
 */
class FoyerDevice {
public:
  /**
   * @param deviceID The CAN ID configured on the device (0-63).
   */
  explicit FoyerDevice(int deviceID);

  /**
   * Polls the latest packets from the CAN bus.
   * @return true if any new data was received.
   */
  bool Update();

  const VersionStatus &GetVersionStatus() const { return m_versionStatus; }
  const FoyerStatus &GetGeneralStatus() const { return m_generalStatus; }
  const TOFStatus &GetTOFStatus() const { return m_tofStatus; }
  const EncoderStatus &GetEncoderStatus() const { return m_encoderStatus; }

private:
  frc::CAN m_can;
  VersionStatus m_versionStatus{0, false, 0, 0, 0, 0};
  FoyerStatus m_generalStatus{0, 0, 0.0f, 0, 0};
  TOFStatus m_tofStatus{0, 0, 0, 0, 0};
  EncoderStatus m_encoderStatus{0.0f, 0, 0.0f, 0, 0};

  static constexpr uint8_t kApiVersionStatus = (5 << 4) | 0;
  static constexpr uint8_t kApiGeneralStatus = (5 << 4) | 1;
  static constexpr uint8_t kApiTOFStatus = (5 << 4) | 2;
  static constexpr uint8_t kApiEncoderStatus = (5 << 4) | 3;
};

} // namespace foyer
