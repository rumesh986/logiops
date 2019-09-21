#include <hid/DeviceMonitor.h>
#include <hidpp/SimpleDispatcher.h>
#include <hidpp/Device.h>
#include <hidpp10/Error.h>
#include <hidpp10/IReceiver.h>
#include <hidpp20/Error.h>
#include <cstring>
#include <unistd.h>
#include <thread>
#include <fstream>
#include <sstream>
#include <chrono>

#include "DeviceFinder.h"
#include "util.h"
#include "Device.h"

#define MAX_CONNECTION_TRIES 10
#define TIME_BETWEEN_CONNECTION_TRIES 1s

void stopAndDeleteConnectedDevice (ConnectedDevice &connected_device)
{
    log_printf(INFO, "%s (Device %d on %s) disconnected", connected_device.device->name.c_str(), connected_device.device->index, connected_device.device->path.c_str()); 
    connected_device.device->stop();
    connected_device.associatedThread.join();
    delete(connected_device.device);
}

DeviceFinder::~DeviceFinder()
{
    this->devices_mutex.lock();
        for (auto it = this->devices.begin(); it != this->devices.end(); it++) {
            for (auto jt = it->second.begin(); jt != it->second.end(); jt++) {
                stopAndDeleteConnectedDevice(jt->second);
            }
        }
    this->devices_mutex.unlock();
}

void DeviceFinder::insertNewDevice(const std::string &path, HIDPP::DeviceIndex index)
{
    Device *device = new Device(path, index);

    this->devices_mutex.lock();
        log_printf(INFO, "%s detected: device %d on %s", device->name.c_str(), index, path.c_str());
        auto path_bucket = this->devices.emplace(path, std::map<HIDPP::DeviceIndex, ConnectedDevice>()).first;
        path_bucket->second.emplace(index, ConnectedDevice{
            device,
            std::thread([device]() {
                device->start();
            })
        });
    this->devices_mutex.unlock();
}

void DeviceFinder::stopAndDeleteAllDevicesIn (const std::string &path)
{
    this->devices_mutex.lock();
        auto path_bucket = this->devices.find(path);
        if (path_bucket != this->devices.end())
        {
            for (auto& index_bucket : path_bucket->second) {
                stopAndDeleteConnectedDevice(index_bucket.second);
            }
            this->devices.erase(path_bucket);
        }
    this->devices_mutex.unlock();

    log_printf(WARN, "Attempted to disconnect not previously connected devices on %s", path.c_str());
}

void DeviceFinder::stopAndDeleteDevice (const std::string &path, HIDPP::DeviceIndex index)
{
    this->devices_mutex.lock();
        auto path_bucket = this->devices.find(path);
        if (path_bucket != this->devices.end())
        {
            auto index_bucket = path_bucket->second.find(index);
            if (index_bucket != path_bucket->second.end())
            {
                stopAndDeleteConnectedDevice(index_bucket->second);
                path_bucket->second.erase(index_bucket);
            }
        }
    this->devices_mutex.unlock();

    log_printf(WARN, "Attempted to disconnect not previously connected device %d on %s", index, path.c_str());
}
    

void DeviceFinder::addDevice(const char *path)
{
    using namespace std::chrono_literals;

    std::string string_path(path);
    // Asynchronously scan device
    std::thread{[=]()
    {

        //Check if device is an HID++ device and handle it accordingly
        try
        {
            HIDPP::SimpleDispatcher dispatcher(string_path.c_str());
            bool has_receiver_index = false;
            for(HIDPP::DeviceIndex index: {
                HIDPP::DefaultDevice, HIDPP::CordedDevice,
                HIDPP::WirelessDevice1, HIDPP::WirelessDevice2,
                HIDPP::WirelessDevice3, HIDPP::WirelessDevice4,
                HIDPP::WirelessDevice5, HIDPP::WirelessDevice6})
            {

                if(!has_receiver_index && index == HIDPP::WirelessDevice1)
                    break;

                bool device_not_connected = true;
                bool device_unknown = false;
                int remaining_tries = MAX_CONNECTION_TRIES;
                do {
                    try
                    {
                        HIDPP::Device d(&dispatcher, index);
                        auto version = d.protocolVersion();
                        uint major, minor;
                        std::tie(major, minor) = version;
                        if(index == HIDPP::DefaultDevice && version == std::make_tuple(1, 0))
                            has_receiver_index = true;
                        if(major > 1) // HID++ 2.0 devices only
                        {
                            this->insertNewDevice(string_path, index);
                        }
                        device_not_connected = false;
                    }
                    catch(HIDPP10::Error &e)
                    {
                        if (e.errorCode() == HIDPP10::Error::ResourceError)
                        {
                            if(remaining_tries == 1)
                            {
                                log_printf(WARN, "While querying %s (possibly asleep), wireless device %d: %s", string_path.c_str(), index, e.what());
                                remaining_tries += MAX_CONNECTION_TRIES;  // asleep devices may raise a resource error, so do not count this try
                            } 
                        }
                        else if(e.errorCode() != HIDPP10::Error::UnknownDevice)
                        {
                            if(remaining_tries == 1)
                                log_printf(ERROR, "While querying %s, wireless device %d: %s", string_path.c_str(), index, e.what());
                        }
                        else device_unknown = true;
                    }
                    catch(HIDPP20::Error &e)
                    {
                        if(e.errorCode() != HIDPP20::Error::UnknownDevice)
                        {
                            if(remaining_tries == 1)
                                log_printf(ERROR, "Error while querying %s, device %d: %s", string_path.c_str(), index, e.what());
                        }
                        else device_unknown = true;
                    }
                    catch(HIDPP::Dispatcher::TimeoutError &e)
                    {
                        if(remaining_tries == 1)
                            log_printf(ERROR, "Device %s (index %d) timed out.", string_path.c_str(), index);
                    }
                    catch(std::runtime_error &e)
                    {
                        if(remaining_tries == 1)
                            log_printf(ERROR, "Runtime error on device %d on %s: %s", index, string_path.c_str(), e.what());
                    }

                    remaining_tries--;
                    std::this_thread::sleep_for(TIME_BETWEEN_CONNECTION_TRIES);

                } while (device_not_connected && !device_unknown && remaining_tries > 0);
            } 
        }
        catch(HIDPP::Dispatcher::NoHIDPPReportException &e) { }
        catch(std::system_error &e) { log_printf(WARN, "Failed to open %s: %s", string_path.c_str(), e.what()); }

    }}.detach();
}

void DeviceFinder::removeDevice(const char* path)
{
    this->stopAndDeleteAllDevicesIn(std::string(path));
}