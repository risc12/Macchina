//
//  CFMessagePortTransport.cpp
//  Macchina — new-cpp/core/transport-macos
//
//  Port of new/core/NIRawPort.m (NIRawPortClient / NIRawPortServer).
//

#include "core/transport-macos/CFMessagePortTransport.hpp"
#include "core/transport/HexDump.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <cstring>

namespace macchina {

namespace {

// Minimal RAII for CF objects; replaces ARC.
template <typename T>
struct CFPtr
{
    T ref = nullptr;

    CFPtr() = default;
    explicit CFPtr(T r) : ref(r) {}
    ~CFPtr() { if (ref) CFRelease(ref); }

    CFPtr(const CFPtr &) = delete;
    CFPtr & operator=(const CFPtr &) = delete;

    explicit operator bool() const { return ref != nullptr; }
};

CFPtr<CFStringRef> MakeCFString(const std::string & s)
{
    return CFPtr<CFStringRef>(CFStringCreateWithBytes(kCFAllocatorDefault,
                                                      (const UInt8 *)s.data(),
                                                      (CFIndex)s.size(),
                                                      kCFStringEncodingUTF8,
                                                      false));
}

Frame FrameFromCFData(CFDataRef data)
{
    if (data == nullptr)
        return {};
    const UInt8 * bytes = CFDataGetBytePtr(data);
    return Frame(bytes, bytes + CFDataGetLength(data));
}


class CFRequestPort : public RequestPort
{
public:
    CFRequestPort(CFMessagePortRef port, std::string name)
        : port_(port), name_(std::move(name)) {}

    ~CFRequestPort() override
    {
        if (port_.ref)
            CFMessagePortInvalidate(port_.ref);
    }

    const std::string & name() const override { return name_; }

    std::optional<Frame> request(const Frame & frame) override
    {
        return send(frame, /*wantReply=*/true);
    }

    void post(const Frame & frame) override
    {
        send(frame, /*wantReply=*/false);
    }

private:
    std::optional<Frame> send(const Frame & frame, bool wantReply)
    {
        if (frame.size() < 4)
        {
            fprintf(stderr, "%s !! refusing to send %zu-byte frame (no messageId)\n",
                    name_.c_str(), frame.size());
            return std::nullopt;
        }

        SInt32 msgid;
        memcpy(&msgid, frame.data(), sizeof(msgid));

        if (!quiet)
            fprintf(stderr, "%s %s %s\n", name_.c_str(), wantReply ? "->" : "~>",
                    describeFrameBounded(frame).c_str());

        CFPtr<CFDataRef> payload(CFDataCreate(kCFAllocatorDefault,
                                              frame.data(), (CFIndex)frame.size()));

        // replyMode NULL + no return-data pointer → send without waiting.
        CFDataRef replyData = nullptr;
        SInt32    err       = CFMessagePortSendRequest(port_.ref,
                                                       msgid,
                                                       payload.ref,
                                                       2.0,
                                                       wantReply ? 2.0 : 0.0,
                                                       wantReply ? kCFRunLoopDefaultMode : nullptr,
                                                       wantReply ? &replyData : nullptr);
        CFPtr<CFDataRef> reply(replyData);

        if (err != kCFMessagePortSuccess)
            fprintf(stderr, "%s !! CFMessagePortSendRequest error %d\n", name_.c_str(), (int)err);

        if (!wantReply)
            return std::nullopt;
        if (err != kCFMessagePortSuccess || replyData == nullptr)
        {
            if (!quiet)
                fprintf(stderr, "%s <- (no reply)\n\n", name_.c_str());
            return std::nullopt;
        }

        Frame result = FrameFromCFData(replyData);
        if (!quiet)
            fprintf(stderr, "%s <- %s\n\n", name_.c_str(),
                    result.empty() ? "(empty)" : hexDump(result).c_str());
        return result;
    }

    CFPtr<CFMessagePortRef> port_;
    std::string             name_;
};


class CFReceivePort : public ReceivePort
{
public:
    static std::unique_ptr<CFReceivePort> create(const std::string & name, FrameHandler handler)
    {
        auto self = std::unique_ptr<CFReceivePort>(new CFReceivePort(name, std::move(handler)));

        CFMessagePortContext context;
        memset(&context, 0, sizeof(context));
        context.info = self.get();

        auto cfName = MakeCFString(name);
        self->port_.ref = CFMessagePortCreateLocal(kCFAllocatorDefault,
                                                   cfName.ref,
                                                   &CFReceivePort::callback,
                                                   &context,
                                                   nullptr);
        if (self->port_.ref == nullptr)
            return nullptr;

        self->source_.ref = CFMessagePortCreateRunLoopSource(kCFAllocatorDefault, self->port_.ref, 0);
        CFRunLoopAddSource(CFRunLoopGetCurrent(), self->source_.ref, kCFRunLoopCommonModes);
        return self;
    }

    ~CFReceivePort() override
    {
        if (source_.ref)
            CFRunLoopSourceInvalidate(source_.ref);
        if (port_.ref)
            CFMessagePortInvalidate(port_.ref);
    }

    const std::string & name() const override { return name_; }

private:
    CFReceivePort(std::string name, FrameHandler handler)
        : name_(std::move(name)), handler_(std::move(handler)) {}

    static CFDataRef callback(CFMessagePortRef, SInt32, CFDataRef data, void * info)
    {
        auto * self  = static_cast<CFReceivePort *>(info);
        Frame  frame = FrameFromCFData(data);

        if (!self->quiet)
            fprintf(stderr, "%s <- %s\n", self->name_.c_str(),
                    describeFrameBounded(frame).c_str());

        std::optional<Frame> reply =
            self->handler_ ? self->handler_(frame) : std::nullopt;

        if (!self->quiet)
            fprintf(stderr, "%s -> %s\n\n", self->name_.c_str(),
                    !reply ? "(none)" : reply->empty() ? "(empty)" : hexDump(*reply).c_str());

        if (!reply)
            return nullptr;
        // CF takes ownership of the returned CFDataRef and releases it after sending.
        return CFDataCreate(kCFAllocatorDefault, reply->data(), (CFIndex)reply->size());
    }

    std::string             name_;
    FrameHandler            handler_;
    CFPtr<CFMessagePortRef> port_;
    CFPtr<CFRunLoopSourceRef> source_;
};

} // namespace


std::unique_ptr<RequestPort> CFMessagePortTransport::openRequestPort(const std::string & name)
{
    auto cfName = MakeCFString(name);
    CFMessagePortRef port = CFMessagePortCreateRemote(kCFAllocatorDefault, cfName.ref);
    if (port == nullptr)
        return nullptr;
    return std::make_unique<CFRequestPort>(port, name);
}

std::unique_ptr<ReceivePort> CFMessagePortTransport::createReceivePort(const std::string & name,
                                                                       FrameHandler handler)
{
    return CFReceivePort::create(name, std::move(handler));
}

void CFMessagePortTransport::run()
{
    CFRunLoopRun();
}

void CFMessagePortTransport::runFor(double seconds)
{
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
}

} // namespace macchina
