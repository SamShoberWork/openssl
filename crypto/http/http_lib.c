/*
 * Copyright 2001-2021 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <openssl/http.h>
#include <openssl/httperr.h>
#include <openssl/bio.h> /* for BIO_snprintf() */
#include <openssl/err.h>
#include <string.h>
#include "internal/cryptlib.h" /* for ossl_assert() */

#include "http_local.h"

static void init_pstring(char **pstr)
{
    if (pstr != NULL) {
        *pstr = NULL;
    }
}

static int copy_substring(char **dest, const char *start, const char *end)
{
    return dest == NULL
        || (*dest = OPENSSL_strndup(start, end - start)) != NULL;
}

static void free_pstring(char **pstr)
{
    if (pstr != NULL) {
        OPENSSL_free(*pstr);
        *pstr = NULL;
    }
}

int OSSL_parse_url(const char *url, char **pscheme, char **puser, char **phost,
                   char **pport, int *pport_num,
                   char **ppath, char **pquery, char **pfrag)
{
    const char *p, *tmp;
    const char *scheme, *scheme_end;
    const char *user, *user_end;
    const char *host, *host_end;
    const char *port, *port_end;
    unsigned int portnum;
    const char *path, *path_end;
    const char *query, *query_end;
    const char *frag, *frag_end;

    init_pstring(pscheme);
    init_pstring(puser);
    init_pstring(phost);
    init_pstring(pport);
    init_pstring(ppath);
    init_pstring(pfrag);
    init_pstring(pquery);

    if (url == NULL) {
        ERR_raise(ERR_LIB_HTTP, ERR_R_PASSED_NULL_PARAMETER);
        return 0;
    }

    /* check for optional prefix "<scheme>://" */
    scheme = scheme_end = url;
    p = strstr(url, "://");
    if (p == NULL) {
        p = url;
    } else {
        scheme_end = p;
        if (scheme_end == scheme)
            goto parse_err;
        p += strlen("://");
    }

    /* parse optional "userinfo@" */
    user = user_end = host = p;
    host = strchr(p, '@');
    if (host != NULL)
        user_end = host++;
    else
        host = p;

    /* parse host name/address as far as needed here */
    if (host[0] == '[') {
        /* ipv6 literal, which may include ':' */
        host++;
        host_end = strchr(host, ']');
        if (host_end == NULL)
            goto parse_err;
        p = host_end + 1;
    } else {
        /* look for start of optional port, path, query, or fragment */
        host_end = strchr(host, ':');
        if (host_end == NULL)
            host_end = strchr(host, '/');
        if (host_end == NULL)
            host_end = strchr(host, '?');
        if (host_end == NULL)
            host_end = strchr(host, '#');
        if (host_end == NULL) /* the remaining string is just the hostname */
            host_end = host + strlen(host);
        p = host_end;
    }

    /* parse optional port specification starting with ':' */
    port = "0"; /* default */
    if (*p == ':')
        port = ++p;
    /* remaining port spec handling is also done for the default values */
    /* make sure a decimal port number is given */
    if (!sscanf(port, "%u", &portnum) || portnum > 65535) {
        ERR_raise(ERR_LIB_HTTP, HTTP_R_INVALID_PORT_NUMBER);
        goto err;
    }
    for (port_end = port; '0' <= *port_end && *port_end <= '9'; port_end++)
        ;
    if (port == p) /* port was given explicitly */
        p += port_end - port;

    /* check for optional path starting with '/' or '?'. Else must start '#' */
    path = p;
    if (*path != '\0' && *path != '/' && *path != '?' && *path != '#') {
        ERR_raise(ERR_LIB_HTTP, HTTP_R_INVALID_URL_PATH);
        goto parse_err;
    }
    path_end = query = query_end = frag = frag_end = path + strlen(path);

    /* parse optional "?query" */
    tmp = strchr(p, '?');
    if (tmp != NULL) {
        p = tmp;
        if (pquery != NULL) {
            path_end = p;
            query = p + 1;
        }
    }

    /* parse optional "#fragment" */
    tmp = strchr(p, '#');
    if (tmp != NULL) {
        if (query == path_end) /* we did not record a query component */
            path_end = tmp;
        query_end = tmp;
        frag = tmp + 1;
    }

    if (!copy_substring(pscheme, scheme, scheme_end)
            || !copy_substring(phost, host, host_end)
            || !copy_substring(pport, port, port_end)
            || !copy_substring(puser, user, user_end)
            || !copy_substring(pquery, query, query_end)
            || !copy_substring(pfrag, frag, frag_end))
        goto err;
    if (pport_num != NULL)
        *pport_num = (int)portnum;
    if (*path == '/') {
        if (!copy_substring(ppath, path, path_end))
            goto err;
    } else if (ppath != NULL) { /* must prepend '/' */
        size_t buflen = 1 + path_end - path + 1;

        if ((*ppath = OPENSSL_malloc(buflen)) == NULL)
            goto err;
        BIO_snprintf(*ppath, buflen, "/%s", path);
    }
    return 1;

 parse_err:
    ERR_raise(ERR_LIB_HTTP, HTTP_R_ERROR_PARSING_URL);

 err:
    free_pstring(pscheme);
    free_pstring(puser);
    free_pstring(phost);
    free_pstring(pport);
    free_pstring(ppath);
    free_pstring(pquery);
    free_pstring(pfrag);
    return 0;
}

