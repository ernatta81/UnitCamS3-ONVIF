/**
 * @file rtsp_server.cpp
 * @brief Single task RTSP server. RTP is sent over UDP or interleaved on the RTSP
 *        TCP connection, whatever the client asks for. Digest authentication
 *        (basic is accepted too) once per connection, when a password is set.
 * @version 0.1
 * @date 2026-10-29
 *
 */
#include <Arduino.h>
#include <esp_camera.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include "rtsp_server.h"
#include "../app_config.h"
#include "../net/net_manager.h"
#include "../utils/auth_utils.h"


#define RTSP_MAX_CLIENTS        4
#define RTSP_RX_BUFFER_SIZE     2048
#define RTSP_SESSION_TIMEOUT_S  60
#define RTSP_SEND_TIMEOUT_S     3
#define RTP_SERVER_PORT         6970    // rtcp on RTP_SERVER_PORT + 1
#define RTP_MAX_PAYLOAD_SIZE    1400
#define RTP_PAYLOAD_TYPE_JPEG   26
#define RTCP_SR_INTERVAL_MS     5000


struct RtspClient_t
{
    int sock;
    bool is_authenticated;
    bool is_setup;
    bool is_playing;
    bool is_tcp;
    char nonce[33];
    uint8_t rtp_channel;
    uint8_t rtcp_channel;
    sockaddr_in rtp_addr;
    sockaddr_in rtcp_addr;
    uint32_t session_id;
    uint32_t ssrc;
    uint16_t seq;
    uint32_t packet_count;
    uint32_t octet_count;
    uint32_t last_seen;
    uint32_t last_sr;
    size_t rx_len;
    char rx_buf[RTSP_RX_BUFFER_SIZE + 1];
};


struct JpegInfo_t
{
    const uint8_t* qtables[2];
    uint16_t width;
    uint16_t height;
    uint8_t type;   // RFC 2435: 0 = 4:2:2, 1 = 4:2:0
    uint16_t dri;   // Restart interval, 0 if none
    const uint8_t* scan;
    size_t scan_len;
};


static RtspClient_t _clients[RTSP_MAX_CLIENTS];
static int _listen_sock = -1;
static int _rtp_sock = -1;
static int _rtcp_sock = -1;
static uint16_t _port = APP_RTSP_PORT;
static int _fps = 15;
static String _auth_user;
static String _auth_password;
static volatile int _playing_count = 0;

// Interleave header + rtp header + payload, built once per packet and shared by all clients
static uint8_t _packet[4 + 12 + RTP_MAX_PAYLOAD_SIZE];


/* -------------------------------------------------------------------------- */
/*                                    Utils                                   */
/* -------------------------------------------------------------------------- */

static inline void _put_u32(uint8_t* p, uint32_t value)
{
    p[0] = value >> 24;
    p[1] = value >> 16;
    p[2] = value >> 8;
    p[3] = value;
}


// 90 kHz media clock
static inline uint32_t _get_rtp_timestamp()
{
    return (uint32_t)(esp_timer_get_time() * 9 / 100);
}


static bool _send_all(int sock, const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    while (len > 0)
    {
        int ret = send(sock, p, len, 0);
        if (ret <= 0)
            return false;
        p += ret;
        len -= ret;
    }
    return true;
}


static void _send_udp(int sock, const sockaddr_in& addr, const uint8_t* data, size_t len)
{
    // lwip runs out of buffers when a whole frame goes out in a burst, give it a moment
    for (int retry = 0; retry < 20; retry++)
    {
        if (sendto(sock, data, len, 0, (const sockaddr*)&addr, sizeof(addr)) >= 0)
            return;
        if (errno != ENOMEM)
            return;
        vTaskDelay(1);
    }
}


static int _create_udp_socket(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
        return -1;

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        return -1;
    }
    return sock;
}


/* -------------------------------------------------------------------------- */
/*                                    JPEG                                    */
/* -------------------------------------------------------------------------- */

