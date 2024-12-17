# PET-SRO

This project focuses on the development of a modular and scalable 
Streaming Readout (SRO) Data Acquisition (DAQ) and analysis platform 
tailored for whole-body Positron Emission Tomography (PET) applications, 
leveraging the advanced capabilities of the ERSAP framework.

## PET ERSAP-Based Streaming Readout and Data Processing Actors

This repository provides a framework for implementing **ERSAP**-compliant data streaming applications, particularly suited for PET (Positron Emission Tomography) environments. The project leverages ERSAP to structure computation into **source**, **processor**, and **sink** actors, each represented by an engine. By composing these actors, users can build scalable, streaming data pipelines.

### Repository Structure

The codebase is organized under the `pet` directory and is divided into three primary subdirectories:

- **engine/**  
  Contains example implementations of ERSAP engines. These engines serve as building blocks (actors) within the ERSAP framework. They are specialized into two major categories:
    1. **Source Engine Example**: Demonstrates how to implement a socket-based data source actor that feeds streaming data into the pipeline.
    2. **Processor Engine Example**: Provides a template "geometry processor" engine, illustrating how computational logic can be integrated into the pipeline.

- **proc/**  
  Intended for user-defined processing algorithms. While the processor engines remain minimal for clarity, the complex logic is encapsulated in classes conforming to the `IEProcessor` interface, placed under the `util` directory. This design ensures the engine code remains maintainable and concise.

- **source/**  
  Contains the concrete implementations of source actors and their underlying connection-handling logic. Although "source" may suggest source code, here it specifically refers to "source actors" as defined by the ERSAP model.

### ERSAP Actor Model and Engine Interfaces

ERSAP categorizes actors into three types:

- **Source Actors**: Data producers that feed data into the pipeline.
- **Processor Actors**: Intermediate computation units that transform or analyze the incoming data stream.
- **Sink Actors**: Data consumers that handle output or storage operations.

### Engine Implementations

- **PetGeometryProcessorEngine**:  
  Implements the `Engine` interface, allowing ERSAP to present this engine as a processor actor. The actual geometry processing logic can be isolated within a class implementing `IEProcessor` to maintain clear separation between engine integration and computation logic.

- **petStreamSourceEngine**:  
  Extends `AbstractEventReaderService`, enabling ERSAP to treat it as a source actor. The `AbstractEventReaderService` is a parameterized abstract class that accepts any class implementing the `IESource` interface (found in `util/`) to encapsulate the source-specific data retrieval logic.

By structuring engines this way, developers can easily integrate complex data processing tasks into ERSAP-compliant pipelines without entangling user code with framework-specific details.

### Source Directory Details

The `source/` directory houses the logic and classes that define how data is sourced from external systems (e.g., sockets) and integrated into the ERSAP-driven pipeline.

### AbstractConnectionHandler Class

`AbstractConnectionHandler` is an abstract base class that outlines how to establish, manage, and receive data from a streaming source connection:

- **Abstract Methods**:
    - `establishConnection()`: Initiates the network connection to the data source.
    - `getByteOrder()`: Retrieves the configured byte order for interpreting received data.
    - `closeConnection()`: Gracefully closes the open network connection.
    - `receiveData(Object connection)`: Simulates reading incoming data (via a `DataInputStream`) and handles various I/O exceptions and end-of-stream conditions.

- **Concrete Methods**:
    - `listenAndPublish(Object connection)`: Initiates a dedicated thread that continuously listens for incoming data and publishes events to the LMAX Disruptor ring buffer.
    - `getNextEvent()`: Retrieves the next event produced by the disruptor ring, ensuring efficient data flow to downstream processing.

- **Thread-Safe Event Handling**:  
  Utilizes the Disruptor framework to guarantee thread-safe event publication and retrieval, enabling high-performance streaming pipelines.

- **Customizable Behavior**:  
  Subclasses must provide their own implementation of connection setup, byte order configuration, and data parsing, allowing full customization of how data is ingested into the pipeline.

### SocketConnectionHandler Class

`SocketConnectionHandler` is a concrete subclass of `AbstractConnectionHandler` that manages connections to a VTP socket server. It specifies how data is read and handled over a socket:

- **Core Methods**:
    - `establishConnection()`: Connects to the specified host and port with configurable timeouts, logging any errors and retrying as needed.
    - `receiveData()`: Interprets raw bytes from the socket’s input stream, validating and returning them as a byte array.
    - `closeConnection()`: Closes the underlying socket cleanly.

- **Byte Order Handling**:
  The byte order is set at initialization, ensuring correct interpretation of the stream’s binary data (e.g., `ByteOrder.BIG_ENDIAN`).

- **Error Handling & Robustness**:
    - **Connection Errors**: Catches `UnknownHostException` and `IOException` during connection setup, logging issues and retrying based on configurable parameters.
    - **Invalid Data**: Discards corrupted or empty data after validation, logging warnings as needed.
    - **End-of-Stream Detection**: Handles `-1` reads, logging and gracefully terminating the connection.
    - **General Exceptions**: Logs and handles unexpected `IOException` or `SocketException`, ensuring proper shutdown and allowing the system to recover.

- **Timeout Management**:
    - **Connection Timeout**: Uses configurable timeouts during connection attempts to avoid indefinite blocking.
    - **Read Timeout**: Enforces a read timeout so the system does not stall waiting for data indefinitely.  
      Logs meaningful errors and may raise exceptions when timeouts occur.

- **Broken Connection & Retry Logic**:
    - Implements a maximum number of retries and exponential backoff to re-establish connections after failures.
    - Alerts operators (via logs or extensible hooks) when retries exceed defined thresholds, aiding in monitoring and intervention.
    - Provides hooks for fallback strategies, such as attempting alternative hosts or notification systems.

**Metrics and Alerts**:
- Tracks retry attempts, triggers alerts at configured thresholds, and integrates seamlessly with potential external alerting systems (e.g., email, Slack).
- Offers an extensible architecture that can be augmented to handle evolving operational requirements.

### PetStreamReceiver

`PetStreamReceiver` instantiates `SocketConnectionHandler` or any other `AbstractConnectionHandler` implementation. It serves as a factory or convenience class to kick-start the data ingestion process from socket-based sources into the disruptor ring for further processing.

### Event

`Event` is a helper class employed by the LMAX Disruptor framework. It facilitates the storage and retrieval of data payloads as they travel through the pipeline, ensuring a lock-free, high-performance event processing mechanism.

---

**In summary**, this repository demonstrates extensible architecture for building PET-specific streaming pipelines under the ERSAP framework. 
It separates ERSAP engine definitions from user-specific data processing 
logic. By adhering to the interfaces and structures described above, 
developers can efficiently scale their processing capabilities, integrate fault-tolerance, and maintain high-throughput data ingestion 
and computation pipelines.


## Building
1. Define ERSAP_HOME and ERSAP_USER_DATA environmental variables

2. Clone and build ersap-java project from the Git repository


    $ clone https://github.com/JeffersonLab/ersap-java.git

    $ ./gradlew deploy

    $ ./gradlew publishToMavenLocal

3. Clone and build pet-sro project from the Git repository


    $ clone https://github.com/JeffersonLab/pet-sro.git
    
    $ ./gradlew deploy

