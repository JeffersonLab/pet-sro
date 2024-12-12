package org.jlab.ersap.actor.pet.source;
/**
 * Copyright (c) 2021, Jefferson Science Associates, all rights reserved.
 * See LICENSE.txt file.
 * Thomas Jefferson National Accelerator Facility
 * Experimental Physics Software and Computing Infrastructure Group
 * 12000, Jefferson Ave, Newport News, VA 23606
 * Phone : (757)-269-7100
 *
 * @author gurjyan on 12/10/24
 * @project pet-sro
 */

import com.lmax.disruptor.dsl.Disruptor;
import org.jlab.ersap.actor.util.IESource;

import java.net.Socket;
import java.nio.ByteOrder;

public class PetStreamReceiver implements IESource {

    private SocketConnectionHandler handler;
    private Socket connection;

    public PetStreamReceiver (String streamHost,
                             int streamPort,
                             int ringBufferSize,
                             int connectionTimeout,
                             int readTimeout) {
        Disruptor<Event> disruptor = new Disruptor<>(Event::new, ringBufferSize, Runnable::run);
        disruptor.start();

        handler = new SocketConnectionHandler(
                disruptor.getRingBuffer(),
                streamHost, streamPort,
                ByteOrder.BIG_ENDIAN,
                connectionTimeout,
                readTimeout
        );

        try {
            connection = handler.establishConnection();
            handler.listenAndPublish(connection);
        } catch (IllegalStateException e) {
            System.err.println("Failed to establish connection: " + e.getMessage());
        }
    }


    @Override
    public Object nextEvent() {
        // This section is designated for the implementation of
        // a potential supplementary singles-finding algorithm.
       return  handler.getNextEvent();
    }

    @Override
    public ByteOrder getByteOrder() {
        return handler.getByteOrder();
    }

    @Override
    public void close() {
       handler.closeConnection(connection);
    }
}