// Pick out what RFC 2435 needs: quantization tables, size, sampling and the entropy coded data
static bool _parse_jpeg(const uint8_t* buf, size_t len, JpegInfo_t& info)
{
    memset(&info, 0, sizeof(info));
    if (len < 4 || buf[0] != 0xFF || buf[1] != 0xD8)
        return false;

    size_t i = 2;
    while (i + 4 <= len)
    {
        if (buf[i] != 0xFF)
            return false;

        uint8_t marker = buf[i + 1];
        // Fill bytes
        if (marker == 0xFF)
        {
            i++;
            continue;
        }

        size_t seg_len = (buf[i + 2] << 8) | buf[i + 3];
        if (seg_len < 2 || i + 2 + seg_len > len)
            return false;
        const uint8_t* seg = buf + i + 4;
        size_t seg_data_len = seg_len - 2;

        // DQT, may hold several tables
        if (marker == 0xDB)
        {
            size_t p = 0;
            while (p + 65 <= seg_data_len)
            {
                // Only 8 bit tables can be sent
                if ((seg[p] >> 4) != 0)
                    return false;
                uint8_t id = seg[p] & 0x0F;
                if (id < 2)
                    info.qtables[id] = seg + p + 1;
                p += 65;
            }
        }

        // SOF0, baseline
        else if (marker == 0xC0)
        {
            if (seg_data_len < 9)
                return false;
            info.height = (seg[1] << 8) | seg[2];
            info.width = (seg[3] << 8) | seg[4];
            // Luma sampling factors
            uint8_t sampling = seg[7];
            if (seg[5] != 3)
                return false;
            if (sampling == 0x21)
                info.type = 0;
            else if (sampling == 0x22)
                info.type = 1;
            else
                return false;
        }

        // Progressive, lossless, arithmetic: not streamable
        else if (marker >= 0xC1 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
        {
            return false;
        }

        // DRI
        else if (marker == 0xDD)
        {
            if (seg_data_len < 2)
                return false;
            info.dri = (seg[0] << 8) | seg[1];
        }

        // SOS, entropy coded data follows until EOI
        else if (marker == 0xDA)
        {
            size_t scan_start = i + 2 + seg_len;
            size_t scan_end = len;
            while (scan_end > scan_start + 1 && !(buf[scan_end - 2] == 0xFF && buf[scan_end - 1] == 0xD9))
                scan_end--;
            if (scan_end <= scan_start + 1)
                return false;

            info.scan = buf + scan_start;
            info.scan_len = scan_end - 2 - scan_start;

            if (info.width == 0 || info.height == 0 || info.width > 2040 || info.height > 2040)
                return false;
            if (info.qtables[0] == nullptr)
                return false;
            // Some encoders share one table between luma and chroma
            if (info.qtables[1] == nullptr)
                info.qtables[1] = info.qtables[0];
            return true;
        }

        i += 2 + seg_len;
    }
    return false;
}


/* -------------------------------------------------------------------------- */
/*                                     RTP                                    */
/* -------------------------------------------------------------------------- */

static void _close_client(RtspClient_t& client);


// Packet payload must already be in _packet
static void _send_rtp(RtspClient_t& client, size_t rtp_len, uint32_t timestamp, bool marker)
{
    uint8_t* rtp = _packet + 4;
    rtp[0] = 0x80;
    rtp[1] = (marker ? 0x80 : 0x00) | RTP_PAYLOAD_TYPE_JPEG;
    rtp[2] = client.seq >> 8;
    rtp[3] = client.seq;
    _put_u32(rtp + 4, timestamp);
    _put_u32(rtp + 8, client.ssrc);

    client.seq++;
    client.packet_count++;
    client.octet_count += rtp_len - 12;

    if (client.is_tcp)
    {
        _packet[0] = '$';
        _packet[1] = client.rtp_channel;
        _packet[2] = rtp_len >> 8;
        _packet[3] = rtp_len;
        if (!_send_all(client.sock, _packet, rtp_len + 4))
        {
            log_w("rtsp client send failed");
            _close_client(client);
        }
        return;
    }

    _send_udp(_rtp_sock, client.rtp_addr, rtp, rtp_len);
}


static void _send_sender_report(RtspClient_t& client)
{
    uint8_t buffer[4 + 28];
    uint8_t* sr = buffer + 4;

    timeval tv;
    gettimeofday(&tv, nullptr);
    uint32_t ntp_sec = (uint32_t)tv.tv_sec + 2208988800UL;
    uint32_t ntp_frac = (uint32_t)(((uint64_t)tv.tv_usec << 32) / 1000000);

    sr[0] = 0x80;
    sr[1] = 200;    // SR
    sr[2] = 0;
    sr[3] = 6;      // Length in 32 bit words minus one
    _put_u32(sr + 4, client.ssrc);
    _put_u32(sr + 8, ntp_sec);
    _put_u32(sr + 12, ntp_frac);
    _put_u32(sr + 16, _get_rtp_timestamp());
    _put_u32(sr + 20, client.packet_count);
    _put_u32(sr + 24, client.octet_count);

    if (client.is_tcp)
    {
        buffer[0] = '$';
        buffer[1] = client.rtcp_channel;
        buffer[2] = 0;
        buffer[3] = 28;
        if (!_send_all(client.sock, buffer, sizeof(buffer)))
            _close_client(client);
        return;
    }

    _send_udp(_rtcp_sock, client.rtcp_addr, sr, 28);
}


static void _send_jpeg(const JpegInfo_t& jpeg)
{
    uint32_t timestamp = _get_rtp_timestamp();
    uint8_t type = jpeg.type | (jpeg.dri ? 64 : 0);

    size_t offset = 0;
    while (offset < jpeg.scan_len)
    {
        uint8_t* payload = _packet + 4 + 12;
        size_t header_len = 0;

        // Main JPEG header
        payload[header_len++] = 0;              // Type specific
        payload[header_len++] = offset >> 16;   // Fragment offset
        payload[header_len++] = offset >> 8;
        payload[header_len++] = offset;
        payload[header_len++] = type;
        payload[header_len++] = 255;            // Q: tables are sent in band
        payload[header_len++] = jpeg.width / 8;
        payload[header_len++] = jpeg.height / 8;

        // Restart marker header
        if (jpeg.dri)
        {
            payload[header_len++] = jpeg.dri >> 8;
            payload[header_len++] = jpeg.dri;
            payload[header_len++] = 0xFF;       // F = 1, L = 1, count = 0x3FFF
            payload[header_len++] = 0xFF;
        }

        // Quantization table header, first packet only
        if (offset == 0)
        {
            payload[header_len++] = 0;          // MBZ
            payload[header_len++] = 0;          // 8 bit precision
            payload[header_len++] = 0;
            payload[header_len++] = 128;        // Two 64 byte tables
            memcpy(payload + header_len, jpeg.qtables[0], 64);
            header_len += 64;
            memcpy(payload + header_len, jpeg.qtables[1], 64);
            header_len += 64;
        }

        size_t chunk = RTP_MAX_PAYLOAD_SIZE - header_len;
        if (chunk > jpeg.scan_len - offset)
            chunk = jpeg.scan_len - offset;
        memcpy(payload + header_len, jpeg.scan + offset, chunk);
        offset += chunk;

        size_t rtp_len = 12 + header_len + chunk;
        bool is_last = offset >= jpeg.scan_len;
        for (auto& client : _clients)
        {
            if (client.sock >= 0 && client.is_playing)
                _send_rtp(client, rtp_len, timestamp, is_last);
        }
    }

    uint32_t now = millis();
    for (auto& client : _clients)
    {
        if (client.sock >= 0 && client.is_playing && now - client.last_sr > RTCP_SR_INTERVAL_MS)
        {
            client.last_sr = now;
            _send_sender_report(client);
        }
    }
}


static void _stream_frame()
{
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr)
    {
        log_e("camera frame failed");
        return;
    }

    JpegInfo_t jpeg;
    if (fb->format == PIXFORMAT_JPEG && _parse_jpeg(fb->buf, fb->len, jpeg))
        _send_jpeg(jpeg);
    else
        log_w("frame not streamable, skipped");

    esp_camera_fb_return(fb);
}


