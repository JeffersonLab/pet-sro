package org.jlab.ersap.actor.pet.engine;

import org.jlab.epsci.ersap.base.ErsapUtil;
import org.jlab.epsci.ersap.engine.EngineDataType;
import org.jlab.epsci.ersap.std.services.AbstractEventWriterService;
import org.jlab.epsci.ersap.std.services.EventWriterException;
import org.jlab.ersap.actor.datatypes.EvioBlockDataType;
import org.jlab.ersap.actor.datatypes.JavaObjectType;
import org.json.JSONObject;

import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Path;
import java.util.Set;
import java.util.logging.Logger;

/**
 * Writes, or just logs, whatever the last stage of the chain produces.
 *
 * <p>{@code AbstractEventWriterService} admits exactly one input type: its
 * {@code execute()} refuses anything whose mime type is not equal to
 * {@link #getDataType()}. This project has two chains that end here and they do
 * not speak the same type -- the EJFAT chain carries EVIO blocks from the C++
 * {@code EjfatReceiverActor}, the simulator chain carries opaque byte arrays --
 * so the accepted type is a configuration choice rather than a constant.
 *
 * <p>Configuration, in the writer's block of the ERSAP YAML:
 * <pre>
 *   dataType   : "binary/data-evio" (default) or "binary/data-jobj"
 *   fileOutput : "true" to log each event's class
 * </pre>
 *
 * <p>{@link #getInputDataTypes()} declares both, so a payload of either type
 * always deserializes; {@code dataType} then decides which one this instance
 * will actually accept.
 */
public class PetStreamSinkEngine extends AbstractEventWriterService<FileOutputStream> {
    private String fileName;
    boolean bFileOutput = false;

    /**
     * The type this sink accepts. Read by the framework on every event, and
     * set once by {@link #createWriter}, which runs at configure time and so
     * always precedes the first event.
     */
    private EngineDataType dataType = EvioBlockDataType.EVIO_BLOCK;

    @Override
    protected FileOutputStream createWriter(Path filePath, JSONObject options) throws EventWriterException {

        Logger.getGlobal().info("createWriter, path: " + filePath);

        if (options.has("fileOutput")) {
            if (options.getString("fileOutput").equalsIgnoreCase("true")) {
                bFileOutput = true;
            }
        }

        if (options.has("dataType")) {
            String requested = options.getString("dataType");
            if (requested.equalsIgnoreCase(EvioBlockDataType.MIME_TYPE)) {
                dataType = EvioBlockDataType.EVIO_BLOCK;
            } else if (requested.equalsIgnoreCase(JavaObjectType.JOBJ.mimeType())) {
                dataType = JavaObjectType.JOBJ;
            } else {
                throw new EventWriterException("unsupported dataType '" + requested
                        + "'; expected " + EvioBlockDataType.MIME_TYPE + " or "
                        + JavaObjectType.JOBJ.mimeType());
            }
        }
        Logger.getGlobal().info("accepting data type: " + dataType.mimeType());

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
        return dataType;
    }

    /**
     * Both types this sink can be configured for, so either deserializes
     * regardless of which one {@code dataType} selects. The framework's own
     * implementation would declare only {@link #getDataType()}, which would
     * make a reconfiguration between the two impossible.
     */
    @Override
    public Set<EngineDataType> getInputDataTypes() {
        return ErsapUtil.buildDataTypes(EvioBlockDataType.EVIO_BLOCK,
                JavaObjectType.JOBJ,
                EngineDataType.JSON);
    }
}

