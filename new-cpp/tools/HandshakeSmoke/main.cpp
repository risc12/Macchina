//
//  main.cpp
//  Macchina — new-cpp/tools/HandshakeSmoke
//
//  Smoke test for the C++ core: run the full v3 handshake against the real
//  NIHardwareAgent, then prove the controller connection with a
//  GetSerialNumber round-trip. No focus claim, no draws, no LEDs — this
//  deliberately stops at layer 2 and leaves the device state untouched.
//
//  Usage:  HandshakeSmoke [serial]          (default 39195855)
//  Needs NIHardwareAgent RUNNING and a Studio plugged in.
//  Exit code 0 = handshake + serial readback OK.
//

#include "core/client/AgentConnection.hpp"
#include "core/transport-macos/CFMessagePortTransport.hpp"
#include "core/transport/HexDump.hpp"

#include <cstdio>

using namespace macchina;
using namespace macchina::nhl2;

// The smoke test is Studio-specific (a tool, not core/): USB product id of the
// Maschine Studio and the known serial of the dev unit.
static const uint32_t kStudioProductId = 0x1300;
static const char *   kDefaultSerial   = "39195855";

int main(int argc, const char * argv[])
{
    std::string serial = (argc > 1) ? argv[1] : kDefaultSerial;

    CFMessagePortTransport transport;

    auto conn = AgentConnection::open(transport, "NIHWMainHandler");
    if (conn == nullptr)
    {
        fprintf(stderr, "FAIL: NIHWMainHandler not found — is NIHardwareAgent running?\n");
        return 1;
    }

    fprintf(stderr, "== 1. negotiate version\n");
    if (!conn->negotiateVersion())
    {
        fprintf(stderr, "FAIL: version negotiation\n");
        return 1;
    }

    fprintf(stderr, "== 2. subscription connection (Du)\n");
    auto sub = conn->subscribe(kStudioProductId, kClientTagMaschine2, kClientRolePrimary);
    auto subEndpoints = conn->openEndpoints(sub, "subscription");
    if (subEndpoints)
    {
        conn->registerNotificationPort(*subEndpoints);
        conn->sendSubscribeVoidOp(*subEndpoints);
    }
    else
        fprintf(stderr, "warn: subscription connect failed (continuing — the "
                "controller connection is the real test)\n");

    fprintf(stderr, "== 3. controller connection (DeviceConnect, serial %s)\n", serial.c_str());
    auto dev = conn->connectDevice(kStudioProductId, kClientTagMaschine2,
                                   kClientRolePrimary, serial);
    auto controller = conn->openEndpoints(dev, "controller");
    if (controller == nullptr)
    {
        fprintf(stderr, "FAIL: controller connect refused (wrong serial? device off?)\n");
        return 1;
    }
    conn->registerNotificationPort(*controller);
    conn->sendSubscribeVoidOp(*controller);

    // Let the receive port process the async MsgConnectionEstablished
    // completion (mirrors the proven ordering — LEARNINGS.md E17).
    transport.runFor(0.3);

    fprintf(stderr, "== 4. GetSerialNumber round-trip on the controller port\n");
    auto reply = conn->sendTag(kTagGetSerialNumber, {}, *controller->request);
    if (!reply)
    {
        fprintf(stderr, "FAIL: no GetSerialNumber reply\n");
        return 1;
    }
    auto readBack = parseLengthPrefixedString(*reply);
    if (!readBack)
    {
        fprintf(stderr, "FAIL: unparseable GetSerialNumber reply:\n%s", hexDump(*reply).c_str());
        return 1;
    }

    fprintf(stderr, "== 5. GetFirmwareVersion (informational)\n");
    auto fw = conn->sendTag(kTagGetFirmwareVersion, {}, *controller->request);
    if (fw && readU32(*fw, 0))
        fprintf(stderr, "    firmware version: 0x%02x\n", *readU32(*fw, 0));

    bool match = (*readBack == serial);
    fprintf(stderr, "\n%s: handshake complete, device serial reads back '%s'%s\n",
            match ? "PASS" : "FAIL",
            readBack->c_str(),
            match ? "" : " (does not match requested serial)");
    return match ? 0 : 1;
}
