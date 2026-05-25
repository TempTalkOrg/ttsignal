# libsignal

High-performance messaging module over QUIC/WebSocket/WSS with multiple network protocols. JNI interface module supporting 10K+ QUIC-V1 concurrent connections, Server/Connector cluster networking, WebSocket/WSS protocol, connection Group creation and efficient group messaging.

## Dependencies

### 1. Operating System
- Windows 7/10/11
- CentOS 6.5 or later
- Ubuntu 16.04 or later

### 2. Build Environment
- OpenJDK 1.8+
- GCC 7.0+ (C++17 support)
- CMake 3.15+

## Build Steps

### 1. Run from project root

- android/scripts/build-so.sh to build the JNI module

## Usage

* 1. Build the JNI module;
* 2. Create Server/Connector following the Java sample code and configure callback interfaces appropriately;
* 3. Call Server.start() to accept new connections or Connector.createConnection() to create and initiate a connection;
* 4. Call conn.sendPacket() to send data;
* 5. Call conn.close() to close the connection.

## Notes
* 1. The JNI layer uses multithreading; callbacks may be invoked from multiple threads. Ensure thread safety on the Java side;
* 2. Do not use the connection object after calling conn.close() to close the connection.