int OSSL_HTTP_parse_url(const char *url, int *pssl, char **puser, char **phost,
                        char **pport, int *pport_num,
                        char **ppath, char **pquery, char **pfrag)
{
    char *scheme, *port;
    int ssl = 0, portnum;

    init_pstring(pport);
    if (pssl != NULL)
        *pssl = 0;
    if (!OSSL_parse_url(url, &scheme, puser, phost, &port, pport_num,
                        ppath, pquery, pfrag))
        return 0;

    /* check for optional HTTP scheme "http[s]" */
    if (strcmp(scheme, OSSL_HTTPS_NAME) == 0) {
        ssl = 1;
        if (pssl != NULL)
            *pssl = ssl;
    } else if (*scheme != '\0' && strcmp(scheme, OSSL_HTTP_NAME) != 0) {
        ERR_raise(ERR_LIB_HTTP, HTTP_R_INVALID_URL_SCHEME);
        OPENSSL_free(scheme);
        OPENSSL_free(port);
        goto err;
    }
    OPENSSL_free(scheme);

    if (strcmp(port, "0") == 0) {
        /* set default port */
        OPENSSL_free(port);
        port = ssl ? OSSL_HTTPS_PORT : OSSL_HTTP_PORT;
        if (!ossl_assert(sscanf(port, "%d", &portnum) == 1))
            goto err;
        if (pport_num != NULL)
            *pport_num = portnum;
        if (pport != NULL) {
            *pport = OPENSSL_strdup(port);
            if (*pport == NULL)
                goto err;
        }
    } else {
        if (pport != NULL)
            *pport = port;
        else
            OPENSSL_free(port);
    }
    return 1;

 err:
    free_pstring(puser);
    free_pstring(phost);
    free_pstring(ppath);
    free_pstring(pquery);
    free_pstring(pfrag);
    return 0;
}

int ossl_http_use_proxy(const char *no_proxy, const char *server)
{
    size_t sl;
    const char *found = NULL;

    if (!ossl_assert(server != NULL))
        return 0;
    sl = strlen(server);

    /*
     * using environment variable names, both lowercase and uppercase variants,
     * compatible with other HTTP client implementations like wget, curl and git
     */
    if (no_proxy == NULL)
        no_proxy = getenv("no_proxy");
    if (no_proxy == NULL)
        no_proxy = getenv(OPENSSL_NO_PROXY);
    if (no_proxy != NULL)
        found = strstr(no_proxy, server);
    while (found != NULL
           && ((found != no_proxy && found[-1] != ' ' && found[-1] != ',')
               || (found[sl] != '\0' && found[sl] != ' ' && found[sl] != ',')))
        found = strstr(found + 1, server);
    return found == NULL;
}

const char *ossl_http_adapt_proxy(const char *proxy, const char *no_proxy,
                                  const char *server, int use_ssl)
{
    const int http_len = strlen(OSSL_HTTP_PREFIX);
    const int https_len = strlen(OSSL_HTTPS_PREFIX);

    /*
     * using environment variable names, both lowercase and uppercase variants,
     * compatible with other HTTP client implementations like wget, curl and git
     */
    if (proxy == NULL)
        proxy = getenv(use_ssl ? "https_proxy" : "http_proxy");
    if (proxy == NULL)
        proxy = getenv(use_ssl ? OPENSSL_HTTP_PROXY :
                       OPENSSL_HTTPS_PROXY);
    if (proxy == NULL)
        return NULL;

    /* skip any leading "http://" or "https://" */
    if (strncmp(proxy, OSSL_HTTP_PREFIX, http_len) == 0)
        proxy += http_len;
    else if (strncmp(proxy, OSSL_HTTPS_PREFIX, https_len) == 0)
        proxy += https_len;

    if (*proxy == '\0' || !ossl_http_use_proxy(no_proxy, server))
        return NULL;
    return proxy;
}
