package org.jlab.ersap.actor.pet.source.simulator;

import com.lmax.disruptor.*;
import org.jlab.ersap.actor.pet.source.Event;

import java.nio.ByteOrder;
import java.util.Arrays;

public class ByteArrayGenerator {

    private final int dataSize;
    private final ByteOrder byteOrder;
    protected final RingBuffer<Event> disruptorRingBuffer;
    private final Sequence sequence; // Tracks the consumer's progress
    private final SequenceBarrier barrier; // Ensures thread-safe access to the RingBuffer

    public ByteArrayGenerator(
            RingBuffer<Event> dRingBuffer,
            ByteOrder byteOrder,
            int dataSize) {
        this.byteOrder = byteOrder;
        this.dataSize = dataSize;
        this.disruptorRingBuffer = dRingBuffer;
        this.barrier = disruptorRingBuffer.newBarrier(); // Create a SequenceBarrier for safe access
        this.sequence = new Sequence(RingBuffer.INITIAL_CURSOR_VALUE); // Initialize consumer sequence
        disruptorRingBuffer.addGatingSequences(sequence); // Register this sequence for gating


    }

    public void generateAndPublish() {

        new Thread(() -> {
                while (!Thread.currentThread().isInterrupted()) {
                    byte[] data = new byte[dataSize];
                    Arrays.fill(data, (byte) 7);
                        publishEvent(data);
                }
        }).start();
    }

    public ByteOrder getByteOrder() {
        return byteOrder;
    }

    /**
     * Publishes the received data to the RingBuffer.
     *
     * @param data the received data to be published.
     */
    private void publishEvent(byte[] data) {
        long sequence = disruptorRingBuffer.next(); // Reserve next slot
        try {
            Event event = disruptorRingBuffer.get(sequence); // Get entry for sequence
            event.setData(data); // Set data in event
        } finally {
            disruptorRingBuffer.publish(sequence); // Publish event
        }
    }

    public Event getNextEvent() {
        try {
            // Retrieve the next available sequence for this consumer
            long nextSequence = sequence.get() + 1;

            // Wait until the sequence is available
            long availableSequence = barrier.waitFor(nextSequence);

            if (nextSequence <= availableSequence) {
                // Retrieve the event from the RingBuffer
                Event event = disruptorRingBuffer.get(nextSequence);

                // Update the consumer sequence after processing
                sequence.set(nextSequence);

                return event;
            }
        } catch (TimeoutException e) {
            System.err.println("Timeout while waiting for the next event.");
        } catch (AlertException e) {
            System.err.println("Alert received while waiting for the next event.");
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            System.err.println("Interrupted while waiting for the next event.");
        }

        return null; // No event available or an error occurred
    }

}
