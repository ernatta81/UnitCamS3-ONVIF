/**
 * @file auth_utils.h
 * @brief Credential checks shared by the RTSP and ONVIF servers
 * @version 0.1
 * @date 2026-10-29
 *
 */
#pragma once
#include <Arduino.h>

namespace AuthUtils
{
    // 32 lower case hex chars
    String new_nonce();

    // Value of a parameter in an "Authorization: Digest ..." header, quoted or not
    String get_digest_param(const char* header_value, const char* name);

    // header_value is what follows "Basic " / "Digest " in the Authorization header.
    // Digest: RFC 2617, with or without qop. nonce must match the one we handed out.
    bool check_basic(const char* header_value, const String& user, const String& password);
    bool check_digest(const char* header_value, const char* method, const char* realm, const char* nonce,
                      const String& user, const String& password);

    // WS-Security UsernameToken: Base64(SHA1(Base64Decode(nonce) + created + password))
    String get_ws_password_digest(const String& nonce_base64, const String& created, const String& password);

    // Compare without leaking the position of the first mismatch through timing
    bool secure_equals(const String& a, const String& b);
}
