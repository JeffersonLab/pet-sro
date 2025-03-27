package org.jlab.ersap.actor.pet.source.simulator;

import org.jlab.ersap.actor.util.IESource;

import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;

public class PetMultiStreamSimulator implements IESource {
    private int numStreamReceivers;
    private List<PetStreamSimulator> streamReceivers;

    public PetMultiStreamSimulator(int ringBufferSize, int dataSize, int numStreamReceivers) {
        this.numStreamReceivers = numStreamReceivers;
        streamReceivers = new ArrayList<>();
        for (int i = 0; i < numStreamReceivers; i++) {
            PetStreamSimulator streamReceiver = new PetStreamSimulator(ringBufferSize, dataSize);
            streamReceivers.add(streamReceiver);
        }
    }

    @Override
    public Object nextEvent() {
        Object[] res = new Object[numStreamReceivers];
        // Simple object aggregation
        for (int i = 0; i < numStreamReceivers; i++) {
            res[i] = streamReceivers.get(i).nextEvent();
        }
        return res;
    }

    @Override
    public ByteOrder getByteOrder() {
        return ByteOrder.BIG_ENDIAN;
    }

    @Override
    public void close() {
    }
}