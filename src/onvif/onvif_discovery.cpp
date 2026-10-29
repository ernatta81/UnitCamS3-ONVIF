/**
 * @file onvif_discovery.cpp
 * @brief WS-Discovery (2005/04) responder so ONVIF clients can find the camera
 * @version 0.1
 * @date 2026-10-29
 *
 */
#include <Arduino.h>
#include <lwip/sockets.h>
#include <unistd.h>
#include "onvif.h"
#include "../net/net_manager.h"


#define WSD_MULTICAST_ADDR  "239.255.255.250"
#define WSD_PORT            3702


static int _sock = -1;
static char _rx_buf[2048];


static String _new_message_id()
{
    char buffer[48];
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    uint32_t r3 = esp_random();
    uint32_t r4 = esp_random();
    snprintf(buffer, sizeof(buffer), "urn:uuid:%08x-%04x-4%03x-%04x-%04x%08x",
             (unsigned)r1, (unsigned)(r2 >> 16), (unsigned)(r2 & 0x0FFF),
             (unsigned)(((r3 >> 16) & 0x3FFF) | 0x8000), (unsigned)(r3 & 0xFFFF), (unsigned)r4);
    return buffer;
}


static String _envelope(const String& relates_to, const char* to, const char* action, const String& body)
{
    String xml;
    xml.reserve(1400);
    xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\" "
           "xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
           "xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
           "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\" "
           "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
           "<SOAP-ENV:Header><wsa:MessageID>";
    xml += _new_message_id();
    xml += "</wsa:MessageID>";
    if (relates_to != "")
    {
        xml += "<wsa:RelatesTo>";
        xml += relates_to;
        xml += "</wsa:RelatesTo>";
    }
    xml += "<wsa:To>";
    xml += to;
    xml += "</wsa:To><wsa:Action>";
    xml += action;
    xml += "</wsa:Action></SOAP-ENV:Header><SOAP-ENV:Body>";
    xml += body;
    xml += "</SOAP-ENV:Body></SOAP-ENV:Envelope>";
    return xml;
}


// Content shared by Hello and ProbeMatch
static String _endpoint_description()
{
    String xml = "<wsa:EndpointReference><wsa:Address>";
    xml += Onvif::get_device_uuid();
    xml += "</wsa:Address></wsa:EndpointReference>"
           "<wsd:Types>dn:NetworkVideoTransmitter tds:Device</wsd:Types><wsd:Scopes>";
    auto scopes = Onvif::get_scopes();
    for (size_t i = 0; i < scopes.size(); i++)
    {
        if (i > 0)
            xml += " ";
        xml += scopes[i];
    }
    xml += "</wsd:Scopes><wsd:XAddrs>";
    xml += NetManager::get_onvif_url();
    xml += "</wsd:XAddrs><wsd:MetadataVersion>1</wsd:MetadataVersion>";
    return xml;
}


static void _send_to(const sockaddr_in& addr, const String& message)
{
    sendto(_sock, message.c_str(), message.length(), 0, (const sockaddr*)&addr, sizeof(addr));
}


static void _send_hello()
{
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(WSD_PORT);
    addr.sin_addr.s_addr = inet_addr(WSD_MULTICAST_ADDR);

    String body = "<wsd:Hello>" + _endpoint_description() + "</wsd:Hello>";
    _send_to(addr, _envelope("", "urn:schemas-xmlsoap-org:ws:2005:04:discovery",
                             "http://schemas.xmlsoap.org/ws/2005/04/discovery/Hello", body));
}


static void _handle_message(const sockaddr_in& from)
{
    // Only probes, our own Hello and other devices' matches land here too
    if (Onvif::find_element(_rx_buf, "Probe") == nullptr)
        return;

    // An empty Types means "anything"
    String types = Onvif::get_element_text(_rx_buf, "Types");
    if (types != "" && types.indexOf("NetworkVideoTransmitter") < 0 && types.indexOf("Device") < 0)
        return;

    String message_id = Onvif::get_element_text(_rx_buf, "MessageID");
    String body = "<wsd:ProbeMatches><wsd:ProbeMatch>" + _endpoint_description() + "</wsd:ProbeMatch></wsd:ProbeMatches>";
    _send_to(from, _envelope(message_id, "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous",
                             "http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches", body));
    log_i("onvif probe from %s answered", inet_ntoa(from.sin_addr));
}


static void _discovery_task(void* param)
{
    while (1)
    {
        sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(_sock, _rx_buf, sizeof(_rx_buf) - 1, 0, (sockaddr*)&from, &from_len);
        if (len <= 0)
        {
            delay(100);
            continue;
        }
        _rx_buf[len] = 0;
        _handle_message(from);
    }
}


void Onvif::start_discovery()
{
    _sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (_sock < 0)
    {
        log_e("discovery socket failed");
        return;
    }
    int one = 1;
    setsockopt(_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(WSD_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(_sock, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        log_e("discovery bind failed");
        close(_sock);
        _sock = -1;
        return;
    }

    ip_mreq mreq = {};
    mreq.imr_multiaddr.s_addr = inet_addr(WSD_MULTICAST_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
        log_w("discovery multicast join failed, only unicast probes will work");

    _send_hello();
    xTaskCreatePinnedToCore(_discovery_task, "wsd", 6144, nullptr, 2, nullptr, 1);
    log_i("onvif discovery started");
}
