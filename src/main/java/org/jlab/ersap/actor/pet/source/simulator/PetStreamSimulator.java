package org.jlab.ersap.actor.pet.source.simulator;

import com.lmax.disruptor.dsl.Disruptor;
import org.jlab.ersap.actor.pet.source.Event;
import org.jlab.ersap.actor.util.IESource;

import java.nio.ByteOrder;
import java.util.concurrent.Callable;

public class PetStreamSimulator implements IESource, Callable<String> {

    private ByteArrayGenerator handler;

    public PetStreamSimulator (int ringBufferSize, int dataSize) {
        Disruptor<Event> disruptor = new Disruptor<>(Event::new, ringBufferSize, Runnable::run);
        disruptor.start();

        handler = new ByteArrayGenerator(
                disruptor.getRingBuffer(),
                ByteOrder.BIG_ENDIAN,
                dataSize
        );

        try {
            handler.generateAndPublish();
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
    public void close() {}

    @Override
    public String call() throws Exception {
        return null;
    }
}