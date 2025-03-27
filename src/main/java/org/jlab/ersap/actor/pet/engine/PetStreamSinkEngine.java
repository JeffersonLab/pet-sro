package org.jlab.ersap.actor.pet.engine;

import org.jlab.epsci.ersap.engine.EngineDataType;
import org.jlab.epsci.ersap.std.services.AbstractEventWriterService;
import org.jlab.epsci.ersap.std.services.EventWriterException;
import org.jlab.ersap.actor.datatypes.JavaObjectType;
import org.json.JSONObject;

import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Path;
import java.util.logging.Logger;

public class PetStreamSinkEngine extends AbstractEventWriterService<FileOutputStream> {
    private String fileName;
    boolean bFileOutput = false;

    @Override
    protected FileOutputStream createWriter(Path filePath, JSONObject options) throws EventWriterException {

        Logger.getGlobal().info("createWriter, path: " + filePath);

        if (options.has("fileOutput")) {
            if (options.getString("fileOutput").equalsIgnoreCase("true")) {
                bFileOutput = true;
            }
        }

        try {
            fileName = filePath.toString();
            return new FileOutputStream(fileName);
        } catch (IOException e) {
            throw new EventWriterException(e);
        }

    }

    @Override
    protected void closeWriter() {
        try {
            writer.flush();
            writer.close();
        } catch (IOException e) {
            e.printStackTrace();
        }
    }

    @Override
    protected void writeEvent(Object o) throws EventWriterException {
        if (bFileOutput) {

            Logger.getGlobal().info("Sink got event of type: " + o.getClass());
        }
    }

    @Override
    protected EngineDataType getDataType() {
        return JavaObjectType.JOBJ;
    }
}

