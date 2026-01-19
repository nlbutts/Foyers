# STM32G0B1 CAN Bootloader

This project implements a CAN-based bootloader for the STM32G0B1 using the WPIlib CAN addressing scheme.

## Overview
- **Bootloader Size**: 32KB (`0x08000000` - `0x08007FFF`)
- **Application App Size**: 96KB (`0x08008000` - `0x0801FFFF`)
- **RAM Buffer**: 96KB Reserved at `0x2000C000`

## CAN Protocol
The bootloader uses 29-bit Extended IDs following the WPIlib format.
- **Device Type**: `10` (Miscellaneous)
- **Manufacturer**: `42` (Custom)
- **Device ID**: `0` (Default)

### CAN ID Structure
| Bits [28:24] | Bits [23:16] | Bits [15:10] | Bits [9:6] | Bits [5:0] |
|--------------|--------------|--------------|------------|------------|
| Device Type  | Manufacturer | API Class    | API Index  | Device ID  |
| `0x0A` (10)  | `0x2A` (42)  | *Variable*   | `0`        | `0`        |

### API Classes
1. **Control (0x01)**: Command and Status operations.
2. **Data (0x02)**: streaming binary application data.

### Commands
#### 1. Start Session
Prepare bootloader to receive new data.
- **ID**: `0x0A2A0400` (Class 1)
- **Data[0]**: `0x01` (CMD_START)
- **Response**: `0xAA 0x00 ...` (ACK)

#### 2. Streaming Data
Send application binary in 8-byte chunks.
- **ID**: `0x0A2A0800` (Class 2)
- **Data**: `[Bytes 0-7]`
- **Note**: Must start sending from the beginning of the binary file.

#### 3. Commit & Flash
Verify integrity and program to Flash.
- **ID**: `0x0A2A0400` (Class 1)
- **Data**: `0x02` (CMD_COMMIT) + `[CRC32 (4 Bytes, Little Endian)]`
- **Response**: 
    - `0xAA 0x01 ...` (Success/Rebooting)
    - `0xAA 0xEE ...` (CRC Failure)

## CRC Verification
The bootloader uses the standard **STM32 Hardware CRC-32** unit.
- **Polynomial**: `0x04C11DB7` (Ethernet Standard)
- **Initial Value**: `0xFFFFFFFF`
- **Input/Output Inversion**: None
- **Input Format**: 32-bit Words (Big Endian logic in terms of word feed, but Little Endian memory. Standard STM32 HAL behavior).

### Calculating CRC for Build
The Host tool must calculate the CRC32 of the binary file to send in the `COMMIT` command.
**Python Example**:
```python
import zlib

def calc_crc(file_path):
    with open(file_path, 'rb') as f:
        data = f.read()
    
    # STM32 CRC-32 is slightly different from zlib.crc32 (which is usually inverted).
    # However, STM32 HAL with Default settings is:
    # Poly: 0x04C11DB7, Init: 0xFFFFFFFF
    # Software equivalent usually requires custom implementation or 'crcmod'.
    # Standard MPEG-2 CRC-32 is often close.
    pass
```
*Note*: The easiest way to verify the CRC algorithm matches is to run a small known binary on the chip and print the calculated CRC via debug, then match your script to it.

### CRC Placement in Binary
For the *Boot-Up Verification* (checking app validity on power-on), the bootloader stores the CRC in a special **Boot Config** flash page at `0x08007800`.
- You **do not** need to embed the CRC in the application binary file itself.
- The Host Tool sends the CRC in the `COMMIT` packet.
- The Bootloader validates the RAM buffer against this CRC.
- If match, it flashes the App **AND** updates the Boot Config with this CRC.

## Testing Walkthrough
1. **Build**: Compile the Bootloader.
2. **Flash**: Load `bootloader.elf` to the target.
3. **Connect**: Use a USB-CAN Adapter (e.g., PCAN, CandleLight) with 500kbps (or configured baud).
4. **Commands**:
    - Send `START` (`0x0A2A0400` -> `01`)
    - Send `DATA` chunks (`0x0A2A0800` -> `...`)
    - Send `COMMIT` (`0x0A2A0400` -> `02 AA BB CC DD`)
5. **Verify**: Device should reboot and run application.
