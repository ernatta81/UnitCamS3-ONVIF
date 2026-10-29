/**
 * @file onvif_device.cpp
 * @brief ONVIF device and media services over SOAP/HTTP.
 *        One fixed profile (JPEG, current camera resolution) pointing to the RTSP server.
 *        When a password is set, requests need a WS-Security UsernameToken or HTTP digest,
 *        except the pre-auth actions clients use before logging in.
 * @version 0.1
 * @date 2026-10-29
 *
 */
#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <time.h>
#include <ESPAsyncWebServer.h>
#include "onvif.h"
#include "../app_config.h"
#include "../hal/hal.h"
#include "../net/net_manager.h"
#include "../utils/auth_utils.h"


#define SOAP_MAX_BODY_SIZE  (16 * 1024)
#define SOAP_CONTENT_TYPE   "application/soap+xml; charset=utf-8"


static const char* SOAP_ENVELOPE_BEGIN =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\" "
    "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
    "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" "
    "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
    "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
    "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
    "xmlns:ter=\"http://www.onvif.org/ver10/error\">"
    "<SOAP-ENV:Body>";

static const char* SOAP_ENVELOPE_END = "</SOAP-ENV:Body></SOAP-ENV:Envelope>";


/* -------------------------------------------------------------------------- */
/*                                   Helpers                                  */
/* -------------------------------------------------------------------------- */

String Onvif::get_device_uuid()
{
    // Stable across reboots, derived from the mac
    return "urn:uuid:554e4954-4341-4d53-3300-" + HAL::hal::GetHal()->getDeviceId();
}


std::vector<String> Onvif::get_scopes()
{
    return {
        "onvif://www.onvif.org/type/video_encoder",
        "onvif://www.onvif.org/Profile/Streaming",
        "onvif://www.onvif.org/hardware/" APP_MODEL,
        "onvif://www.onvif.org/name/" + HAL::hal::GetHal()->getConfig().hostname,
    };
}


// True if the name at name_pos opens a start tag: "<name" or "<prefix:name"
static bool _is_start_tag(const char* xml, const char* name_pos)
{
    if (name_pos == xml)
        return false;
    const char* p = name_pos - 1;
    if (*p == '<')
        return true;
    if (*p != ':')
        return false;
    while (p > xml)
    {
        p--;
        if (*p == '<')
            return true;
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.')
            return false;
    }
    return false;
}


const char* Onvif::find_element(const char* xml, const char* local_name)
{
    size_t name_len = strlen(local_name);
    const char* p = xml;
    while ((p = strstr(p, local_name)) != nullptr)
    {
        // Followed by '>', '/' or attributes
        char next = p[name_len];
        if (_is_start_tag(xml, p) && (next == '>' || next == ' ' || next == '/' || next == '\t' || next == '\r' || next == '\n'))
        {
            const char* end = strchr(p, '>');
            return end ? end + 1 : nullptr;
        }
        p += name_len;
    }
    return nullptr;
}


String Onvif::get_element_text(const char* xml, const char* local_name)
{
    const char* start = find_element(xml, local_name);
    if (start == nullptr || start[-2] == '/')
        return "";
    const char* end = strchr(start, '<');
    if (end == nullptr)
        return "";

    String text;
    text.reserve(end - start);
    for (const char* p = start; p < end; p++)
        text += *p;
    text.trim();
    return text;
}


// Local name of the first element inside the soap Body, e.g. "GetProfiles"
static String _get_action(const char* request)
{
    const char* p = Onvif::find_element(request, "Body");
    if (p == nullptr)
        return "";
    p = strchr(p, '<');
    if (p == nullptr)
        return "";
    p++;

    String name;
    while (*p != 0 && strchr(" />\t\r\n", *p) == nullptr)
    {
        if (*p == ':')
            name = "";
        else
            name += *p;
        p++;
    }
    return name;
}


struct VideoInfo_t
{
    int width;
    int height;
    int fps;
    int quality;
};

static VideoInfo_t _get_video_info()
{
    auto& config = HAL::hal::GetHal()->getConfig();
    sensor_t* s = esp_camera_sensor_get();
    framesize_t frame_size = s ? (framesize_t)s->status.framesize : FRAMESIZE_VGA;

    VideoInfo_t info;
    info.width = resolution[frame_size].width;
    info.height = resolution[frame_size].height;
    info.fps = config.fps;
    // ONVIF quality: higher is better
    info.quality = 63 - (s ? s->status.quality : config.jpeg_quality);
    return info;
}


