/*
 * @Description: Arduino_DriveBus.cpp
 * @Author: LILYGO_L
 * @Date: 2023-11-16 15:53:46
 * @LastEditTime: 2024-12-09 10:23:52
 * @License: GPL 3.0
 */
#include "Arduino_DriveBus.h"

Arduino_IIC_DriveBus::Arduino_IIC_DriveBus() {}

bool Arduino_IIC_DriveBus::WriteC8D8(uint8_t c, uint8_t d)
{
    if (Write(c) == false)
    {
        log_w("->Write(c) fail");
        return false;
    }
    if (Write(d) == false)
    {
        log_w("->Write(d) fail");
        return false;
    }
    return true;
}

bool Arduino_IIC_DriveBus::BufferOperation(uint8_t device_address, const uint8_t *operations, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        switch (operations[i])
        {
        case Arduino_DriveBus_BufferOperation::BO_BEGIN_TRANSMISSION:
            BeginTransmission(device_address);
            break;
        case Arduino_DriveBus_BufferOperation::BO_WRITE:
            i++;
            if (Write(operations[i]) == false)
            {
                log_w("->Write(operations[i]) fail");
                return false;
            }
            break;
        case Arduino_DriveBus_BufferOperation::BO_WRITE_C8_D8:
            i++;
            if (Write(operations[i]) == false)
            {
                log_w("->Write(operations[i]) fail");
                return false;
            }
            i++;
            if (Write(operations[i]) == false)
            {
                log_w("->Write(operations[i]) fail");
                return false;
            }
            break;
        case Arduino_DriveBus_BufferOperation::BO_END_TRANSMISSION:
            if (EndTransmission() == false)
            {
                log_w("->EndTransmission() fail");
                return false;
            }
            break;
        case Arduino_DriveBus_BufferOperation::BO_DELAY:
            i++;
            delay(operations[i]);
            break;
        default:
            return false;
            break;
        }
    }

    return true;
}

bool Arduino_IIC_DriveBus::IIC_Device_7Bit_Scan(std::vector<unsigned char> *device_address)
{
    std::vector<unsigned char> device_address_temp; // 地址存储器

    for (uint8_t i = 1; i < 128; i++)
    {
        BeginTransmission(i);
        if (EndTransmission() == true)
        {
            device_address_temp.push_back(i);
        }
    }

    if (device_address_temp.empty() == false)
    {
        device_address->assign(device_address_temp.begin(), device_address_temp.end());
        return true;
    }
    return false;
}

bool Arduino_IIC_DriveBus::IIC_Write_Data(uint8_t device_address, const uint8_t *data, size_t length)
{
    BeginTransmission(device_address);
    if (Write(data, length) == false)
    {
        log_w("->Write(data, length) fail");
        return false;
    }
    if (EndTransmission() == false)
    {
        log_w("->EndTransmission() fail");
        return false;
    }
    return true;
}

bool Arduino_IIC_DriveBus::IIC_WriteC8_Data(uint8_t device_address, uint8_t c, const uint8_t *data, size_t length)
{
    BeginTransmission(device_address);
    if (Write(c) == false)
    {
        log_w("->Write(c) fail");
        return false;
    }
    if (Write(data, length) == false)
    {
        log_w("->Write(data, length) fail");
        return false;
    }
    if (EndTransmission() == false)
    {
        log_w("->EndTransmission() fail");
        return false;
    }
    return true;
}

bool Arduino_IIC_DriveBus::IIC_WriteC16_Data(uint8_t device_address, uint16_t c, const uint8_t *data, size_t length)
{
    BeginTransmission(device_address);
    if (Write((uint8_t)(c >> 8)) == false)
    {
        log_w("->Write((uint8_t)(c >> 8)) fail");
        return false;
    }
    if (Write((uint8_t)c) == false)
    {
        log_w("->Write((uint8_t)c) fail");
        return false;
    }
    if (Write(data, length) == false)
    {
        log_w("->Write(data, length) fail");
        return false;
    }
    if (EndTransmission() == false)
    {
        log_w("->EndTransmission() fail");
        return false;
    }
    return true;
}

bool Arduino_IIC_DriveBus::IIC_WriteC8D8(uint8_t device_address, uint8_t c, uint8_t d)
{
    BeginTransmission(device_address);
    if (WriteC8D8(c, d) == false)
    {
        log_w("->WriteC8D8(c, d) fail");
        return false;
    }
    if (EndTransmission() == false)
    {
        log_w("->EndTransmission() fail");
        return false;
    }
    return true;
}