/* -------------------------------------------------------------------------- */
/*                                    RTSP                                    */
/* -------------------------------------------------------------------------- */

static void _reset_client(RtspClient_t& client)
{
    client.sock = -1;
    client.is_authenticated = false;
    client.is_setup = false;
    client.is_playing = false;
    client.is_tcp = false;
    client.nonce[0] = 0;
    client.rtp_channel = 0;
    client.rtcp_channel = 1;
    memset(&client.rtp_addr, 0, sizeof(client.rtp_addr));
    memset(&client.rtcp_addr, 0, sizeof(client.rtcp_addr));
    client.session_id = 0;
    client.ssrc = esp_random();
    client.seq = esp_random();
    client.packet_count = 0;
    client.octet_count = 0;
    client.last_seen = millis();
    client.last_sr = 0;
    client.rx_len = 0;
}


static void _close_client(RtspClient_t& client)
{
    if (client.sock < 0)
        return;
    log_i("rtsp client closed");
    close(client.sock);
    _reset_client(client);
}


// Header value of a null terminated request, case insensitive name
static bool _get_header(const char* request, const char* name, char* out, size_t out_size)
{
    size_t name_len = strlen(name);
    const char* line = strstr(request, "\r\n");
    while (line != nullptr)
    {
        line += 2;
        if (strncasecmp(line, name, name_len) == 0 && line[name_len] == ':')
        {
            const char* value = line + name_len + 1;
            while (*value == ' ' || *value == '\t')
                value++;
            size_t n = strcspn(value, "\r\n");
            if (n >= out_size)
                n = out_size - 1;
            memcpy(out, value, n);
            out[n] = 0;
            return true;
        }
        line = strstr(line, "\r\n");
    }
    return false;
}


