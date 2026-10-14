#pragma once

#include <WiFiClientSecure.h>

/** TLS helpers: always verify server certificates (never setInsecure). */
namespace openai_tls
{

/** Google Trust Services roots used by api.openai.com (WE1 → GTS Root R4). */
const char *rootCertsPem();

/** Configure a WiFiClientSecure for verified HTTPS to api.openai.com. */
void configureClient(WiFiClientSecure &client);

} // namespace openai_tls
