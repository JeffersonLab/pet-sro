package org.jlab.ersap.actor.pet.source;

import org.jlab.ersap.actor.util.EConstants;

import java.nio.ByteOrder;

public class StreamParameters {
    private String host= EConstants.udf;
    private int port = 7777;
    private ByteOrder byteOrder;
    private int connectionTimeout = 5000; // Connection timeout in milliseconds.
    private int readTimeout = 2000;      // Read timeout in milliseconds.
    private int ringBufferSize = 1024;

    public String getHost() {
        return host;
    }

    public void setHost(String host) {
        this.host = host;
    }

    public int getPort() {
        return port;
    }

    public void setPort(int port) {
        this.port = port;
    }

    public ByteOrder getByteOrder() {
        return byteOrder;
    }

    public void setByteOrder(ByteOrder byteOrder) {
        this.byteOrder = byteOrder;
    }

    public int getConnectionTimeout() {
        return connectionTimeout;
    }

    public void setConnectionTimeout(int connectionTimeout) {
        this.connectionTimeout = connectionTimeout;
    }

    public int getReadTimeout() {
        return readTimeout;
    }

    public void setReadTimeout(int readTimeout) {
        this.readTimeout = readTimeout;
    }

    public int getRingBufferSize() {
        return ringBufferSize;
    }

    public void setRingBufferSize(int ringBufferSize) {
        this.ringBufferSize = ringBufferSize;
    }
}