static bool _reply(RtspClient_t& client, const char* cseq, const char* status, const char* headers = "", const char* body = nullptr)
{
    char buffer[1024];
    int n = snprintf(buffer, sizeof(buffer), "RTSP/1.0 %s\r\nCSeq: %s\r\nServer: " APP_MODEL "\r\n%s", status, cseq, headers);
    if (body != nullptr)
        n += snprintf(buffer + n, sizeof(buffer) - n, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(body), body);
    else
        n += snprintf(buffer + n, sizeof(buffer) - n, "\r\n");

    if (n >= (int)sizeof(buffer))
        return false;
    return _send_all(client.sock, buffer, n);
}


// Clients repeat the Authorization header on every request, checking it once per connection is enough
static bool _check_auth(RtspClient_t& client, const char* request, const char* method)
{
    if (_auth_password == "" || client.is_authenticated)
        return true;

    char authorization[512];
    if (!_get_header(request, "Authorization", authorization, sizeof(authorization)))
        return false;

    if (strncasecmp(authorization, "Digest ", 7) == 0)
        client.is_authenticated = AuthUtils::check_digest(authorization + 7, method, APP_MODEL, client.nonce, _auth_user, _auth_password);
    else if (strncasecmp(authorization, "Basic ", 6) == 0)
        client.is_authenticated = AuthUtils::check_basic(authorization + 6, _auth_user, _auth_password);

    if (!client.is_authenticated)
        log_w("rtsp authentication failed");
    return client.is_authenticated;
}


static bool _reply_unauthorized(RtspClient_t& client, const char* cseq)
{
    // A fresh nonce for every challenge
    strncpy(client.nonce, AuthUtils::new_nonce().c_str(), sizeof(client.nonce) - 1);
    client.nonce[sizeof(client.nonce) - 1] = 0;

    char headers[128];
    snprintf(headers, sizeof(headers), "WWW-Authenticate: Digest realm=\"" APP_MODEL "\", nonce=\"%s\"\r\n", client.nonce);
    return _reply(client, cseq, "401 Unauthorized", headers);
}


