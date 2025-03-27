package org.jlab.ersap.actor.pet.engine;

import org.jlab.epsci.ersap.engine.EngineDataType;
import org.jlab.epsci.ersap.std.services.AbstractEventReaderService;
import org.jlab.epsci.ersap.std.services.EventReaderException;
import org.jlab.ersap.actor.datatypes.JavaObjectType;
import org.jlab.ersap.actor.pet.source.simulator.PetMultiStreamSimulator;
import org.jlab.ersap.actor.pet.source.simulator.PetStreamSimulator;
import org.json.JSONObject;

import java.nio.ByteOrder;
import java.nio.file.Path;

public class SimPetMultiStreamSourceEngine extends AbstractEventReaderService<PetMultiStreamSimulator> {
    @Override
    protected PetMultiStreamSimulator createReader(Path path, JSONObject jsonObject) throws EventReaderException {

        int ringBufferSize = 1024;
        int dataSize = 100;
        int streamCount = 3;

        // Get parameters from the ERSAP YAML configuration file
        if (jsonObject.has("ringBufferSize")) {
            ringBufferSize = jsonObject.getInt("ringBufferSize");
        }
        if (jsonObject.has("dataSize")) {
            dataSize = jsonObject.getInt("dataSize");
        }
        if (jsonObject.has("streamCount")) {
            streamCount = jsonObject.getInt("streamCount");
        }
        return new PetMultiStreamSimulator(ringBufferSize,dataSize, streamCount);

    }
    @Override
    protected void closeReader() {
        reader.close();
    }

    @Override
    protected int readEventCount() throws EventReaderException {
        return Integer.MAX_VALUE;
    }

    @Override
    protected ByteOrder readByteOrder() throws EventReaderException {
        return reader.getByteOrder();
    }

    @Override
    protected Object readEvent(int i) throws EventReaderException {
        return reader.nextEvent();
    }

    @Override
    protected EngineDataType getDataType() {
        return JavaObjectType.JOBJ;
    }
}