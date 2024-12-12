package org.jlab.ersap.actor.util;
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
import java.nio.ByteOrder;

public interface IESource {

    /**
     * Retrieves the next event from the source data ring (ringBuffer)
     *
     * @return an Object representing the next event. The specifics are
     * defined by the class implementing this interface.
     */
    public Object nextEvent();

    /**
     * Retrieves the byte order used to read or write data to the source.
     *
     * @return a ByteOrder object indicating the byte order used by the source.
     */
    public ByteOrder getByteOrder();

    /**
     * Closes the connection to the resource or source.
     * It should perform any necessary cleanup and resource deallocation.
     */
    public void close();
}
