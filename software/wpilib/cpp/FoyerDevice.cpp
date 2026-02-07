#include "FoyerDevice.h"
#include <hal/CAN.h>

using namespace foyer;

FoyerDevice::FoyerDevice(int deviceID) : m_can(deviceID, 42, 10) {}

bool FoyerDevice::Update() {
  bool updated = false;
  uint8_t data[8];
  int32_t length;
  uint32_t timestamp;

  // Software Version (Index 0)
  if (m_can.ReadPacketNewer(kApiVersionStatus, &data[0], &length, &timestamp)) {
    if (length >= 8) {
      m_versionStatus.uniqueID =
          data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      m_versionStatus.isAppMode = (data[4] & 0x01) != 0;
      m_versionStatus.major = (data[4] >> 1) & 0x07;
      m_versionStatus.minor = (data[4] >> 4) & 0x0F;
      m_versionStatus.build = data[5] | (data[6] << 8) | (data[7] << 16);
      m_versionStatus.timestamp = timestamp;
      updated = true;
    }
  }

  // General Status (Index 1)
  if (m_can.ReadPacketNewer(kApiGeneralStatus, &data[0], &length, &timestamp)) {
    if (length >= 8) {
      m_generalStatus.uniqueID =
          data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      m_generalStatus.currentMA = data[4];
      m_generalStatus.voltageV = (data[5] | (data[6] << 8)) / 1000.0f;
      m_generalStatus.tempC = static_cast<int8_t>(data[7]);
      m_generalStatus.timestamp = timestamp;
      updated = true;
    }
  }

  // TOF Status (Index 2)
  if (m_can.ReadPacketNewer(kApiTOFStatus, &data[0], &length, &timestamp)) {
    if (length >= 8) {
      m_tofStatus.apiStatus = data[0];
      m_tofStatus.distanceMM = data[2] | (data[3] << 8);
      m_tofStatus.ambientMcps = data[4] | (data[5] << 8);
      m_tofStatus.signalMcps = data[6] | (data[7] << 8);
      m_tofStatus.timestamp = timestamp;
      updated = true;
    }
  }

  // Encoder Status (Index 3)
  if (m_can.ReadPacketNewer(kApiEncoderStatus, &data[0], &length, &timestamp)) {
    if (length >= 8) {
      m_encoderStatus.enc1AbsDeg = (data[0] | (data[1] << 8)) / 100.0f;
      m_encoderStatus.enc1Inc = static_cast<int16_t>(data[2] | (data[3] << 8));
      m_encoderStatus.enc2AbsDeg = (data[4] | (data[5] << 8)) / 100.0f;
      m_encoderStatus.enc2Inc = static_cast<int16_t>(data[6] | (data[7] << 8));
      m_encoderStatus.timestamp = timestamp;
      updated = true;
    }
  }

  return updated;
}
