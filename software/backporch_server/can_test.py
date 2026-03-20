import socket
import struct
import threading
import sys
import argparse

def parse_can_frame(data):
    if len(data) < 5:
        return None
    
    # Unpack 4 bytes as Big-Endian Int, 1 byte as length
    can_id, length = struct.unpack(">IB", data[:5])
    
    # Slice the payload based on length
    payload = data[5:5+length]
    return can_id, length, payload

def receive_loop(sock, stop_event):
    print("Listening for incoming CAN frames... (Press Ctrl+C to stop or type 'quit')")
    sock.settimeout(1.0) # 1 sec timeout so we can check stop_event
    while not stop_event.is_set():
        try:
            data, addr = sock.recvfrom(1024)
            frame = parse_can_frame(data)
            if frame:
                can_id, length, payload = frame
                hex_data = " ".join([f"{b:02X}" for b in payload])
                # Overwrite the input prompt momentarily
                sys.stdout.write(f"\rRX: ID: 0x{can_id:08X} | Len: {length} | Data: [{hex_data}]\n> ")
                sys.stdout.flush()
        except socket.timeout:
            continue
        except Exception as e:
            if not stop_event.is_set():
                print(f"\nError receiving: {e}")
            break

def send_can_frame(sock, ip, port, can_id, payload):
    if len(payload) > 8:
        print("\nError: Payload cannot exceed 8 bytes.")
        return
    
    # Pack the ID (4 bytes, Big-Endian), Length (1 byte), and exactly the payload
    packet = struct.pack(">IB", can_id, len(payload)) + bytes(payload)
    sock.sendto(packet, (ip, port))
    
    hex_data = " ".join([f"{b:02X}" for b in payload])
    # Print what we just sent
    sys.stdout.write(f"\rTX: ID: 0x{can_id:08X} | Len: {len(payload)} | Data: [{hex_data}]\n> ")
    sys.stdout.flush()

def main():
    parser = argparse.ArgumentParser(description="Test script for the RoboRIO Ethernet-to-CAN Server")
    parser.add_argument("--ip", type=str, default="127.0.0.1", help="RoboRIO IP address. Examples: 10.TE.AM.2 or roborio-TEAM-frc.local")
    parser.add_argument("--port", type=int, default=5800, help="UDP port (must match the port in UDPCANServer.java)")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    stop_event = threading.Event()
    recv_thread = threading.Thread(target=receive_loop, args=(sock, stop_event), daemon=True)
    recv_thread.start()

    print(f"\nConnected to RoboRIO at {args.ip}:{args.port}")
    print("Sending heartbeat dummy message so the server knows where to reply...")
    send_can_frame(sock, args.ip, args.port, 0x00, [])

    print("\n--- Interactive CAN Terminal ---")
    print("Format: <hex_id> [hex_data_bytes...]")
    print("Example: 1F0 11 22 33 44")
    print("Type 'q' or 'quit' to exit.")

    try:
        while True:
            cmd = input("> ").strip()
            if cmd.lower() in ('q', 'quit', 'exit'):
                break
            
            if not cmd:
                continue
                
            parts = cmd.split()
            try:
                can_id = int(parts[0], 16)
                data = [int(x, 16) for x in parts[1:]]
                send_can_frame(sock, args.ip, args.port, can_id, data)
            except ValueError:
                print("Invalid input. Please use hexadecimal format without '0x'.")
    except KeyboardInterrupt:
        print("\nExiting...")
    finally:
        stop_event.set()
        recv_thread.join(timeout=2)
        sock.close()

if __name__ == "__main__":
    main()
