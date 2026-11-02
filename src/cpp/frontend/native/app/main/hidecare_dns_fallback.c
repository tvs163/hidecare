// Hidecare: if DNS fails for the Hidecare server host, retry getaddrinfo()
// with CONFIG_HIDECARE_SERVER_IP_FALLBACK. Callers (esp-tls, OTA) keep the
// hostname for SNI and certificate checks; only the socket address changes.
#include <string.h>
#include <strings.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "lwip/netdb.h"

int __real_lwip_getaddrinfo(const char *nodename, const char *servname,
                            const struct addrinfo *hints, struct addrinfo **res);

#if defined(CONFIG_HIDECARE_SERVER_IP_FALLBACK) && defined(CONFIG_HIDECARE_SERVER_URL)
static int host_matches(const char *name)
{
    const char *url = CONFIG_HIDECARE_SERVER_URL;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t n = strcspn(p, ":/");
    return n > 0 && strlen(name) == n && strncasecmp(name, p, n) == 0;
}
#endif

int __wrap_lwip_getaddrinfo(const char *nodename, const char *servname,
                            const struct addrinfo *hints, struct addrinfo **res)
{
    int rc = __real_lwip_getaddrinfo(nodename, servname, hints, res);
#if defined(CONFIG_HIDECARE_SERVER_IP_FALLBACK) && defined(CONFIG_HIDECARE_SERVER_URL)
    if (rc != 0 && nodename && CONFIG_HIDECARE_SERVER_IP_FALLBACK[0] && host_matches(nodename)) {
        ESP_LOGW("hidecare", "DNS failed for %s (%d), using fallback %s",
                 nodename, rc, CONFIG_HIDECARE_SERVER_IP_FALLBACK);
        rc = __real_lwip_getaddrinfo(CONFIG_HIDECARE_SERVER_IP_FALLBACK, servname, hints, res);
    }
#endif
    return rc;
}
