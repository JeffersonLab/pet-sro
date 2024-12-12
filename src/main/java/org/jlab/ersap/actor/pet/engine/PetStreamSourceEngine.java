package org.jlab.ersap.actor.pet.engine;
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

import org.jlab.epsci.ersap.engine.EngineDataType;
import org.jlab.epsci.ersap.std.services.AbstractEventReaderService;
import org.jlab.epsci.ersap.std.services.EventReaderException;
import org.jlab.ersap.actor.datatypes.JavaObjectType;
import org.jlab.ersap.actor.pet.source.PetStreamReceiver;
import org.jlab.ersap.actor.util.EConstants;
import org.json.JSONObject;

import java.nio.ByteOrder;
import java.nio.file.Path;

public class PetStreamSourceEngine extends AbstractEventReaderService<PetStreamReceiver> {
    @Override
    protected PetStreamReceiver createReader(Path path, JSONObject jsonObject) throws EventReaderException {
        //  Default parameters to construct PetStreamReceiver object
        String streamHost = EConstants.udf;
        int streamPort = 7777;
        int ringBufferSize = 1024; // Must be power of 2s
        int connectionTimeout = 5000; // 5 seconds
        int readTimeout = 2000; // 2 seconds

        // Get parameters from the ERSAP YAML configuration file
        if (jsonObject.has("streamHost")) {
            streamHost = jsonObject.getString("streamHost");
        } else if (jsonObject.has("streamPort")) {
            streamPort = jsonObject.getInt("streamPort");
        } else if (jsonObject.has("ringBufferSize")) {
            ringBufferSize = jsonObject.getInt("ringBufferSize");
        } else if (jsonObject.has("connectionTimeout")) {
            connectionTimeout = jsonObject.getInt("connectionTimeout");
        } else if (jsonObject.has("readTimeout")) {
            readTimeout = jsonObject.getInt("readTimeout");
        }
        if (!streamHost.equals(EConstants.udf)) {
            return new PetStreamReceiver(streamHost,
                    streamPort,
                    ringBufferSize,
                    connectionTimeout,
                    readTimeout);
        } else {
            throw new EventReaderException("Stream host is undefined");
        }
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