static bool _handle_setup(RtspClient_t& client, const char* request, const char* cseq)
{
    char transport[256] = "";
    _get_header(request, "Transport", transport, sizeof(transport));

    if (strstr(transport, "multicast") != nullptr)
        return _reply(client, cseq, "461 Unsupported Transport");

    char transport_reply[160];
    if (strstr(transport, "RTP/AVP/TCP") != nullptr)
    {
        int rtp_channel = 0;
        int rtcp_channel = 1;
        const char* p = strstr(transport, "interleaved=");
        if (p != nullptr && sscanf(p, "interleaved=%d-%d", &rtp_channel, &rtcp_channel) == 1)
            rtcp_channel = rtp_channel + 1;

        client.is_tcp = true;
        client.rtp_channel = rtp_channel;
        client.rtcp_channel = rtcp_channel;
        snprintf(transport_reply, sizeof(transport_reply), "RTP/AVP/TCP;unicast;interleaved=%d-%d;ssrc=%08X",
                 rtp_channel, rtcp_channel, (unsigned)client.ssrc);
    }
    else
    {
        int rtp_port = 0;
        int rtcp_port = 0;
        const char* p = strstr(transport, "client_port=");
        int n = p ? sscanf(p, "client_port=%d-%d", &rtp_port, &rtcp_port) : 0;
        if (n < 1 || _rtp_sock < 0)
            return _reply(client, cseq, "461 Unsupported Transport");
        if (n == 1)
            rtcp_port = rtp_port + 1;

        // Udp goes to the address the rtsp connection comes from
        sockaddr_in peer = {};
        socklen_t peer_len = sizeof(peer);
        getpeername(client.sock, (sockaddr*)&peer, &peer_len);

        client.is_tcp = false;
        client.rtp_addr = peer;
        client.rtp_addr.sin_port = htons(rtp_port);
        client.rtcp_addr = peer;
        client.rtcp_addr.sin_port = htons(rtcp_port);
        snprintf(transport_reply, sizeof(transport_reply), "RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d;ssrc=%08X",
                 rtp_port, rtcp_port, RTP_SERVER_PORT, RTP_SERVER_PORT + 1, (unsigned)client.ssrc);
    }

    if (client.session_id == 0)
        client.session_id = esp_random() | 1;
    client.is_setup = true;

    char headers[256];
    snprintf(headers, sizeof(headers), "Transport: %s\r\nSession: %08X;timeout=%d\r\n",
             transport_reply, (unsigned)client.session_id, RTSP_SESSION_TIMEOUT_S);
    return _reply(client, cseq, "200 OK", headers);
}


// Returns false when the connection has to be closed
static bool _handle_request(RtspClient_t& client, const char* request)
{
    char method[16];
    char url[256];
    if (sscanf(request, "%15s %255s", method, url) != 2)
        return false;

    char cseq[16] = "0";
    _get_header(request, "CSeq", cseq, sizeof(cseq));
    client.last_seen = millis();
    log_d("rtsp %s %s", method, url);

    char session[48];
    snprintf(session, sizeof(session), "Session: %08X\r\n", (unsigned)client.session_id);

    if (strcmp(method, "OPTIONS") == 0)
    {
        return _reply(client, cseq, "200 OK", "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, SET_PARAMETER\r\n");
    }

    // Everything past OPTIONS needs credentials
    if (!_check_auth(client, request, method))
        return _reply_unauthorized(client, cseq);

    if (strcmp(method, "DESCRIBE") == 0)
    {
        char sdp[400];
        snprintf(sdp, sizeof(sdp),
                 "v=0\r\n"
                 "o=- %u 1 IN IP4 %s\r\n"
                 "s=" APP_MODEL "\r\n"
                 "c=IN IP4 0.0.0.0\r\n"
                 "t=0 0\r\n"
                 "a=control:*\r\n"
                 "a=range:npt=0-\r\n"
                 "m=video 0 RTP/AVP 26\r\n"
                 "a=rtpmap:26 JPEG/90000\r\n"
                 "a=framerate:%d\r\n"
                 "a=control:track1\r\n",
                 (unsigned)esp_random(), NetManager::get_local_ip().toString().c_str(), _fps);

        char headers[400];
        size_t url_len = strlen(url);
        snprintf(headers, sizeof(headers), "Content-Base: %s%s\r\nContent-Type: application/sdp\r\n",
                 url, (url_len > 0 && url[url_len - 1] == '/') ? "" : "/");
        return _reply(client, cseq, "200 OK", headers, sdp);
    }

    if (strcmp(method, "SETUP") == 0)
    {
        return _handle_setup(client, request, cseq);
    }

    if (strcmp(method, "PLAY") == 0)
    {
        if (!client.is_setup)
            return _reply(client, cseq, "455 Method Not Valid in This State");

        char headers[96];
        snprintf(headers, sizeof(headers), "%sRange: npt=0.000-\r\n", session);
        if (!_reply(client, cseq, "200 OK", headers))
            return false;

        client.is_playing = true;
        client.last_sr = 0;
        log_i("rtsp client playing over %s", client.is_tcp ? "tcp" : "udp");
        return true;
    }

    if (strcmp(method, "PAUSE") == 0)
    {
        client.is_playing = false;
        return _reply(client, cseq, "200 OK", session);
    }

    if (strcmp(method, "TEARDOWN") == 0)
    {
        _reply(client, cseq, "200 OK", session);
        return false;
    }

    // Keep alive
    if (strcmp(method, "GET_PARAMETER") == 0 || strcmp(method, "SET_PARAMETER") == 0)
    {
        return _reply(client, cseq, "200 OK", session);
    }

    return _reply(client, cseq, "501 Not Implemented");
}


