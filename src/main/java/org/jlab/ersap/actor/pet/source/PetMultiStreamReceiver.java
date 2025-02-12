package org.jlab.ersap.actor.pet.source;

import org.jlab.ersap.actor.util.IESource;

import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.*;

public class PetMultiStreamReceiver implements IESource {
    private ExecutorService executorService;
    private int numStreamReceivers;
    private List<PetStreamReceiver> streamReceivers;

    public PetMultiStreamReceiver(StreamParameters[] ps) {
        numStreamReceivers = ps.length;
        streamReceivers = new ArrayList<>();
        for (int i = 0; i < numStreamReceivers; i++) {
            PetStreamReceiver streamReceiver = new PetStreamReceiver(ps[i]);
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
        return null;
    }

    @Override
    public void close() {

    }
}