bool Arduino_IIC_DriveBus::IIC_WriteC16D8(uint8_t device_address, uint16_t c, uint8_t d)
{
    BeginTransmission(device_address);
    if (Write((uint8_t)(c >> 8)) == false)
    {
        log_w("->Write((uint8_t)(c >> 8)) fail");
        return false;
    }
    if (Write((uint8_t)c) == false)
    {
        log_w("->Write((uint8_t)c) fail");
        return false;
    }
    if (EndTransmission() == false)
    {
        log_w("->EndTransmission() fail");
        return false;
    }
    return true;
}

bool Arduino_IIC_DriveBus::IIC_ReadC8_Data(uint8_t device_address, uint8_t c, uint8_t *d, size_t length)
{
    BeginTransmission(device_address);
    if (Write(c) == false)
    {
        log_w("->Write(c) fail");
        return false;
    }
    if (EndTransmission() == false)
    {
        log_w("->EndTransmission() fail");
        return false;
    }
    if (RequestFrom(device_address, length) == false)
    {
        log_w("->RequestFrom(device_address, length) fail");
        return false;
    }
    for (size_t i = 0; i < length; i++)
    {
        d[i] = Read();
    }

    return true;
}

bool Arduino_IIC_DriveBus::IIC_ReadC16_Data(uint8_t device_address, uint16_t c, uint8_t *d, size_t length)
{
    BeginTransmission(device_address);
    if (Write((uint8_t)(c >> 8)) == false)
    {
        log_w("->Write((uint8_t)(c >> 8)) fail");
        return false;
    }
    if (Write((uint8_t)c) == false)
    {
        log_w("->Write((uint8_t)c) fail");
        return false;
    }
    if (EndTransmission() == false)
    {
        log_w("->EndTransmission() fail");
        return false;
    }
    if (RequestFrom(device_address, length) == false)
    {
        log_w("->RequestFrom(device_address, length) fail");
        return false;
    }
    for (size_t i = 0; i < length; i++)
    {
        d[i] = Read();
    }

    return true;
}

bool Arduino_IIC_DriveBus::IIC_ReadC8_Delay_Data(uint8_t device_address, uint8_t c, uint32_t delay_ms, uint8_t *d, size_t length)
{
    BeginTransmission(device_address);
    if (Write(c) == false)
    {
        log_w("->Write(c) fail");
        return false;
    }
    if (EndTransmission() == false)
    {
        log_w("->EndTransmission() fail");
        return false;
    }
    delay(delay_ms);
    if (RequestFrom(device_address, length) == false)
    {
        log_w("->RequestFrom(device_address, length) fail");
        return false;
    }
    for (size_t i = 0; i < length; i++)
    {
        d[i] = Read();
    }

    return true;
}

bool Arduino_IIC_DriveBus::IIC_ReadC8D8(uint8_t device_address, uint8_t c, uint8_t *d)
{
    BeginTransmission(device_address);
    if (Write(c) == false)
    {
        log_w("->Write(c) fail");
        return false;
    }
    if (EndTransmission() == false)
    {
        log_w("->EndTransmission() fail");
        return false;
    }
    if (RequestFrom(device_address, 1) == false)
    {
        log_w("->RequestFrom(device_address, 1) fail");
        return false;
    }
    *d = Read();

    return true;
}

bool Arduino_IIC_DriveBus::IIC_ReadC16D8(uint8_t device_address, uint16_t c, uint8_t *d)
{
    BeginTransmission(device_address);
    if (Write((uint8_t)(c >> 8)) == false)
    {
        log_w("->Write((uint8_t)(c >> 8)) fail");
        return false;
    }
    if (Write((uint8_t)c) == false)
    {
        log_w("->Write((uint8_t)c) fail");
        return false;
    }
    if (EndTransmission() == false)
    {
        log_w("->EndTransmission() fail");
        return false;
    }
    if (RequestFrom(device_address, 1) == false)
    {
        log_w("->RequestFrom(device_address, 1) fail");
        return false;
    }
    *d = Read();

    return true;
}

Arduino_IIS_DriveBus::Arduino_IIS_DriveBus() {}

bool Arduino_IIS_DriveBus::IIS_Read(void *data, size_t bytes)
{
    size_t temp_buf = Read(data, bytes);

    if (temp_buf < bytes)
    {
        log_w("->Read(data, bytes) fail");
        return false;
    }
    return true;
}

bool Arduino_IIS_DriveBus::IIS_Write(const void *data, size_t bytes)
{
    size_t temp_buf = Write(data, bytes);

    if (temp_buf < bytes)
    {
        log_w("->Write(data, bytes) fail");
        return false;
    }
    return true;
}