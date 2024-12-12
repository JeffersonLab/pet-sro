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

import org.jlab.epsci.ersap.base.ErsapUtil;
import org.jlab.epsci.ersap.engine.Engine;
import org.jlab.epsci.ersap.engine.EngineData;
import org.jlab.epsci.ersap.engine.EngineDataType;
import org.jlab.ersap.actor.datatypes.JavaObjectType;
import org.jlab.ersap.actor.pet.proc.PetGeometryProcessor;
import org.json.JSONObject;

import java.util.Set;

/**
 * This class represents a processing engine for PET
 * (Positron Emission Tomography) geometry data.
 * It implements the Engine interface.
 */
public class PetGeometryProcessorEngine implements Engine {
    private PetGeometryProcessor petGeometryProcessor;

    /**
     * Configures the engine with the provided EngineData settings.
     *
     * @param engineData The EngineData settings to configure the engine with.
     * @return The resulting EngineData after configuration.
     */
    @Override
    public EngineData configure(EngineData engineData) {
        if (engineData.getMimeType().equalsIgnoreCase(EngineDataType.JSON.mimeType())) {
            String source = (String) engineData.getData();
            JSONObject data = new JSONObject(source);
            // Use data to retrieve configuration parameters defined
            // in the ERSAP YAML file for the user engine: PetGeometryProcessor
            /* Action goes here... */

            // Use parameters to create the petGeometryProcessor object
            petGeometryProcessor = new PetGeometryProcessor();
        }
        return null;
    }

    /**
     * Executes the processing logic of the engine on the provided EngineData.
     *
     * @param engineData The EngineData to process.
     * @return The resulting EngineData after processing.
     */
    @Override
    public EngineData execute(EngineData engineData) {
        EngineData out = new EngineData();
        Object inData;
        try {
            inData = engineData.getData();
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
        Object outData = petGeometryProcessor.process(inData);
        out.setData(JavaObjectType.JOBJ, outData);
        return out;
    }

    @Override
    public EngineData executeGroup(Set<EngineData> set) {
        return null;
    }

    /**
     * Retrieves the set of data types that the engine can accept as input.
     *
     * @return A Set of EngineDataType representing possible input data types.
     */
    @Override
    public Set<EngineDataType> getInputDataTypes() {
        return ErsapUtil.buildDataTypes(JavaObjectType.JOBJ,
                EngineDataType.JSON);
    }

    /**
     * Retrieves the set of data types that the engine can provide as output.
     *
     * @return A Set of EngineDataType representing possible output data types.
     */
    @Override
    public Set<EngineDataType> getOutputDataTypes() {
        return ErsapUtil.buildDataTypes(JavaObjectType.JOBJ);
    }

    @Override
    public Set<String> getStates() {
        return null;
    }

    /**
     * Retrieves a description for the engine.
     *
     * @return A String description for the engine.
     */
    @Override
    public String getDescription() {
        return "Processes the geometry data for a PET " +
                "(Positron Emission Tomography) system";
    }

    /**
     * Retrieves the version number of the engine.
     *
     * @return A String representing the version number.
     */
    @Override
    public String getVersion() {
        return "v1.0";
    }

    /**
     * Retrieves the author of the engine.
     *
     * @return A String representing the author.
     */
    @Override
    public String getAuthor() {
        return "gurjyan";
    }

    /**
     * Resets the engine to its initial state.
     */
    @Override
    public void reset() {
        petGeometryProcessor.reset();
    }

    /**
     * Destroys the engine, performing cleanup and resource deallocation.
     */
    @Override
    public void destroy() {
        petGeometryProcessor.destruct();
    }
}
