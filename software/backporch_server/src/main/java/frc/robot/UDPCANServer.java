package frc.robot;

import edu.wpi.first.hal.can.CANJNI;
import edu.wpi.first.wpilibj.RobotBase;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.SocketException;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.nio.IntBuffer;
import java.nio.ByteOrder;

/**
 * A simple Ethernet to CAN server over UDP.
 * Listens for UDP packets to send frames to the CAN bus.
 * Reads CAN frames and sends them as UDP packets to the last known client.
 *
 * Uses a single long-lived DatagramSocket. UDP is connectionless, so there is
 * no need to close/reopen the socket when a client disconnects. We simply
 * clear the target address and wait for the next heartbeat.
 */
public class UDPCANServer {
    private DatagramSocket socket;
    private Thread receiveThread;
    private Thread sendThread;
    private volatile boolean running = false;
    private final int port;

    // Track the last client that sent us a message to forward CAN traffic to
    private volatile InetAddress clientAddress = null;
    private volatile int clientPort = -1;

    public UDPCANServer(int port) {
        this.port = port;
    }

    public void start() {
        if (running) return;

        try {
            socket = new DatagramSocket(port);
            System.out.println("UDP CAN Server listening on port " + port);
        } catch (SocketException e) {
            System.err.println("Failed to bind UDP socket on port " + port + ": " + e.getMessage());
            return;
        }

        running = true;

        receiveThread = new Thread(this::receiveLoop, "UDPCANServer-RX");
        receiveThread.setDaemon(true);
        receiveThread.start();

        sendThread = new Thread(this::sendLoop, "UDPCANServer-TX");
        sendThread.setDaemon(true);
        sendThread.start();
    }

    public void stop() {
        running = false;
        if (socket != null && !socket.isClosed()) {
            socket.close();
        }
    }

    /**
     * Receives UDP packets from the client. Each packet is either a heartbeat
     * (length < 5) which just registers the client, or a CAN frame to transmit.
     */
    private void receiveLoop() {
        byte[] receiveData = new byte[1024];

        while (running && !socket.isClosed()) {
            try {
                DatagramPacket receivePacket = new DatagramPacket(receiveData, receiveData.length);
                socket.receive(receivePacket);

                // Always update client address on every received packet
                clientAddress = receivePacket.getAddress();
                clientPort = receivePacket.getPort();

                byte[] data = receivePacket.getData();
                int length = receivePacket.getLength();

                // Only process as a CAN frame if we have enough bytes
                if (length >= 5) {
                    ByteBuffer buffer = ByteBuffer.wrap(data, 0, length);
                    int canId = buffer.getInt();
                    int dataLen = buffer.get() & 0xFF;

                    if (canId == 0 && dataLen == 0) {
                        // This is just a heartbeat/registration packet, skip CAN send
                        System.out.println("Client registered: " + clientAddress + ":" + clientPort);
                        continue;
                    }

                    if (dataLen <= 8 && length >= 5 + dataLen) {
                        byte[] canData = new byte[dataLen];
                        buffer.get(canData);

                        StringBuilder hexStr = new StringBuilder();
                        for (byte b : canData) hexStr.append(String.format("%02X ", b & 0xFF));

                        System.out.printf("UDP->CAN TX: ID=0x%08X Len=%d Data=[%s]%n", canId, dataLen, hexStr.toString().trim());

                        if (RobotBase.isReal()) {
                            // periodMs: 0 = HAL_CAN_SEND_PERIOD_NO_REPEAT (send once)
                            //          -1 = HAL_CAN_SEND_PERIOD_STOP_REPEATING (stop periodic)
                            CANJNI.FRCNetCommCANSessionMuxSendMessage(canId, canData, 0);
                            System.out.printf("  -> CANJNI.SendMessage called OK%n");
                        } else {
                            System.out.printf("  -> Sim mode, not sent to CAN bus%n");
                        }
                    } else {
                        System.out.printf("UDP packet rejected: canId=0x%08X dataLen=%d pktLen=%d%n", canId, dataLen, length);
                    }
                } else {
                    System.out.printf("UDP packet too short: %d bytes%n", length);
                }
            } catch (IOException e) {
                if (running) {
                    // On Linux (RoboRIO) UDP doesn't normally get ICMP errors back,
                    // but handle gracefully regardless. Don't close the socket.
                    System.err.println("UDP Receive error (non-fatal): " + e.getMessage());
                }
            }
        }
        System.out.println("UDPCANServer receive thread exiting.");
    }

    /**
     * Reads CAN frames from the HAL and forwards them to the registered client.
     * If no client is registered, frames are silently discarded.
     * If a send fails, we just clear the client and wait for a new heartbeat.
     */
    private void sendLoop() {
        IntBuffer idBuffer = ByteBuffer.allocateDirect(4).order(ByteOrder.nativeOrder()).asIntBuffer();
        int mask = 0; // Read all messages
        ByteBuffer timeStamp = ByteBuffer.allocateDirect(4);

        while (running && !socket.isClosed()) {
            if (RobotBase.isReal()) {
                try {
                    byte[] data = CANJNI.FRCNetCommCANSessionMuxReceiveMessage(idBuffer, mask, timeStamp);

                    // Snapshot the volatile client info
                    InetAddress targetAddr = clientAddress;
                    int targetPort = clientPort;

                    if (data != null && targetAddr != null && targetPort != -1) {
                        int msgId = idBuffer.get(0);

                        ByteBuffer outBuffer = ByteBuffer.allocate(5 + data.length);
                        outBuffer.putInt(msgId);
                        outBuffer.put((byte) (data.length & 0xFF));
                        outBuffer.put(data);

                        byte[] packetData = outBuffer.array();
                        DatagramPacket sendPacket = new DatagramPacket(
                                packetData, packetData.length, targetAddr, targetPort);

                        try {
                            socket.send(sendPacket);
                        } catch (IOException sendEx) {
                            // Client is gone. Clear it and wait for a new heartbeat.
                            System.err.println("UDP send failed, clearing client: " + sendEx.getMessage());
                            clientAddress = null;
                            clientPort = -1;
                        }
                    }
                } catch (RuntimeException e) {
                    if (e.getClass().getSimpleName().contains("NotFound")) {
                        // Normal: no CAN message in the queue. Yield CPU briefly.
                        try { Thread.sleep(1); } catch (InterruptedException ex) { break; }
                    } else {
                        e.printStackTrace();
                        try { Thread.sleep(100); } catch (InterruptedException ex) { break; }
                    }
                } catch (Exception e) {
                    e.printStackTrace();
                    try { Thread.sleep(100); } catch (InterruptedException ex) { break; }
                }
            } else {
                // Simulation: nothing to read
                try { Thread.sleep(100); } catch (InterruptedException e) { break; }
            }
        }
        System.out.println("UDPCANServer send thread exiting.");
    }
}
