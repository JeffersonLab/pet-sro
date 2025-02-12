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
import org.jlab.ersap.actor.pet.source.PetMultiStreamReceiver;
import org.jlab.ersap.actor.pet.source.PetStreamReceiver;
import org.jlab.ersap.actor.pet.source.StreamParameters;
import org.jlab.ersap.actor.util.EConstants;
import org.json.JSONArray;
import org.json.JSONObject;

import java.nio.ByteOrder;
import java.nio.file.Path;

public class PetMultiStreamSourceEngine extends AbstractEventReaderService<PetMultiStreamReceiver> {
    @Override
    protected PetMultiStreamReceiver createReader(Path path, JSONObject jsonObject) throws EventReaderException {
        //  Default parameters to construct PetStreamReceiver object

        JSONArray modules = jsonObject.getJSONArray("modules");
        StreamParameters[] ps = new StreamParameters[modules.length()];

        for (int i = 0; i < modules.length(); i++) {
            JSONObject jsonObj = modules.getJSONObject(i);
            StreamParameters p = new StreamParameters();

            // Get parameters from the ERSAP YAML configuration file
            if (jsonObject.has("streamHost")) {
                p.setHost(jsonObject.getString("streamHost"));
            } else if (jsonObject.has("streamPort")) {
                p.setPort(jsonObject.getInt("streamPort"));
            } else if (jsonObject.has("ringBufferSize")) {
                p.setRingBufferSize(jsonObject.getInt("ringBufferSize"));
            } else if (jsonObject.has("connectionTimeout")) {
                p.setConnectionTimeout(jsonObject.getInt("connectionTimeout"));
            } else if (jsonObject.has("readTimeout")) {
                p.setReadTimeout(jsonObject.getInt("readTimeout"));
            }
            ps[i] = p;
        }
        return new PetMultiStreamReceiver(ps);
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
