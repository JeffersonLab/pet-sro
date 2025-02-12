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
import java.util.concurrent.Callable;

public class PetStreamReceiver implements IESource, Callable<String> {

    private SocketConnectionHandler handler;
    private Socket connection;

    public PetStreamReceiver (StreamParameters p) {
        Disruptor<Event> disruptor = new Disruptor<>(Event::new, p.getRingBufferSize(), Runnable::run);
        disruptor.start();

        handler = new SocketConnectionHandler(
                disruptor.getRingBuffer(),
                p.getHost(), p.getPort(),
                ByteOrder.BIG_ENDIAN,
                p.getConnectionTimeout(),
                p.getReadTimeout()
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

    @Override
    public String call() throws Exception {
        return null;
    }
}

