#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <semaphore.h>

class ShmemWriter {
public:
    // Maximum seconds write_data will wait for the reader to free the buffer.
    static constexpr int WRITE_TIMEOUT_SEC = 5;

    ShmemWriter(const std::string& shmem_name, size_t size,
                const std::string& sem_name, const std::string& sem_ack_name);
    ~ShmemWriter();

    bool initialize();
    bool write_data(const std::vector<double>& data, uint32_t ndim, const std::vector<uint32_t>& dims, uint16_t data_id = 0);
    void cleanup();

private:
    std::string shmem_name_;
    std::string sem_name_;
    std::string sem_ack_name_;
    size_t shmem_size_;
    int fd_;
    void* ptr_;
    sem_t* sem_;
    sem_t* sem_ack_;
    bool initialized_;
};
