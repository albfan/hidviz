#include "libhidx/Interface.hh"

#include "libhidx/InterfaceHandle.hh"
#include "libhidx/Parser.hh"

#include <iostream>
#include <cassert>

namespace libhidx {

    Interface::Interface(const libusb_interface& interface, Device& device) : m_interface{interface.altsetting[0]}, m_device{device}, readingRuns{false}, stopReadingRequest{false} {
        for(unsigned i = 0; i < m_interface.bNumEndpoints; ++i){
            const auto& endpoint = m_interface.endpoint[i];
            bool isInterrupt =
                (endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_INTERRUPT;
            bool isInput =
                (endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;

            if(isInterrupt && isInput){
                m_inputAddress = endpoint.bEndpointAddress;
                m_inputMaxSize = endpoint.wMaxPacketSize;
                break;
            }
        }
    }

    Interface::~Interface() {
        stopReading();
    }

    bool Interface::isHid() const {
        return m_interface.bInterfaceClass == 3;
    }

    hid::Item* Interface::getHidReportDesc() {
        assert(isHid());

        if(m_hidReportDesc){
            return m_hidReportDesc.get();
        }

        constexpr uint16_t bufferLength = 1024;
        unsigned char buffer[bufferLength];

        auto handle = getHandle();
        auto size = handle->controlTransfer(
            0x81,
            LIBUSB_REQUEST_GET_DESCRIPTOR,
            ((LIBUSB_DT_REPORT << 8) | 0),
            m_interface.bInterfaceNumber,
            buffer,
            bufferLength,
            1000
        );
        if(size <= 0){
            //TODO: throw an exception
            return nullptr;
        }

        auto parser = Parser{buffer, static_cast<size_t>(size)};

        m_hidReportDesc.reset(parser.parse());

        return m_hidReportDesc.get();
    }

    std::string Interface::getName() const {
        auto name = std::string{};
        const auto devStrings = m_device.getStrings();

        name += devStrings.manufacturer;
        name += " ";
        name += devStrings.product;

        name += " (interface ";
        name += std::to_string(getNumber());
        name += ")";

        return name;
    }

    void Interface::beginReading() {
        if(readingRuns) {
            std::cerr << "fail begin reading" << std::endl;
            return;
        }
        if(readingThread.joinable()){
            readingThread.join();
        }

        readingRuns = true;
        std::thread t{&Interface::runner, this};

        readingThread = std::move(t);
    }

    void Interface::stopReading() {
        if(readingRuns) {
            stopReadingRequest = true;
            readingThread.join();
        }
    }

    void Interface::runner() {
        std::cerr << "thread running" << std::endl;

        auto handle = getHandle();

        std::vector<unsigned char> buffer;

        while(!stopReadingRequest){
            buffer.resize(m_inputMaxSize);
            int transferred;
            int size = handle->interruptTransfer(
                m_inputAddress,
                buffer.data(),
                m_inputMaxSize,
                &transferred,
                1000
            );
            buffer.resize(static_cast<size_t>(transferred));

            if(size == 0) {
                updateData(buffer);
                if(m_listener) {
                    m_listener();
                }
            } else if(size == LIBUSB_ERROR_TIMEOUT){
                // pass
            } else {
                std::cerr << "Interrupt transfer fail" << std::endl;
            }
        }

        stopReadingRequest = false;
        readingRuns = false;
    }

    std::shared_ptr<InterfaceHandle> Interface::getHandle() {
        static std::mutex mtx;
        std::lock_guard<std::mutex> lock{mtx};

        std::shared_ptr<InterfaceHandle> ptr;
        if(m_handle.expired()){
            ptr.reset(new InterfaceHandle{*this});
            m_handle = ptr;
        }

        return m_handle.lock();
    }

    void Interface::setReadingListener(std::function<void()> listener) {
        m_listener = listener;
    }

    void Interface::updateData(const std::vector<unsigned char>& data) {
        auto reportDesc = getHidReportDesc();

        reportDesc->forEach([&data](hid::Item* item){
            if(!item->m_control){
                return;
            }
            auto c = static_cast<hid::Control*>(item);
            c->update(data);
        });
    }


}
