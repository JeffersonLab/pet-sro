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


public interface IEProcessor {

    /**
     * Processes an incoming object.
     *
     * @param o The object to be processed.
     *
     * @return An object as a result of the processing. Specifics are
     * defined by the class implementing this interface.
     */
    public Object process(Object o);

    /**
     * Resets the state of the processor.
     * It should prepare the processor for a new round of processing.
     */
    public void reset();

    /**
     * Destructs or shuts down the processor.
     * It should perform any necessary cleanup and resource deallocation.
     */
    public void destruct();

}
