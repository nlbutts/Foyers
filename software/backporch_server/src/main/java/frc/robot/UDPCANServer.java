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
 * Listens for 13-byte UDP packets to send frames to the CAN bus.
 * Reads CAN frames and sends them as 13-byte UDP packets to the last known client.
 */
public class UDPCANServer {
    private DatagramSocket socket;
    private Thread serverThread;
    private boolean running = false;
    private final int port;
    
    // Track the last client that sent us a message to forward CAN traffic to
    private InetAddress clientAddress = null;
    private int clientPort = -1;

    public UDPCANServer(int port) {
        this.port = port;
    }

    public void start() {
        if (running) return;
        running = true;
        try {
            socket = new DatagramSocket(port);
            System.out.println("UDP CAN Server listening on port " + port);
        } catch (SocketException e) {
            e.printStackTrace();
            running = false;
            return;
        }

        serverThread = new Thread(this::runServer);
        serverThread.setDaemon(true);
        serverThread.start();
    }

    public void stop() {
        running = false;
        if (socket != null && !socket.isClosed()) {
            socket.close();
        }
    }

    private void runServer() {
        Thread canReadThread = new Thread(this::readCanFrames);
        canReadThread.setDaemon(true);
        canReadThread.start();

        byte[] receiveData = new byte[1024];

        while (running && !socket.isClosed()) {
            try {
                DatagramPacket receivePacket = new DatagramPacket(receiveData, receiveData.length);
                socket.receive(receivePacket);

                // Update client address so we know where to send CAN frames
                clientAddress = receivePacket.getAddress();
                clientPort = receivePacket.getPort();

                byte[] data = receivePacket.getData();
                int length = receivePacket.getLength();

                if (length >= 5) {
                    ByteBuffer buffer = ByteBuffer.wrap(data, 0, length);
                    int canId = buffer.getInt();
                    int dataLen = buffer.get() & 0xFF;
                    
                    if (dataLen <= 8 && length >= 5 + dataLen) {
                        byte[] canData = new byte[dataLen];
                        buffer.get(canData);

                        if (RobotBase.isReal()) {
                            CANJNI.FRCNetCommCANSessionMuxSendMessage(canId, canData, 0); // 0 means no repeat
                        } else {
                            System.out.printf("Sim: Sent CAN ID 0x%08X length %d%n", canId, dataLen);
                        }
                    }
                }
            } catch (IOException e) {
                if (running) {
                    e.printStackTrace();
                }
                break;
            }
        }
    }

    private void readCanFrames() {
        IntBuffer idBuffer = ByteBuffer.allocateDirect(4).order(ByteOrder.nativeOrder()).asIntBuffer();
        int mask = 0; // Read all messages
        ByteBuffer timeStamp = ByteBuffer.allocateDirect(4);

        while (running && !socket.isClosed() && !Thread.currentThread().isInterrupted()) {
            if (RobotBase.isReal()) {
                try {
                    byte[] data = CANJNI.FRCNetCommCANSessionMuxReceiveMessage(idBuffer, mask, timeStamp);
                    if (data != null && clientAddress != null && clientPort != -1) {
                        int msgId = idBuffer.get(0);
                        
                        // Format: 4 byte ID, 1 byte length, up to 8 byte data
                        ByteBuffer outBuffer = ByteBuffer.allocate(5 + data.length);
                        outBuffer.putInt(msgId);
                        outBuffer.put((byte)(data.length & 0xFF));
                        outBuffer.put(data);

                        byte[] packetData = outBuffer.array();
                        DatagramPacket sendPacket = new DatagramPacket(packetData, packetData.length, clientAddress, clientPort);
                        socket.send(sendPacket);
                    }
                } catch (RuntimeException e) {
                    if (e.getClass().getSimpleName().contains("NotFound")) {
                        // Normal timeout/empty queue. Sleep briefly to yield CPU
                        try {
                            Thread.sleep(10);
                        } catch (InterruptedException ex) {
                            break;
                        }
                    } else {
                        // Other error
                        e.printStackTrace();
                        try {
                            Thread.sleep(100);
                        } catch (InterruptedException ex) {
                            break;
                        }
                    }
                } catch (Exception e) {
                    // Other generic checked exceptions if any
                    e.printStackTrace();
                    try {
                        Thread.sleep(100);
                    } catch (InterruptedException ex) {
                        break;
                    }
                }
            } else {
                // Simulation
                try {
                    Thread.sleep(100); // Poll slower in sim to avoid spinning
                } catch (InterruptedException e) {
                    break;
                }
            }
        }
    }
}
