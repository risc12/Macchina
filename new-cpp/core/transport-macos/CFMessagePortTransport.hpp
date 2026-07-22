//
//  CFMessagePortTransport.hpp
//  Macchina — new-cpp/core/transport-macos
//
//  The macOS implementation of Transport, over CFMessagePort (the IPC the NI
//  agents actually speak — see new/docs/protocol.md "Transport"). The ONLY
//  file pair in core/ that may include CoreFoundation.
//
//  Threading model (same as the ObjC original): single-threaded. Receive
//  ports are scheduled on the run loop of the thread that creates them, and
//  handlers fire while that thread sits in run()/runFor(). Note that
//  CFMessagePortSendRequest also services the default run-loop mode while
//  blocked waiting for a reply.
//

#pragma once

#include "core/transport/Transport.hpp"

namespace macchina {

class CFMessagePortTransport : public Transport
{
public:
    std::unique_ptr<RequestPort> openRequestPort(const std::string & name) override;
    std::unique_ptr<ReceivePort> createReceivePort(const std::string & name,
                                                   FrameHandler handler) override;
    void run() override;
    void runFor(double seconds) override;
};

} // namespace macchina