static void _consume_rx(RtspClient_t& client, size_t len)
{
    memmove(client.rx_buf, client.rx_buf + len, client.rx_len - len);
    client.rx_len -= len;
}


// Returns false when the connection has to be closed
static bool _process_rx(RtspClient_t& client)
{
    while (client.rx_len > 0)
    {
        // Interleaved rtcp receiver reports from the client, skip them
        if (client.rx_buf[0] == '$')
        {
            if (client.rx_len < 4)
                return true;
            size_t frame_len = 4 + (((uint8_t)client.rx_buf[2] << 8) | (uint8_t)client.rx_buf[3]);
            if (frame_len > RTSP_RX_BUFFER_SIZE)
                return false;
            if (client.rx_len < frame_len)
                return true;
            client.last_seen = millis();
            _consume_rx(client, frame_len);
            continue;
        }

        client.rx_buf[client.rx_len] = 0;
        char* header_end = strstr(client.rx_buf, "\r\n\r\n");
        if (header_end == nullptr)
            return client.rx_len < RTSP_RX_BUFFER_SIZE;
        size_t header_len = header_end - client.rx_buf + 4;

        // Terminate the header block to parse it alone
        char saved = client.rx_buf[header_len];
        client.rx_buf[header_len] = 0;

        char content_length[16] = "0";
        _get_header(client.rx_buf, "Content-Length", content_length, sizeof(content_length));
        size_t total_len = header_len + atoi(content_length);
        if (total_len > RTSP_RX_BUFFER_SIZE)
            return false;
        if (client.rx_len < total_len)
        {
            client.rx_buf[header_len] = saved;
            return true;
        }

        bool keep = _handle_request(client, client.rx_buf);
        client.rx_buf[header_len] = saved;
        if (!keep)
            return false;
        _consume_rx(client, total_len);
    }
    return true;
}


