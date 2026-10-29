/**
 * @file auth_utils.cpp
 * @brief
 * @version 0.1
 * @date 2026-10-29
 *
 */
#include "auth_utils.h"
#include <MD5Builder.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <vector>


static String _md5_hex(const String& input)
{
    MD5Builder md5;
    md5.begin();
    md5.add(input);
    md5.calculate();
    return md5.toString();
}


static bool _base64_decode(const String& input, std::vector<uint8_t>& output)
{
    size_t len = 0;
    output.resize(input.length());
    if (mbedtls_base64_decode(output.data(), output.size(), &len, (const uint8_t*)input.c_str(), input.length()) != 0)
        return false;
    output.resize(len);
    return true;
}


static String _base64_encode(const uint8_t* data, size_t data_len)
{
    std::vector<uint8_t> output(4 * ((data_len + 2) / 3) + 1);
    size_t len = 0;
    if (mbedtls_base64_encode(output.data(), output.size(), &len, data, data_len) != 0)
        return "";
    output[len] = 0;
    return (const char*)output.data();
}


String AuthUtils::new_nonce()
{
    char buffer[33];
    snprintf(buffer, sizeof(buffer), "%08x%08x%08x%08x",
             (unsigned)esp_random(), (unsigned)esp_random(), (unsigned)esp_random(), (unsigned)esp_random());
    return buffer;
}


String AuthUtils::get_digest_param(const char* header_value, const char* name)
{
    size_t name_len = strlen(name);
    const char* p = header_value;
    while ((p = strstr(p, name)) != nullptr)
    {
        // Whole names only, "nonce" must not match "cnonce"
        bool is_boundary = p == header_value || p[-1] == ' ' || p[-1] == ',' || p[-1] == '\t';
        const char* value = p + name_len;
        while (*value == ' ')
            value++;
        if (is_boundary && *value == '=')
        {
            value++;
            while (*value == ' ')
                value++;

            String result;
            if (*value == '"')
            {
                value++;
                while (*value != 0 && *value != '"')
                    result += *value++;
            }
            else
            {
                while (*value != 0 && *value != ',' && *value != ' ' && *value != '\r' && *value != '\n')
                    result += *value++;
            }
            return result;
        }
        p += name_len;
    }
    return "";
}


bool AuthUtils::check_basic(const char* header_value, const String& user, const String& password)
{
    std::vector<uint8_t> decoded;
    String value = header_value;
    value.trim();
    if (!_base64_decode(value, decoded))
        return false;

    String credentials;
    credentials.reserve(decoded.size());
    for (auto c : decoded)
        credentials += (char)c;
    return secure_equals(credentials, user + ":" + password);
}


bool AuthUtils::check_digest(const char* header_value, const char* method, const char* realm, const char* nonce,
                             const String& user, const String& password)
{
    if (get_digest_param(header_value, "username") != user)
        return false;
    if (get_digest_param(header_value, "realm") != realm)
        return false;
    String header_nonce = get_digest_param(header_value, "nonce");
    if (header_nonce == "" || header_nonce != nonce)
        return false;

    String ha1 = _md5_hex(user + ":" + realm + ":" + password);
    String ha2 = _md5_hex(String(method) + ":" + get_digest_param(header_value, "uri"));

    String qop = get_digest_param(header_value, "qop");
    String expected;
    if (qop == "")
    {
        expected = _md5_hex(ha1 + ":" + header_nonce + ":" + ha2);
    }
    else
    {
        expected = _md5_hex(ha1 + ":" + header_nonce + ":" + get_digest_param(header_value, "nc") + ":" +
                            get_digest_param(header_value, "cnonce") + ":" + qop + ":" + ha2);
    }

    String response = get_digest_param(header_value, "response");
    response.toLowerCase();
    return secure_equals(response, expected);
}


String AuthUtils::get_ws_password_digest(const String& nonce_base64, const String& created, const String& password)
{
    std::vector<uint8_t> input;
    if (!_base64_decode(nonce_base64, input))
        return "";
    input.insert(input.end(), created.c_str(), created.c_str() + created.length());
    input.insert(input.end(), password.c_str(), password.c_str() + password.length());

    uint8_t sha1[20];
    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), input.data(), input.size(), sha1) != 0)
        return "";
    return _base64_encode(sha1, sizeof(sha1));
}


bool AuthUtils::secure_equals(const String& a, const String& b)
{
    if (a.length() != b.length())
        return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.length(); i++)
        diff |= a[i] ^ b[i];
    return diff == 0;
}