static String _media_service_url()
{
    return "http://" + NetManager::get_local_ip().toString() + "/onvif/media_service";
}


static String _fault(const char* code, const char* subcode, const char* reason)
{
    String xml = "<SOAP-ENV:Fault><SOAP-ENV:Code><SOAP-ENV:Value>";
    xml += code;
    xml += "</SOAP-ENV:Value><SOAP-ENV:Subcode><SOAP-ENV:Value>";
    xml += subcode;
    xml += "</SOAP-ENV:Value></SOAP-ENV:Subcode></SOAP-ENV:Code><SOAP-ENV:Reason><SOAP-ENV:Text xml:lang=\"en\">";
    xml += reason;
    xml += "</SOAP-ENV:Text></SOAP-ENV:Reason></SOAP-ENV:Fault>";
    return xml;
}


/* -------------------------------------------------------------------------- */
/*                               Device service                               */
/* -------------------------------------------------------------------------- */

static String _get_system_date_and_time(const char* request)
{
    time_t now = time(nullptr);
    struct tm t;
    gmtime_r(&now, &t);

    char buffer[640];
    snprintf(buffer, sizeof(buffer),
             "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>"
             "<tt:DateTimeType>NTP</tt:DateTimeType>"
             "<tt:DaylightSavings>false</tt:DaylightSavings>"
             "<tt:TimeZone><tt:TZ>UTC0</tt:TZ></tt:TimeZone>"
             "<tt:UTCDateTime>"
             "<tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time>"
             "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date>"
             "</tt:UTCDateTime>"
             "</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>",
             t.tm_hour, t.tm_min, t.tm_sec, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    return buffer;
}


static String _get_capabilities(const char* request)
{
    String xml = "<tds:GetCapabilitiesResponse><tds:Capabilities>"
                 "<tt:Device><tt:XAddr>";
    xml += NetManager::get_onvif_url();
    xml += "</tt:XAddr>"
           "<tt:Network><tt:IPFilter>false</tt:IPFilter><tt:ZeroConfiguration>false</tt:ZeroConfiguration>"
           "<tt:IPVersion6>false</tt:IPVersion6><tt:DynDNS>false</tt:DynDNS></tt:Network>"
           "<tt:System><tt:DiscoveryResolve>false</tt:DiscoveryResolve><tt:DiscoveryBye>false</tt:DiscoveryBye>"
           "<tt:RemoteDiscovery>false</tt:RemoteDiscovery><tt:SystemBackup>false</tt:SystemBackup>"
           "<tt:SystemLogging>false</tt:SystemLogging><tt:FirmwareUpgrade>false</tt:FirmwareUpgrade>"
           "<tt:SupportedVersions><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tt:SupportedVersions></tt:System>"
           "<tt:IO><tt:InputConnectors>0</tt:InputConnectors><tt:RelayOutputs>0</tt:RelayOutputs></tt:IO>"
           "<tt:Security><tt:TLS1.1>false</tt:TLS1.1><tt:TLS1.2>false</tt:TLS1.2>"
           "<tt:OnboardKeyGeneration>false</tt:OnboardKeyGeneration><tt:AccessPolicyConfig>false</tt:AccessPolicyConfig>"
           "<tt:X.509Token>false</tt:X.509Token><tt:SAMLToken>false</tt:SAMLToken>"
           "<tt:KerberosToken>false</tt:KerberosToken><tt:RELToken>false</tt:RELToken></tt:Security>"
           "</tt:Device>"
           "<tt:Media><tt:XAddr>";
    xml += _media_service_url();
    xml += "</tt:XAddr>"
           "<tt:StreamingCapabilities><tt:RTPMulticast>false</tt:RTPMulticast>"
           "<tt:RTP_TCP>true</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP></tt:StreamingCapabilities>"
           "</tt:Media>"
           "</tds:Capabilities></tds:GetCapabilitiesResponse>";
    return xml;
}


static String _get_services(const char* request)
{
    String xml = "<tds:GetServicesResponse>"
                 "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace><tds:XAddr>";
    xml += NetManager::get_onvif_url();
    xml += "</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
           "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace><tds:XAddr>";
    xml += _media_service_url();
    xml += "</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
           "</tds:GetServicesResponse>";
    return xml;
}


static String _get_device_service_capabilities(const char* request)
{
    return "<tds:GetServiceCapabilitiesResponse><tds:Capabilities>"
           "<tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" IPVersion6=\"false\" DynDNS=\"false\" NTP=\"0\"/>"
           "<tds:Security TLS1.0=\"false\" TLS1.1=\"false\" TLS1.2=\"false\" OnboardKeyGeneration=\"false\" "
           "AccessPolicyConfig=\"false\" DefaultAccessPolicy=\"false\" Dot1X=\"false\" RemoteUserHandling=\"false\" "
           "X.509Token=\"false\" SAMLToken=\"false\" KerberosToken=\"false\" UsernameToken=\"true\" "
           "HttpDigest=\"false\" RELToken=\"false\"/>"
           "<tds:System DiscoveryResolve=\"false\" DiscoveryBye=\"false\" RemoteDiscovery=\"false\" "
           "SystemBackup=\"false\" SystemLogging=\"false\" FirmwareUpgrade=\"false\"/>"
           "</tds:Capabilities></tds:GetServiceCapabilitiesResponse>";
}


static String _get_device_information(const char* request)
{
    String xml = "<tds:GetDeviceInformationResponse>"
                 "<tds:Manufacturer>" APP_MANUFACTURER "</tds:Manufacturer>"
                 "<tds:Model>" APP_MODEL "</tds:Model>"
                 "<tds:FirmwareVersion>" APP_FIRMWARE_VERSION "</tds:FirmwareVersion>"
                 "<tds:SerialNumber>";
    xml += HAL::hal::GetHal()->getDeviceId();
    xml += "</tds:SerialNumber><tds:HardwareId>ESP32-S3</tds:HardwareId></tds:GetDeviceInformationResponse>";
    return xml;
}


static String _get_scopes(const char* request)
{
    String xml = "<tds:GetScopesResponse>";
    for (auto& scope : Onvif::get_scopes())
    {
        xml += "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>";
        xml += scope;
        xml += "</tt:ScopeItem></tds:Scopes>";
    }
    xml += "</tds:GetScopesResponse>";
    return xml;
}


static String _get_hostname(const char* request)
{
    String xml = "<tds:GetHostnameResponse><tds:HostnameInformation><tt:FromDHCP>false</tt:FromDHCP><tt:Name>";
    xml += HAL::hal::GetHal()->getConfig().hostname;
    xml += "</tt:Name></tds:HostnameInformation></tds:GetHostnameResponse>";
    return xml;
}


static String _get_network_interfaces(const char* request)
{
    IPAddress mask = NetManager::is_setup_mode() ? WiFi.softAPSubnetMask() : WiFi.subnetMask();
    uint32_t mask_bits = (uint32_t)mask;
    int prefix_length = 0;
    while (mask_bits)
    {
        prefix_length += mask_bits & 1;
        mask_bits >>= 1;
    }

    String xml = "<tds:GetNetworkInterfacesResponse><tds:NetworkInterfaces token=\"wlan0\">"
                 "<tt:Enabled>true</tt:Enabled><tt:Info><tt:Name>wlan0</tt:Name><tt:HwAddress>";
    xml += WiFi.macAddress();
    xml += "</tt:HwAddress><tt:MTU>1500</tt:MTU></tt:Info>"
           "<tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Config><tt:FromDHCP><tt:Address>";
    xml += NetManager::get_local_ip().toString();
    xml += "</tt:Address><tt:PrefixLength>";
    xml += prefix_length;
    xml += "</tt:PrefixLength></tt:FromDHCP><tt:DHCP>true</tt:DHCP></tt:Config></tt:IPv4>"
           "</tds:NetworkInterfaces></tds:GetNetworkInterfacesResponse>";
    return xml;
}


static String _get_dns(const char* request)
{
    String xml = "<tds:GetDNSResponse><tds:DNSInformation><tt:FromDHCP>true</tt:FromDHCP>"
                 "<tt:DNSFromDHCP><tt:Type>IPv4</tt:Type><tt:IPv4Address>";
    xml += WiFi.dnsIP().toString();
    xml += "</tt:IPv4Address></tt:DNSFromDHCP></tds:DNSInformation></tds:GetDNSResponse>";
    return xml;
}


static String _get_ntp(const char* request)
{
    return "<tds:GetNTPResponse><tds:NTPInformation><tt:FromDHCP>false</tt:FromDHCP>"
           "<tt:NTPManual><tt:Type>DNS</tt:Type><tt:DNSname>" APP_NTP_SERVER_1 "</tt:DNSname></tt:NTPManual>"
           "</tds:NTPInformation></tds:GetNTPResponse>";
}


static String _get_network_protocols(const char* request)
{
    char buffer[400];
    snprintf(buffer, sizeof(buffer),
             "<tds:GetNetworkProtocolsResponse>"
             "<tds:NetworkProtocols><tt:Name>HTTP</tt:Name><tt:Enabled>true</tt:Enabled><tt:Port>%d</tt:Port></tds:NetworkProtocols>"
             "<tds:NetworkProtocols><tt:Name>RTSP</tt:Name><tt:Enabled>true</tt:Enabled><tt:Port>%d</tt:Port></tds:NetworkProtocols>"
             "</tds:GetNetworkProtocolsResponse>",
             APP_HTTP_PORT, APP_RTSP_PORT);
    return buffer;
}


static String _get_discovery_mode(const char* request)
{
    return "<tds:GetDiscoveryModeResponse><tds:DiscoveryMode>Discoverable</tds:DiscoveryMode></tds:GetDiscoveryModeResponse>";
}


static String _get_users(const char* request)
{
    String xml = "<tds:GetUsersResponse><tds:User><tt:Username>";
    xml += HAL::hal::GetHal()->getConfig().auth_user;
    xml += "</tt:Username><tt:UserLevel>Administrator</tt:UserLevel></tds:User></tds:GetUsersResponse>";
    return xml;
}


static String _set_system_date_and_time(const char* request)
{
    // Clock comes from NTP
    return "<tds:SetSystemDateAndTimeResponse/>";
}


static String _system_reboot(const char* request)
{
    HAL::hal::GetHal()->requestReboot();
    return "<tds:SystemRebootResponse><tds:Message>Rebooting</tds:Message></tds:SystemRebootResponse>";
}


/* -------------------------------------------------------------------------- */
/*                                Media service                               */
/* -------------------------------------------------------------------------- */

static String _video_source_configuration(const char* tag, const VideoInfo_t& video)
{
    char buffer[400];
    snprintf(buffer, sizeof(buffer),
             "<%s token=\"vsc_1\"><tt:Name>VideoSourceConfig</tt:Name><tt:UseCount>1</tt:UseCount>"
             "<tt:SourceToken>vs_1</tt:SourceToken><tt:Bounds x=\"0\" y=\"0\" width=\"%d\" height=\"%d\"/></%s>",
             tag, video.width, video.height, tag);
    return buffer;
}


static String _video_encoder_configuration(const char* tag, const VideoInfo_t& video)
{
    char buffer[1024];
    snprintf(buffer, sizeof(buffer),
             "<%s token=\"vec_1\"><tt:Name>VideoEncoderConfig</tt:Name><tt:UseCount>1</tt:UseCount>"
             "<tt:Encoding>JPEG</tt:Encoding>"
             "<tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>"
             "<tt:Quality>%d</tt:Quality>"
             "<tt:RateControl><tt:FrameRateLimit>%d</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval>"
             "<tt:BitrateLimit>8000</tt:BitrateLimit></tt:RateControl>"
             "<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address>"
             "<tt:Port>0</tt:Port><tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast>"
             "<tt:SessionTimeout>PT60S</tt:SessionTimeout></%s>",
             tag, video.width, video.height, video.quality, video.fps, tag);
    return buffer;
}


static String _profile(const char* tag)
{
    VideoInfo_t video = _get_video_info();
    String xml = "<";
    xml += tag;
    xml += " token=\"profile_1\" fixed=\"true\"><tt:Name>MainStream</tt:Name>";
    xml += _video_source_configuration("tt:VideoSourceConfiguration", video);
    xml += _video_encoder_configuration("tt:VideoEncoderConfiguration", video);
    xml += "</";
    xml += tag;
    xml += ">";
    return xml;
}


static String _get_profiles(const char* request)
{
    return "<trt:GetProfilesResponse>" + _profile("trt:Profiles") + "</trt:GetProfilesResponse>";
}


static String _get_profile(const char* request)
{
    return "<trt:GetProfileResponse>" + _profile("trt:Profile") + "</trt:GetProfileResponse>";
}


static String _get_video_sources(const char* request)
{
    VideoInfo_t video = _get_video_info();
    char buffer[400];
    snprintf(buffer, sizeof(buffer),
             "<trt:GetVideoSourcesResponse><trt:VideoSources token=\"vs_1\"><tt:Framerate>%d</tt:Framerate>"
             "<tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>"
             "</trt:VideoSources></trt:GetVideoSourcesResponse>",
             video.fps, video.width, video.height);
    return buffer;
}


static String _get_video_source_configurations(const char* request)
{
    return "<trt:GetVideoSourceConfigurationsResponse>" +
           _video_source_configuration("trt:Configurations", _get_video_info()) +
           "</trt:GetVideoSourceConfigurationsResponse>";
}


static String _get_compatible_video_source_configurations(const char* request)
{
    return "<trt:GetCompatibleVideoSourceConfigurationsResponse>" +
           _video_source_configuration("trt:Configurations", _get_video_info()) +
           "</trt:GetCompatibleVideoSourceConfigurationsResponse>";
}


static String _get_video_source_configuration(const char* request)
{
    return "<trt:GetVideoSourceConfigurationResponse>" +
           _video_source_configuration("trt:Configuration", _get_video_info()) +
           "</trt:GetVideoSourceConfigurationResponse>";
}


static String _get_video_encoder_configurations(const char* request)
{
    return "<trt:GetVideoEncoderConfigurationsResponse>" +
           _video_encoder_configuration("trt:Configurations", _get_video_info()) +
           "</trt:GetVideoEncoderConfigurationsResponse>";
}


static String _get_compatible_video_encoder_configurations(const char* request)
{
    return "<trt:GetCompatibleVideoEncoderConfigurationsResponse>" +
           _video_encoder_configuration("trt:Configurations", _get_video_info()) +
           "</trt:GetCompatibleVideoEncoderConfigurationsResponse>";
}


static String _get_video_encoder_configuration(const char* request)
{
    return "<trt:GetVideoEncoderConfigurationResponse>" +
           _video_encoder_configuration("trt:Configuration", _get_video_info()) +
           "</trt:GetVideoEncoderConfigurationResponse>";
}


static String _get_video_encoder_configuration_options(const char* request)
{
    VideoInfo_t video = _get_video_info();
    char buffer[640];
    snprintf(buffer, sizeof(buffer),
             "<trt:GetVideoEncoderConfigurationOptionsResponse><trt:Options>"
             "<tt:QualityRange><tt:Min>0</tt:Min><tt:Max>57</tt:Max></tt:QualityRange>"
             "<tt:JPEG><tt:ResolutionsAvailable><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:ResolutionsAvailable>"
             "<tt:FrameRateRange><tt:Min>1</tt:Min><tt:Max>30</tt:Max></tt:FrameRateRange>"
             "<tt:EncodingIntervalRange><tt:Min>1</tt:Min><tt:Max>1</tt:Max></tt:EncodingIntervalRange></tt:JPEG>"
             "</trt:Options></trt:GetVideoEncoderConfigurationOptionsResponse>",
             video.width, video.height);
    return buffer;
}


static String _get_guaranteed_number_of_video_encoder_instances(const char* request)
{
    return "<trt:GetGuaranteedNumberOfVideoEncoderInstancesResponse><trt:TotalNumber>1</trt:TotalNumber>"
           "<trt:JPEG>1</trt:JPEG></trt:GetGuaranteedNumberOfVideoEncoderInstancesResponse>";
}


static String _set_video_encoder_configuration(const char* request)
{
    // Settings are changed from the web page, accept and ignore so NVR setup wizards don't fail
    return "<trt:SetVideoEncoderConfigurationResponse/>";
}


static String _get_stream_uri(const char* request)
{
    String xml = "<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>";
    xml += NetManager::get_rtsp_url();
    xml += "</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
           "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT0S</tt:Timeout>"
           "</trt:MediaUri></trt:GetStreamUriResponse>";
    return xml;
}


static String _get_snapshot_uri(const char* request)
{
    String xml = "<trt:GetSnapshotUriResponse><trt:MediaUri><tt:Uri>";
    xml += NetManager::get_snapshot_url();
    xml += "</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
           "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT0S</tt:Timeout>"
           "</trt:MediaUri></trt:GetSnapshotUriResponse>";
    return xml;
}


static String _get_media_service_capabilities(const char* request)
{
    return "<trt:GetServiceCapabilitiesResponse><trt:Capabilities SnapshotUri=\"true\" Rotation=\"false\" "
           "VideoSourceMode=\"false\" OSD=\"false\" EXICompression=\"false\">"
           "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"1\"/>"
           "<trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"true\" RTP_RTSP_TCP=\"true\" "
           "NonAggregateControl=\"false\" NoRTSPStreaming=\"false\"/>"
           "</trt:Capabilities></trt:GetServiceCapabilitiesResponse>";
}


static String _get_audio_sources(const char* request)
{
    return "<trt:GetAudioSourcesResponse/>";
}


static String _get_audio_encoder_configurations(const char* request)
{
    return "<trt:GetAudioEncoderConfigurationsResponse/>";
}


/* -------------------------------------------------------------------------- */
/*                                  Dispatch                                  */
/* -------------------------------------------------------------------------- */

typedef String (*SoapHandler_t)(const char* request);

struct SoapAction_t
{
    const char* name;
    SoapHandler_t handler;
};

// GetServiceCapabilities exists in both services and is dispatched by url
static const SoapAction_t _actions[] = {
    // Device
    {"GetSystemDateAndTime", _get_system_date_and_time},
    {"SetSystemDateAndTime", _set_system_date_and_time},
    {"GetCapabilities", _get_capabilities},
    {"GetServices", _get_services},
    {"GetDeviceInformation", _get_device_information},
    {"GetScopes", _get_scopes},
    {"GetHostname", _get_hostname},
    {"GetNetworkInterfaces", _get_network_interfaces},
    {"GetDNS", _get_dns},
    {"GetNTP", _get_ntp},
    {"GetNetworkProtocols", _get_network_protocols},
    {"GetDiscoveryMode", _get_discovery_mode},
    {"GetUsers", _get_users},
    {"SystemReboot", _system_reboot},
    // Media
    {"GetProfiles", _get_profiles},
    {"GetProfile", _get_profile},
    {"GetVideoSources", _get_video_sources},
    {"GetVideoSourceConfigurations", _get_video_source_configurations},
    {"GetVideoSourceConfiguration", _get_video_source_configuration},
    {"GetCompatibleVideoSourceConfigurations", _get_compatible_video_source_configurations},
    {"GetVideoEncoderConfigurations", _get_video_encoder_configurations},
    {"GetVideoEncoderConfiguration", _get_video_encoder_configuration},
    {"GetCompatibleVideoEncoderConfigurations", _get_compatible_video_encoder_configurations},
    {"GetVideoEncoderConfigurationOptions", _get_video_encoder_configuration_options},
    {"GetGuaranteedNumberOfVideoEncoderInstances", _get_guaranteed_number_of_video_encoder_instances},
    {"SetVideoEncoderConfiguration", _set_video_encoder_configuration},
    {"GetStreamUri", _get_stream_uri},
    {"GetSnapshotUri", _get_snapshot_uri},
    {"GetAudioSources", _get_audio_sources},
    {"GetAudioEncoderConfigurations", _get_audio_encoder_configurations},
};


// ONVIF core spec PRE_AUTH access class: allowed before logging in
static const char* _pre_auth_actions[] = {
    "GetSystemDateAndTime",
    "GetCapabilities",
    "GetServices",
    "GetServiceCapabilities",
    "GetHostname",
    "GetWsdlUrl",
    "GetEndpointReference",
};


static String _envelope(const String& body)
{
    String xml;
    xml.reserve(strlen(SOAP_ENVELOPE_BEGIN) + body.length() + strlen(SOAP_ENVELOPE_END));
    xml = SOAP_ENVELOPE_BEGIN;
    xml += body;
    xml += SOAP_ENVELOPE_END;
    return xml;
}


static bool _is_password_digest(const char* username_token)
{
    // Look at the Password start tag attributes, the default type is PasswordText
    const char* tag_end = Onvif::find_element(username_token, "Password");
    if (tag_end == nullptr)
        return false;
    const char* tag_start = tag_end;
    while (tag_start > username_token && *tag_start != '<')
        tag_start--;

    String tag;
    for (const char* p = tag_start; p < tag_end; p++)
        tag += *p;
    return tag.indexOf("PasswordDigest") >= 0;
}


// WS-Security UsernameToken, digest or plain text password
static bool _check_username_token(const char* request, const String& user, const String& password)
{
    const char* token = Onvif::find_element(request, "UsernameToken");
    if (token == nullptr)
        return false;

    if (Onvif::get_element_text(token, "Username") != user)
        return false;

    String token_password = Onvif::get_element_text(token, "Password");
    if (!_is_password_digest(token))
        return AuthUtils::secure_equals(token_password, password);

    String nonce = Onvif::get_element_text(token, "Nonce");
    String created = Onvif::get_element_text(token, "Created");
    String expected = AuthUtils::get_ws_password_digest(nonce, created, password);
    return expected != "" && AuthUtils::secure_equals(token_password, expected);
}


// Sends the error response itself when the request is not allowed
static bool _check_auth(AsyncWebServerRequest* request, const char* body, const String& action)
{
    auto& config = HAL::hal::GetHal()->getConfig();
    if (!HAL::hal::GetHal()->isAuthEnabled())
        return true;

    for (auto& i : _pre_auth_actions)
    {
        if (action == i)
            return true;
    }

    // WS-Security is what ONVIF clients normally use
    if (Onvif::find_element(body, "UsernameToken") != nullptr)
    {
        if (_check_username_token(body, config.auth_user, config.auth_password))
            return true;

        log_w("onvif %s: bad credentials", action.c_str());
        request->send(400, SOAP_CONTENT_TYPE,
                      _envelope(_fault("SOAP-ENV:Sender", "ter:NotAuthorized", "Sender not Authorized")));
        return false;
    }

    // Some clients fall back to http digest
    if (request->authenticate(config.auth_user.c_str(), config.auth_password.c_str(), APP_MODEL))
        return true;

    request->requestAuthentication(APP_MODEL, true);
    return false;
}


// Returns the http status code
static int _handle_soap(const String& url, const char* request, const String& action, String& response)
{
    String result;
    if (action == "GetServiceCapabilities")
    {
        String lower_url = url;
        lower_url.toLowerCase();
        result = lower_url.indexOf("media") >= 0 ? _get_media_service_capabilities(request) : _get_device_service_capabilities(request);
    }
    else
    {
        for (auto& i : _actions)
        {
            if (action == i.name)
            {
                result = i.handler(request);
                break;
            }
        }
    }

    int code = 200;
    if (result == "")
    {
        log_w("onvif action not supported: %s", action.c_str());
        result = _fault("SOAP-ENV:Receiver", "ter:ActionNotSupported", "Optional Action Not Implemented");
        code = 500;
    }

    response = _envelope(result);
    return code;
}


static void _on_soap_body(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total)
{
    if (total > SOAP_MAX_BODY_SIZE)
        return;

    // Freed by AsyncWebServerRequest
    if (index == 0 && request->_tempObject == nullptr)
        request->_tempObject = calloc(total + 1, 1);
    if (request->_tempObject == nullptr)
        return;

    memcpy((uint8_t*)request->_tempObject + index, data, len);
}


static void _on_soap_request(AsyncWebServerRequest* request)
{
    const char* body = (const char*)request->_tempObject;
    if (body == nullptr)
    {
        request->send(400);
        return;
    }

    String action = _get_action(body);
    log_i("onvif %s %s", request->url().c_str(), action.c_str());
    if (!_check_auth(request, body, action))
        return;

    String response;
    int code = _handle_soap(request->url(), body, action, response);
    request->send(code, SOAP_CONTENT_TYPE, response);
}


void Onvif::load_soap_apis(AsyncWebServer& server)
{
    // Matches /onvif/device_service, /onvif/media_service and whatever else clients make up
    server.on("/onvif", HTTP_POST, _on_soap_request, nullptr, _on_soap_body);
}