static void _accept_client()
{
    sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int sock = accept(_listen_sock, (sockaddr*)&addr, &addr_len);
    if (sock < 0)
        return;

    RtspClient_t* client = nullptr;
    for (auto& i : _clients)
    {
        if (i.sock < 0)
        {
            client = &i;
            break;
        }
    }
    if (client == nullptr)
    {
        log_w("rtsp client rejected, max %d", RTSP_MAX_CLIENTS);
        close(sock);
        return;
    }

    timeval send_timeout = {RTSP_SEND_TIMEOUT_S, 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

    _reset_client(*client);
    client->sock = sock;
    log_i("rtsp client from %s", inet_ntoa(addr.sin_addr));
}


static void _receive_client(RtspClient_t& client)
{
    int n = recv(client.sock, client.rx_buf + client.rx_len, RTSP_RX_BUFFER_SIZE - client.rx_len, 0);
    if (n <= 0)
    {
        _close_client(client);
        return;
    }
    client.rx_len += n;

    if (!_process_rx(client))
        _close_client(client);
}


// Udp clients prove they are alive with rtcp receiver reports
static void _receive_rtcp()
{
    uint8_t buffer[256];
    sockaddr_in from;
    socklen_t from_len = sizeof(from);
    if (recvfrom(_rtcp_sock, buffer, sizeof(buffer), 0, (sockaddr*)&from, &from_len) <= 0)
        return;

    for (auto& client : _clients)
    {
        if (client.sock >= 0 && !client.is_tcp && client.rtcp_addr.sin_addr.s_addr == from.sin_addr.s_addr)
            client.last_seen = millis();
    }
}


static void _rtsp_task(void* param)
{
    const uint32_t frame_interval_ms = 1000 / _fps;
    uint32_t last_frame = 0;

    while (1)
    {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(_listen_sock, &read_fds);
        int max_fd = _listen_sock;
        if (_rtcp_sock >= 0)
        {
            FD_SET(_rtcp_sock, &read_fds);
            max_fd = max(max_fd, _rtcp_sock);
        }
        for (auto& client : _clients)
        {
            if (client.sock >= 0)
            {
                FD_SET(client.sock, &read_fds);
                max_fd = max(max_fd, client.sock);
            }
        }

        // Wake up in time for the next frame when somebody is watching
        uint32_t wait_ms = 200;
        if (_playing_count > 0)
        {
            uint32_t elapsed = millis() - last_frame;
            wait_ms = elapsed >= frame_interval_ms ? 0 : frame_interval_ms - elapsed;
        }
        timeval timeout = {0, (long)wait_ms * 1000};

        if (select(max_fd + 1, &read_fds, nullptr, nullptr, &timeout) > 0)
        {
            if (FD_ISSET(_listen_sock, &read_fds))
                _accept_client();
            if (_rtcp_sock >= 0 && FD_ISSET(_rtcp_sock, &read_fds))
                _receive_rtcp();
            for (auto& client : _clients)
            {
                if (client.sock >= 0 && FD_ISSET(client.sock, &read_fds))
                    _receive_client(client);
            }
        }

        // Drop dead sessions. Tcp streaming clients are dropped when a send fails instead.
        uint32_t now = millis();
        int playing_count = 0;
        for (auto& client : _clients)
        {
            if (client.sock < 0)
                continue;
            bool is_streaming_tcp = client.is_playing && client.is_tcp;
            if (!is_streaming_tcp && now - client.last_seen > RTSP_SESSION_TIMEOUT_S * 2 * 1000)
            {
                log_i("rtsp session timeout");
                _close_client(client);
                continue;
            }
            if (client.is_playing)
                playing_count++;
        }
        _playing_count = playing_count;

        if (_playing_count > 0 && millis() - last_frame >= frame_interval_ms)
        {
            last_frame = millis();
            _stream_frame();
        }
    }
}


void RtspServer::start(uint16_t port, int fps, const String& user, const String& password)
{
    _port = port;
    _fps = constrain(fps, 1, 30);
    _auth_user = user;
    _auth_password = password;

    for (auto& client : _clients)
        _reset_client(client);

    _listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (_listen_sock < 0)
    {
        log_e("rtsp socket failed");
        return;
    }
    int one = 1;
    setsockopt(_listen_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(_listen_sock, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(_listen_sock, RTSP_MAX_CLIENTS) < 0)
    {
        log_e("rtsp bind/listen on %d failed", _port);
        close(_listen_sock);
        _listen_sock = -1;
        return;
    }

    // Udp transport is optional, tcp interleaved still works without it
    _rtp_sock = _create_udp_socket(RTP_SERVER_PORT);
    _rtcp_sock = _create_udp_socket(RTP_SERVER_PORT + 1);
    if (_rtp_sock < 0 || _rtcp_sock < 0)
        log_w("rtp udp sockets failed, only tcp transport available");

    xTaskCreatePinnedToCore(_rtsp_task, "rtsp", 8192, nullptr, 3, nullptr, 1);
    log_i("rtsp server started on %d, %d fps, auth %s", _port, _fps, _auth_password != "" ? "on" : "off");
}


int RtspServer::get_client_count()
{
    return _playing_count;
}
