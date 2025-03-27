package org.jlab.ersap.actor.pet.engine;

import org.jlab.epsci.ersap.engine.EngineDataType;
import org.jlab.epsci.ersap.std.services.AbstractEventReaderService;
import org.jlab.epsci.ersap.std.services.EventReaderException;
import org.jlab.ersap.actor.datatypes.JavaObjectType;
import org.jlab.ersap.actor.pet.source.simulator.PetStreamSimulator;
import org.json.JSONObject;

import java.nio.ByteOrder;
import java.nio.file.Path;

public class SimPetStreamSourceEngine extends AbstractEventReaderService<PetStreamSimulator> {
    @Override
    protected PetStreamSimulator createReader(Path path, JSONObject jsonObject) throws EventReaderException {

        int ringBufferSize = 1024;
        int dataSize = 100;

        // Get parameters from the ERSAP YAML configuration file
        if (jsonObject.has("ringBufferSize")) {
            ringBufferSize = jsonObject.getInt("ringBufferSize");
        }
        if (jsonObject.has("dataSize")) {
            dataSize = jsonObject.getInt("dataSize");
        }
        return new PetStreamSimulator(ringBufferSize,dataSize);

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

