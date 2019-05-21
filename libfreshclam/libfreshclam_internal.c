/*
 *  Copyright (C) 2013-2019 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 *  Copyright (C) 2007-2013 Sourcefire, Inc.
 *  Copyright (C) 2002-2007 Tomasz Kojm <tkojm@clamav.net>
 *
 *  HTTP/1.1 compliance by Arkadiusz Miskiewicz <misiek@pld.org.pl>
 *  Proxy support by Nigel Horne <njh@bandsman.co.uk>
 *  Proxy authorization support by Gernot Tenchio <g.tenchio@telco-tech.de>
 *		     (uses fmt_base64() from libowfat (http://www.fefe.de))
 *
 *  CDIFF code (C) 2006 Sensory Networks, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

/* for strptime, it is POSIX, but defining _XOPEN_SOURCE to 600
 * fails on Solaris because it would require a c99 compiler,
 * 500 fails completely on Solaris, and FreeBSD, and w/o _XOPEN_SOURCE
 * strptime is not defined on Linux */
#define __EXTENSIONS

#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <string.h>
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif
#include <ctype.h>
#ifndef _WIN32
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif
#include <sys/types.h>
#include <time.h>
#include <fcntl.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <zlib.h>

#ifdef _WIN32
#include <wincrypt.h>
#endif

#include <curl/curl.h>

#include "target.h"

#include "libfreshclam.h"
#include "libfreshclam_internal.h"
#include "dns.h"

#include "shared/optparser.h"
#include "shared/output.h"
#include "shared/cdiff.h"
#include "shared/tar.h"
#include "shared/clamdcom.h"

#include "libclamav/clamav.h"
#include "libclamav/others.h"
#include "libclamav/str.h"
#include "libclamav/cvd.h"
#include "libclamav/regex_list.h"

#define DB_FILENAME_MAX 60
#define CVD_HEADER_SIZE 512

/*
 * Globals
 */
/* Callback function pointers */
fccb_download_complete g_cb_download_complete = NULL;

/* Configuration options */
char *g_localIP   = NULL;
char *g_userAgent = NULL;

char *g_proxyServer   = NULL;
uint16_t g_proxyPort  = 0;
char *g_proxyUsername = NULL;
char *g_proxyPassword = NULL;

char *g_tempDirectory     = NULL;
char *g_databaseDirectory = NULL;

uint32_t g_maxAttempts    = 0;
uint32_t g_connectTimeout = 0;
uint32_t g_requestTimeout = 0;

uint32_t g_bCompressLocalDatabase = 0;

/**
 * @brief Get DNS text record field # for official databases.
 *
 * @param database  Official database name.
 * @return int      DNS text record field #
 */
static int textrecordfield(const char *database)
{
    if (!strcmp(database, "main")) {
        return 1;
    } else if (!strcmp(database, "daily")) {
        return 2;
    } else if (!strcmp(database, "bytecode")) {
        return 7;
    } else if (!strcmp(database, "safebrowsing")) {
        return 6;
    }
    return 0;
}

#ifdef _WIN32
CURLcode sslctx_function(CURL *curl, void *ssl_ctx, void *userptr)
{
    CURLcode status = CURLE_BAD_FUNCTION_ARGUMENT;

    uint32_t numCertificatesFound = 0;
    DWORD lastError;

    HCERTSTORE hStore              = NULL;
    PCCERT_CONTEXT pWinCertContext = NULL;
    X509 *x509                     = NULL;
    X509_STORE *store              = SSL_CTX_get_cert_store((SSL_CTX *)ssl_ctx);

    hStore = CertOpenSystemStoreA(NULL, "ROOT");

    if (NULL == hStore) {
        logg("!Failed to open system certificate store.\n");
        goto done;
    }

    while (NULL != (pWinCertContext = CertEnumCertificatesInStore(hStore, pWinCertContext))) {
        int addCertResult                 = 0;
        const unsigned char *encoded_cert = pWinCertContext->pbCertEncoded;

        x509 = NULL;
        x509 = d2i_X509(NULL, &encoded_cert, pWinCertContext->cbCertEncoded);
        if (NULL == x509) {
            logg("!Failed to convert system certificate to x509.\n");
            continue;
        }

        addCertResult = X509_STORE_add_cert(store, x509);
        if (1 != addCertResult) {
            logg("!Failed to add x509 certificate to openssl certificate store.\n");
            continue;
        }

        if (logg_verbose) {
            char *issuer     = NULL;
            size_t issuerLen = 0;
            issuerLen        = CertGetNameStringA(pWinCertContext, CERT_NAME_FRIENDLY_DISPLAY_TYPE, CERT_NAME_ISSUER_FLAG, NULL, NULL, 0);

            issuer = cli_malloc(issuerLen);
            if (NULL == issuer) {
                logg("!Failed to allocate memory for certificate name.\n");
                status = CURLE_OUT_OF_MEMORY;
                goto done;
            }

            if (0 == CertGetNameStringA(pWinCertContext, CERT_NAME_FRIENDLY_DISPLAY_TYPE, CERT_NAME_ISSUER_FLAG, NULL, issuer, issuerLen)) {
                logg("!Failed to get friendly display name for certificate.\n");
            } else {
                logg("Certificate loaded from Windows certificate store: %s\n", issuer);
            }

            free(issuer);
        }

        numCertificatesFound++;
        X509_free(x509);
    }

    lastError = GetLastError();
    switch (lastError) {
        case E_INVALIDARG:
            logg("!The handle in the hCertStore parameter is not the same as that in the certificate context pointed to by pPrevCertContext.\n");
            break;
        case CRYPT_E_NOT_FOUND:
        case ERROR_NO_MORE_FILES:
            if (0 == numCertificatesFound) {
                logg("!No certificates were found.\n");
            }
            break;
        default:
            logg("!Unexpected error code from CertEnumCertificatesInStore()\n");
    }

done:

    if (NULL != pWinCertContext) {
        CertFreeCertificateContext(pWinCertContext);
    }
    if (NULL != hStore) {
        CertCloseStore(hStore, 0);
    }

    status = CURLE_OK;
    return status;
}
#endif

static fc_error_t create_curl_handle(
    int bHttp,
    int bAllowRedirect,
    CURL **curlHandle)
{
    fc_error_t status = FC_EARG;

    CURL *curl        = NULL;
    CURLcode curl_ret = CURLE_OK;

    char userAgent[128];

    if (NULL == curlHandle) {
        logg("!create_curl_handle: Invalid arguments!\n");
        goto done;
    }

    *curlHandle = NULL;

    curl = curl_easy_init();
    if (NULL == curl) {
        logg("!create_curl_handle: curl_easy_init failed!\n");
        status = FC_EINIT;
        goto done;
    }

    if (g_userAgent)
        strncpy(userAgent, g_userAgent, sizeof(userAgent));
    else
        snprintf(userAgent, sizeof(userAgent),
                 PACKAGE "/%s (OS: " TARGET_OS_TYPE ", ARCH: " TARGET_ARCH_TYPE ", CPU: " TARGET_CPU_TYPE ")",
                 get_version());
    userAgent[sizeof(userAgent) - 1] = 0;

    if (mprintf_verbose) {
        /* ask libcurl to show us the verbose output */
        if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L)) {
            logg("!create_curl_handle: Failed to set CURLOPT_VERBOSE!\n");
        }
        if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_STDERR, stdout)) {
            logg("!create_curl_handle: Failed to direct curl debug output to stdout!\n");
        }
    }

    if (bHttp) {
        if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent)) {
            logg("!create_curl_handle: Failed to set CURLOPT_USERAGENT (%s)!\n", userAgent);
        }
        if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, g_connectTimeout)) {
            logg("!create_curl_handle: Failed to set CURLOPT_CONNECTTIMEOUT (%u)!\n", g_connectTimeout);
        }
        if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_TIMEOUT, g_requestTimeout)) {
            logg("!create_curl_handle: Failed to set CURLOPT_TIMEOUT (%u)!\n", g_requestTimeout);
        }

        if (bAllowRedirect) {
            /* allow three redirects */
            if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L)) {
                logg("!create_curl_handle: Failed to set CURLOPT_FOLLOWLOCATION!\n");
            }
            if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L)) {
                logg("!create_curl_handle: Failed to set CURLOPT_MAXREDIRS!\n");
            }
        }
    }

    if (g_localIP) {
        if (NULL == strchr(g_localIP, ':')) {
#ifdef CURLOPT_DNS_LOCAL_IP4
            logg("*Local IPv4 address requested: %s\n", g_localIP);
            curl_ret = curl_easy_setopt(curl, CURLOPT_DNS_LOCAL_IP4, g_localIP); // Option requires libcurl built with c-ares
            switch (curl_ret) {
                case CURLE_BAD_FUNCTION_ARGUMENT:
                    logg("!create_curl_handle: Unable to bind DNS resolves to %s. Invalid IPv4 address.\n", g_localIP);
                    status = FC_ECONFIG;
                    goto done;
                    break;
                case CURLE_UNKNOWN_OPTION:
#ifdef CURLE_NOT_BUILT_IN
                case CURLE_NOT_BUILT_IN:
                    logg("!create_curl_handle: Unable to bind DNS resolves to %s. Option requires that libcurl was built with c-ares.\n", g_localIP);
                    status = FC_ECONFIG;
                    goto done;
#endif
                default:
                    break;
            }
            if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4)) {
                logg("!create_curl_handle: Failed to set CURLOPT_IPRESOLVE (IPv4)!\n");
            }
#endif
        } else {
#ifdef CURLOPT_DNS_LOCAL_IP6
            logg("*Local IPv6 address requested: %s\n", g_localIP);
            curl_ret = curl_easy_setopt(curl, CURLOPT_DNS_LOCAL_IP6, g_localIP); // Option requires libcurl built with c-ares
            switch (curl_ret) {
                case CURLE_BAD_FUNCTION_ARGUMENT:
                    logg("^create_curl_handle: Unable to bind DNS resolves to %s. Invalid IPv4 address.\n", g_localIP);
                    status = FC_ECONFIG;
                    goto done;
                    break;
                case CURLE_UNKNOWN_OPTION:
#ifdef CURLE_NOT_BUILT_IN
                case CURLE_NOT_BUILT_IN:
                    logg("^create_curl_handle: Unable to bind DNS resolves to %s. Option requires that libcurl was built with c-ares.\n", g_localIP);
                    status = FC_ECONFIG;
                    goto done;
#endif
                default:
                    break;
            }
            if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V6)) {
                logg("!create_curl_handle: Failed to set CURLOPT_IPRESOLVE (IPv6)!\n");
            }
#endif
        }
    }
    if (g_proxyServer) {
        /*
         * Proxy requested.
         */
        logg("*Using proxy: %s:%u\n", g_proxyServer, g_proxyPort);

        if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_PROXY, g_proxyServer)) {
            logg("!create_curl_handle: Failed to set CURLOPT_PROXY (%s)!\n", g_proxyServer);
        }
        if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_PROXYPORT, g_proxyPort)) {
            logg("!create_curl_handle: Failed to set CURLOPT_PROXYPORT (%u)!\n", g_proxyPort);
        }
        if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1L)) { // Necessary?
            logg("!create_curl_handle: Failed to set CURLOPT_HTTPPROXYTUNNEL (1)!\n");
        }
#ifdef CURLOPT_SUPPRESS_CONNECT_HEADERS
        if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L)) { // Necessary?
            logg("!create_curl_handle: Failed to set CURLOPT_SUPPRESS_CONNECT_HEADERS (1)!\n");
        }
#endif

        if (g_proxyUsername) {
            if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME, g_proxyUsername)) {
                logg("!create_curl_handle: Failed to set CURLOPT_PROXYUSERNAME (%s)!\n", g_proxyUsername);
            }
            if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD, g_proxyPassword)) {
                logg("!create_curl_handle: Failed to set CURLOPT_PROXYPASSWORD (%s)!\n", g_proxyPassword);
            }
        }
    }

#ifdef _WIN32
    if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, *sslctx_function)) {
        logg("!create_curl_handle: Failed to set SSL CTX function!\n");
    }
#endif

    *curlHandle = curl;
    status      = FC_SUCCESS;

done:

    if (FC_SUCCESS != status) {
        if (NULL != curl) {
            curl_easy_cleanup(curl);
        }
    }

    return status;
}

struct MemoryStruct {
    char *buffer;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t real_size                  = size * nmemb;
    struct MemoryStruct *receivedData = (struct MemoryStruct *)userp;

    if ((NULL == contents) || (NULL == userp)) {
        return 0;
    }

    char *newBuffer = realloc(receivedData->buffer, receivedData->size + real_size + 1);
    if (NULL == newBuffer) {
        logg("!remote_cvdhead - recv callback: Failed to allocate memory CVD header data.\n");
        return 0;
    }

    receivedData->buffer = newBuffer;
    memcpy(&(receivedData->buffer[receivedData->size]), contents, real_size);
    receivedData->size += real_size;
    receivedData->buffer[receivedData->size] = 0;

    return real_size;
}

struct FileStruct {
    int handle;
    size_t size;
};

static size_t WriteFileCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t real_size                = size * nmemb;
    struct FileStruct *receivedFile = (struct FileStruct *)userp;
    size_t bytes_written            = 0;

    if ((NULL == contents) || (NULL == userp)) {
        return 0;
    }

    bytes_written = write(receivedFile->handle, contents, real_size);

    receivedFile->size += bytes_written;

    return bytes_written;
}

/**
 * @brief Get the cvd header info struct for the newest available database.
 *
 * The last-modified datetime will be used to set the If-Modified-Since header.
 * If the remote CVD isn't newer, we should get an HTTP 304 and return
 * FC_UPTODATE instead of FC_SUCCESS, and cvd will be NULL.
 *
 * @param cvdfile           database name including extension.
 * @param ifModifiedSince   modified time of local database. May be 0 to always get the CVD header.
 * @param server            server to use to retrieve for database header.
 * @param logerr            non-zero to upgrade warnings to errors.
 * @param cvd               [out] CVD header of newest available CVD, if FC_SUCCESS
 * @return fc_error_t       FC_SUCCESS if CVD header obtained.
 * @return fc_error_t       FC_UPTODATE if received 304 in response to ifModifiedSince date.
 * @return fc_error_t       Another error code if failure occured.
 */
static fc_error_t remote_cvdhead(
    const char *cvdfile,
    uint32_t ifModifiedSince,
    char *server,
    int logerr,
    struct cl_cvd **cvd)
{
    fc_error_t ret;
    fc_error_t status = FC_EARG;

    int bHttpServer = 0;
    char *url       = NULL;
    size_t urlLen   = 0;

    char head[CVD_HEADER_SIZE + 1];

    struct MemoryStruct receivedData = {0};

    unsigned int i;
    struct cl_cvd *cvdhead;

    CURL *curl = NULL;
    CURLcode curl_ret;
    char errbuf[CURL_ERROR_SIZE];
    struct curl_slist *slist = NULL;

    long http_code = 0;

    if (NULL == cvd) {
        logg("!remote_cvdhead: Invalid arguments.\n");
        goto done;
    }

    *cvd = NULL;

    if (0 == strncasecmp(server, "http", strlen("http"))) {
        bHttpServer = 1;
    }

    /*
     * Request CVD header.
     */
    logg("Reading CVD header (%s): ", cvdfile);

    urlLen = strlen(server) + strlen("/") + strlen(cvdfile);
    url    = malloc(urlLen + 1);
    snprintf(url, urlLen + 1, "%s/%s", server, cvdfile);

    logg("*Trying to retrieve CVD header from %s\n", url);

    if (FC_SUCCESS != (ret = create_curl_handle(
                           bHttpServer, // Set extra HTTP-specific headers.
                           1,           // Allow redirects.
                           &curl))) {   // [out] curl session handle.
        logg("!remote_cvdhead: Failed to create curl handle.\n");
        status = ret;
        goto done;
    }

    if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_URL, url)) {
        logg("!remote_cvdhead: Failed to set CURLOPT_URL for curl session (%s).\n", url);
        status = FC_EFAILEDGET;
        goto done;
    }

    if (bHttpServer) {
        /*
         * For HTTP, set some extra headers.
         */
        struct curl_slist *temp = NULL;

        if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L)) {
            logg("!remote_cvdhead: Failed to set CURLOPT_HTTPGET for curl session.\n");
        }

#ifdef FRESHCLAM_NO_CACHE
        if (NULL == (temp = curl_slist_append(slist, "Cache-Control: no-cache"))) { // Necessary?
            logg("!remote_cvdhead: Failed to append \"Cache-Control: no-cache\" header to custom curl header list.\n");
        } else {
            slist = temp;
        }
#endif
        if (NULL == (temp = curl_slist_append(slist, "Connection: close"))) {
            logg("!remote_cvdhead: Failed to append \"Connection: close\" header to custom curl header list.\n");
        } else {
            slist = temp;
        }
        if (NULL != slist) {
            if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist)) {
                logg("!remote_cvdhead: Failed to add custom header list to curl session.\n");
            }
        }
    }

    if (0 != ifModifiedSince) {
        if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_TIMEVALUE, ifModifiedSince)) {
            logg("!remote_cvdhead: Failed to set if-Modified-Since time value for curl session.\n");
        }
        /* If-Modified-Since the above time stamp */
        else if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE)) {
            logg("!remote_cvdhead: Failed to set if-Modified-Since time condition for curl session.\n");
        }
    }

    /* Request only the first 512 bytes (CVD_HEADER_SIZE) */
    if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_RANGE, "0-511")) {
        logg("!remote_cvdhead: Failed to set CURLOPT_RANGE CVD_HEADER_SIZE for curl session.\n");
    }

    receivedData.buffer = cli_malloc(1); /* will be grown as needed by the realloc above */
    receivedData.size   = 0;             /* no data at this point */

    /* Send all data to this function  */
    if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback)) {
        logg("!remote_cvdhead: Failed to set write-data memory callback function for curl session.\n");
    }

    /* Pass our 'receivedData' struct to the callback function */
    if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&receivedData)) {
        logg("!remote_cvdhead: Failed to set receivedData struct for write-data callback function for curl session.\n");
    }

    /*
     * Perform download.
     */
    memset(errbuf, 0, sizeof(errbuf));
    curl_ret = curl_easy_perform(curl);
    if (curl_ret != CURLE_OK) {
        /*
         * Show the error information.
         * If no detailed error information was written to errbuf
         * show the more generic information from curl_easy_strerror instead.
         */
        size_t len = strlen(errbuf);
        logg("%cremote_cvdhead: Download failed (%d) ", logerr ? '!' : '^', curl_ret);
        if (len)
            logg("%c Message: %s%s", logerr ? '!' : '^', errbuf, ((errbuf[len - 1] != '\n') ? "\n" : ""));
        else
            logg("%c Message: %s\n", logerr ? '!' : '^', curl_easy_strerror(curl_ret));
        status = FC_ECONNECTION;
        goto done;
    }

    /* Check HTTP code */
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    switch (http_code) {
        case 200:
        case 206: {
            status = FC_SUCCESS;
            break;
        }
        case 304: {
            status = FC_UPTODATE;
            goto done;
        }
        case 404: {
            if (g_proxyServer)
                logg("^remote_cvdhead: file not found: %s (Proxy: %s:%u)\n", url, g_proxyServer, g_proxyPort);
            else
                logg("^remote_cvdhead: file not found: %s\n", url);
            status = FC_EFAILEDGET;
            goto done;
        }
        case 522: {
            logg("^remote_cvdhead: Origin Connection Time-out. Cloudflare was unable to reach the origin web server and the request timed out. URL: %s\n", url);
            status = FC_EFAILEDGET;
            goto done;
        }
        default: {
            if (g_proxyServer)
                logg("%cremote_cvdhead: Unexpected response (%li) from %s (Proxy: %s:%u)\n",
                     logerr ? '!' : '^', http_code, server, g_proxyServer, g_proxyPort);
            else
                logg("%cremote_cvdhead: Unexpected response (%li) from %s\n",
                     logerr ? '!' : '^', http_code, server);
            status = FC_EFAILEDGET;
            goto done;
        }
    }

    /*
     * Identify start of CVD header in response body.
     */
    if (receivedData.size < CVD_HEADER_SIZE) {
        logg("%cremote_cvdhead: Malformed CVD header (too short)\n", logerr ? '!' : '^');
        status = FC_EFAILEDGET;
        goto done;
    }

    /*
     * Copy CVD header byte-by-byte from response body to CVD header buffer.
     * Validate that data contains only printable characters and no NULL terminators.
     */
    memset(head, 0, sizeof(head));

    for (i = 0; i < CVD_HEADER_SIZE; i++) {
        if (!receivedData.buffer ||
            (receivedData.buffer && !*receivedData.buffer) ||
            (receivedData.buffer && !isprint(receivedData.buffer[i]))) {

            logg("%cremote_cvdhead: Malformed CVD header (bad chars)\n", logerr ? '!' : '^');
            status = FC_EFAILEDGET;
            goto done;
        }
        head[i] = receivedData.buffer[i];
    }

    /*
     * Parse CVD info into CVD info struct.
     */
    if (!(cvdhead = cl_cvdparse(head))) {
        logg("%cremote_cvdhead: Malformed CVD header (can't parse)\n", logerr ? '!' : '^');
        status = FC_EFAILEDGET;
        goto done;
    } else {
        logg("OK\n");
    }

    *cvd   = cvdhead;
    status = FC_SUCCESS;

done:

    if (NULL != receivedData.buffer) {
        free(receivedData.buffer);
    }
    if (NULL != slist) {
        curl_slist_free_all(slist);
    }
    if (NULL != curl) {
        curl_easy_cleanup(curl);
    }
    if (NULL != url) {
        free(url);
    }

    return status;
}

static fc_error_t downloadFile(
    const char *url,
    const char *destfile,
    int bAllowRedirect,
    int logerr,
    time_t ifModifiedSince)
{
    fc_error_t ret;
    fc_error_t status = FC_EARG;

    int bHttpServer = 0;

    CURL *curl = NULL;
    CURLcode curl_ret;
    char errbuf[CURL_ERROR_SIZE];
    struct curl_slist *slist = NULL;

    long http_code = 0;

    struct FileStruct receivedFile = {-1, 0};

    if ((NULL == url) || (NULL == destfile)) {
        logg("!downloadFile: Invalid arguments.\n");
        goto done;
    }

    logg("*Retrieving %s\n", url);

    if (0 == strncasecmp(url, "http", strlen("http"))) {
        bHttpServer = 1;
    }

    if (FC_SUCCESS != (ret = create_curl_handle(bHttpServer, bAllowRedirect, &curl))) {
        logg("!downloadFile: Failed to create curl handle.\n");
        status = ret;
        goto done;
    }

    if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_URL, url)) {
        logg("!downloadFile: Failed to set CURLOPT_URL for curl session (%s).\n", url);
    }
    if (0 != ifModifiedSince) {
        if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_TIMEVALUE, ifModifiedSince)) {
            logg("!downloadFile: Failed to set if-Modified-Since time value for curl session.\n");
        }
        /* If-Modified-Since the above time stamp */
        else if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE)) {
            logg("!downloadFile: Failed to set if-Modified-Since time condition for curl session.\n");
        }
    }

    if (bHttpServer) {
        /*
         * For HTTP, set some extra headers.
         */
        struct curl_slist *temp = NULL;

        if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L)) {
            logg("!downloadFile: Failed to set CURLOPT_HTTPGET for curl session.\n");
        }

#ifdef FRESHCLAM_NO_CACHE
        if (NULL == (temp = curl_slist_append(slist, "Cache-Control: no-cache"))) { // Necessary?
            logg("!downloadFile: Failed to append \"Cache-Control: no-cache\" header to custom curl header list.\n");
        } else {
            slist = temp;
        }
#endif
        if (NULL == (temp = curl_slist_append(slist, "Connection: close"))) { // Necessary?
            logg("!downloadFile: Failed to append \"Connection: close\" header to custom curl header list.\n");
        } else {
            slist = temp;
        }
        if (NULL != slist) {
            if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist)) {
                logg("!downloadFile: Failed to add custom header list to curl session.\n");
            }
        }
    }

    /* Write the response body to the destination file handle */

    if (-1 == (receivedFile.handle = open(destfile, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0644))) {
        char currdir[PATH_MAX];

        if (getcwd(currdir, sizeof(currdir)))
            logg("!downloadFile: Can't create new file %s in %s\n", destfile, currdir);
        else
            logg("!downloadFile: Can't create new file %s in the current directory\n", destfile);

        logg("Hint: The database directory must be writable for UID %d or GID %d\n", getuid(), getgid());
        status = FC_EDBDIRACCESS;
        goto done;
    }
    receivedFile.size = 0;

    /* Send all data to this function  */
    if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback)) {
        logg("!remote_cvdhead: Failed to set write-data fwrite callback function for curl session.\n");
    }

    if (CURLE_OK != curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&receivedFile)) {
        logg("!remote_cvdhead: Failed to set write-data file handle for curl session.\n");
    }

    logg("*downloadFile: Download source:      %s\n", url);
    logg("*downloadFile: Download destination: %s\n", destfile);

    /* Perform download */
    memset(errbuf, 0, sizeof(errbuf));
    curl_ret = curl_easy_perform(curl);
    if (curl_ret != CURLE_OK) {
        /*
         * Show the error information.
         * If no detailed error information was written to errbuf
         * show the more generic information from curl_easy_strerror instead.
         */
        size_t len = strlen(errbuf);
        logg("%cDownload failed (%d) ", logerr ? '!' : '^', curl_ret);
        if (len)
            logg("%c Message: %s%s", logerr ? '!' : '^', errbuf, ((errbuf[len - 1] != '\n') ? "\n" : ""));
        else
            logg("%c Message: %s\n", logerr ? '!' : '^', curl_easy_strerror(curl_ret));
        status = FC_ECONNECTION;
        goto done;
    }

    /* Check HTTP code */
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    switch (http_code) {
        case 200:
        case 206: {
            status = FC_SUCCESS;
            break;
        }
        case 304: {
            status = FC_UPTODATE;
            break;
        }
        case 404: {
            if (g_proxyServer)
                logg("^downloadFile: file not found: %s (Proxy: %s:%u)\n", url, g_proxyServer, g_proxyPort);
            else
                logg("^downloadFile: file not found: %s\n", url);
            status = FC_EFAILEDGET;
            break;
        }
        case 522: {
            logg("^downloadFile: Origin Connection Time-out. Cloudflare was unable to reach the origin web server and the request timed out. URL: %s\n", url);
            status = FC_EFAILEDGET;
            break;
        }
        default: {
            if (g_proxyServer)
                logg("%cdownloadFile: Unexpected response (%li) from %s (Proxy: %s:%u)\n",
                     logerr ? '!' : '^', http_code, url, g_proxyServer, g_proxyPort);
            else
                logg("%cdownloadFile: Unexpected response (%li) from %s\n",
                     logerr ? '!' : '^', http_code, url);
            status = FC_EFAILEDGET;
        }
    }

done:

    if (NULL != slist) {
        curl_slist_free_all(slist);
    }
    if (NULL != curl) {
        curl_easy_cleanup(curl);
    }

    if (-1 != receivedFile.handle) {
        close(receivedFile.handle);
    }

    if (FC_UPTODATE < status) {
        if (NULL != destfile) {
            unlink(destfile);
        }
    }

    return status;
}

static fc_error_t getcvd(
    const char *cvdfile,
    const char *tmpfile,
    char *server,
    unsigned int remoteVersion,
    int logerr)
{
    fc_error_t ret;
    fc_error_t status = FC_EARG;

    struct cl_cvd *cvd           = NULL;
    char *tmpfile_with_extension = NULL;
    char *url                    = NULL;
    size_t urlLen                = 0;

    if ((NULL == cvdfile) || (NULL == tmpfile) || (NULL == server)) {
        logg("!getcvd: Invalid arguments.\n");
        goto done;
    }

    urlLen = strlen(server) + strlen("/") + strlen(cvdfile);
    url    = malloc(urlLen + 1);
    snprintf(url, urlLen + 1, "%s/%s", server, cvdfile);

    if (FC_SUCCESS != (ret = downloadFile(url, tmpfile, 1, logerr, 0))) {
        logg("%cgetcvd: Can't download %s from %s\n", logerr ? '!' : '^', cvdfile, url);
        status = ret;
        goto done;
    }

    /* Temporarily rename file to correct extension for verification. */
    tmpfile_with_extension = strdup(tmpfile);
    if (!tmpfile_with_extension) {
        logg("!getcvd: Can't allocate memory for temp file with extension!\n");
        status = FC_EMEM;
        goto done;
    }
    strncpy(tmpfile_with_extension + strlen(tmpfile_with_extension) - 4, cvdfile + strlen(cvdfile) - 4, 4);
    if (rename(tmpfile, tmpfile_with_extension) == -1) {
        logg("!getcvd: Can't rename %s to %s: %s\n", tmpfile, tmpfile_with_extension, strerror(errno));
        status = FC_EDBDIRACCESS;
        goto done;
    }

    if ((ret = cl_cvdverify(tmpfile_with_extension))) {
        logg("!getcvd: Verification: %s\n", cl_strerror(ret));
        status = FC_EBADCVD;
        goto done;
    }

    if (!(cvd = cl_cvdhead(tmpfile_with_extension))) {
        logg("!getcvd: Can't read CVD header of new %s database.\n", cvdfile);
        status = FC_EBADCVD;
        goto done;
    }

    /* Rename the file back to the original, since verification passed. */
    if (rename(tmpfile_with_extension, tmpfile) == -1) {
        logg("!getcvd: Can't rename %s to %s: %s\n", tmpfile_with_extension, tmpfile, strerror(errno));
        status = FC_EDBDIRACCESS;
        goto done;
    }

    if (cvd->version < remoteVersion) {
        logg("^Mirror %s is not synchronized.\n", server);
        if (cvd->version < remoteVersion - 1) {
            logg("!Downloaded database version is more than 1 version older than the version advertised in DNS TXT record.\n");
            status = FC_EMIRRORNOTSYNC;
            goto done;
        }

        status = FC_UPTODATE;
        goto done;
    }

    status = FC_SUCCESS;

done:
    if (NULL != cvd) {
        cl_cvdfree(cvd);
    }
    if (NULL != tmpfile_with_extension) {
        unlink(tmpfile_with_extension);
        free(tmpfile_with_extension);
    }
    if (NULL != url) {
        free(url);
    }
    if (FC_SUCCESS != status) {
        if (NULL != tmpfile) {
            unlink(tmpfile);
        }
    }

    return status;
}

/**
 * @brief Change to the temp dir for storing CDIFFs for incremental database update.
 *
 * Will create the temp dir if it does not already exist.
 *
 * @param database      The database we're updating.
 * @param tmpdir        [out] The name of the temp dir to use.
 * @return fc_error_t
 */
static fc_error_t mkdir_and_chdir_for_cdiff_tmp(const char *database, const char *tmpdir)
{
    fc_error_t status = FC_EDIRECTORY;

    char cvdfile[DB_FILENAME_MAX];

    if ((NULL == database) || (NULL == tmpdir)) {
        logg("!mkdir_and_chdir_for_cdiff_tmp: Invalid arguments.\n");
        status = FC_EARG;
        goto done;
    }

    if (-1 == access(tmpdir, R_OK | W_OK)) {
        /*
         * Temp directory for incremental update (cdiff download) does not
         * yet exist.
         */
        int ret;

        /*
         * 1) Double-check that we have a CVD or CLD. Without either one, incremental update won't work.
         */
        ret = snprintf(cvdfile, sizeof(cvdfile), "%s.cvd", database);
        if (((int)sizeof(cvdfile) <= ret) || (-1 == ret)) {
            logg("!mkdir_and_chdir_for_cdiff_tmp: database parameter value too long to create cvd file name: %s\n", database);
            goto done;
        }

        if (-1 == access(cvdfile, R_OK)) {
            ret = snprintf(cvdfile, sizeof(cvdfile), "%s.cld", database);
            if (((int)sizeof(cvdfile) <= ret) || (-1 == ret)) {
                logg("!mkdir_and_chdir_for_cdiff_tmp: database parameter value too long to create cld file name: %s\n", database);
                goto done;
            }

            if (-1 == access(cvdfile, R_OK)) {
                logg("!mkdir_and_chdir_for_cdiff_tmp: Can't find (or access) local CVD or CLD for %s database\n", database);
                goto done;
            }
        }

        /*
         * 2) Create the incremental update temp directory.
         */
        if (-1 == mkdir(tmpdir, 0755)) {
            logg("!mkdir_and_chdir_for_cdiff_tmp: Can't create directory %s\n", tmpdir);
            goto done;
        }

        if (-1 == cli_cvdunpack(cvdfile, tmpdir)) {
            logg("!mkdir_and_chdir_for_cdiff_tmp: Can't unpack %s into %s\n", cvdfile, tmpdir);
            cli_rmdirs(tmpdir);
            goto done;
        }
    }

    if (-1 == chdir(tmpdir)) {
        logg("!mkdir_and_chdir_for_cdiff_tmp: Can't change directory to %s\n", tmpdir);
        goto done;
    }

    status = FC_SUCCESS;

done:

    return status;
}

static fc_error_t downloadPatch(
    const char *database,
    const char *tmpdir,
    int version,
    char *server,
    int logerr)
{
    fc_error_t ret;
    fc_error_t status = FC_EARG;

    char *tempname = NULL;
    char patch[DB_FILENAME_MAX];
    char olddir[PATH_MAX];

    char *url     = NULL;
    size_t urlLen = 0;

    int fd = -1;

    olddir[0] = '\0';

    if ((NULL == database) || (NULL == tmpdir) || (NULL == server) || (0 == version)) {
        logg("!downloadPatch: Invalid arguments.\n");
        goto done;
    }

    if (NULL == getcwd(olddir, sizeof(olddir))) {
        logg("!downloadPatch: Can't get path of current working directory\n");
        status = FC_EDIRECTORY;
        goto done;
    }

    if (FC_SUCCESS != mkdir_and_chdir_for_cdiff_tmp(database, tmpdir)) {
        status = FC_EDIRECTORY;
        goto done;
    }

    if (NULL == (tempname = cli_gentemp("."))) {
        status = FC_EMEM;
        goto done;
    }

    snprintf(patch, sizeof(patch), "%s-%d.cdiff", database, version);
    urlLen = strlen(server) + strlen("/") + strlen(patch);
    url    = malloc(urlLen + 1);
    snprintf(url, urlLen + 1, "%s/%s", server, patch);

    if (FC_SUCCESS != (ret = downloadFile(url, tempname, 1, logerr, 0))) {
        if (ret == FC_EEMPTYFILE) {
            logg("Empty script %s, need to download entire database\n", patch);
        } else {
            logg("%cgetpatch: Can't download %s from %s\n", logerr ? '!' : '^', patch, url);
        }
        status = ret;
        goto done;
    }

    if (-1 == (fd = open(tempname, O_RDONLY | O_BINARY))) {
        logg("!downloadPatch: Can't open %s for reading\n", tempname);
        status = FC_EFILE;
        goto done;
    }

    if (-1 == cdiff_apply(fd, 1)) {
        logg("!downloadPatch: Can't apply patch\n");
        status = FC_EFAILEDUPDATE;
        goto done;
    }

    status = FC_SUCCESS;

done:

    if (NULL != url) {
        free(url);
    }

    if (-1 != fd) {
        close(fd);
    }

    if (NULL != tempname) {
        unlink(tempname);
        free(tempname);
    }

    if ('\0' != olddir[0]) {
        if (-1 == chdir(olddir)) {
            logg("!downloadPatch: Can't chdir to %s\n", olddir);
            status = FC_EDIRECTORY;
        }
    }

    return status;
}

/**
 * @brief Get CVD header info for local CVD/CLD database.
 *
 * @param database          Database name
 * @param localname         [out] (optional) filename of local database.
 * @return struct cl_cvd*   CVD info struct of local database, if found. NULL if not found.
 */
static struct cl_cvd *currentdb(const char *database, char **localname)
{
    char filename[DB_FILENAME_MAX];
    struct cl_cvd *cvd = NULL;

    if (NULL == database) {
        logg("!currentdb: Invalid args!\n");
        goto done;
    }

    snprintf(filename, sizeof(filename), "%s.cvd", database);
    filename[sizeof(filename) - 1] = 0;

    if (-1 == access(filename, R_OK)) {
        /* CVD not found. */
        snprintf(filename, sizeof(filename), "%s.cld", database);
        filename[sizeof(filename) - 1] = 0;

        if (-1 == access(filename, R_OK)) {
            /* CLD also not found. Fail out. */
            goto done;
        }
    }

    if (NULL == (cvd = cl_cvdhead(filename))) {
        goto done;
    }

    if (localname) {
        *localname = cli_strdup(filename);
    }

done:

    return cvd;
}

static fc_error_t buildcld(
    const char *tmpdir,
    const char *database,
    const char *newfile,
    int bCompress)
{
    fc_error_t status = FC_EARG;

    char olddir[PATH_MAX];
    char info[DB_FILENAME_MAX];
    char buff[CVD_HEADER_SIZE + 1];
    char *pt;

    struct dirent *dent = NULL;
    DIR *dir            = NULL;
    gzFile gzs          = NULL;
    int fd              = -1;

    if ((NULL == tmpdir) || (NULL == database) || (NULL == newfile)) {
        logg("!buildcld: Invalid arguments.\n");
        goto done;
    }

    if (!getcwd(olddir, sizeof(olddir))) {
        logg("!buildcld: Can't get path of current working directory\n");
        status = FC_EDIRECTORY;
        goto done;
    }

    if (-1 == chdir(tmpdir)) {
        logg("!buildcld: Can't access directory %s\n", tmpdir);
        status = FC_EDIRECTORY;
        goto done;
    }

    snprintf(info, sizeof(info), "%s.info", database);
    info[sizeof(info) - 1] = 0;
    if (-1 == (fd = open(info, O_RDONLY | O_BINARY))) {
        logg("!buildcld: Can't open %s\n", info);
        status = FC_EFILE;
        goto done;
    }

    if (-1 == read(fd, buff, CVD_HEADER_SIZE)) {
        logg("!buildcld: Can't read %s\n", info);
        status = FC_EFILE;
        goto done;
    }
    buff[CVD_HEADER_SIZE] = 0;

    close(fd);
    fd = -1;

    if (NULL == (pt = strchr(buff, '\n'))) {
        logg("!buildcld: Bad format of %s\n", info);
        status = FC_EFAILEDUPDATE;
        goto done;
    }
    memset(pt, ' ', CVD_HEADER_SIZE + buff - pt);

    if (-1 == (fd = open(newfile, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0644))) {
        logg("!buildcld: Can't open %s for writing\n", newfile);
        status = FC_EFILE;
        goto done;
    }
    if (CVD_HEADER_SIZE != write(fd, buff, CVD_HEADER_SIZE)) {
        logg("!buildcld: Can't write to %s\n", newfile);
        status = FC_EFILE;
        goto done;
    }

    if (bCompress) {
        close(fd);
        fd = -1;
        if (NULL == (gzs = gzopen(newfile, "ab9f"))) {
            logg("!buildcld: gzopen() failed for %s\n", newfile);
            status = FC_EFAILEDUPDATE;
            goto done;
        }
    }

    if (-1 == access("COPYING", R_OK)) {
        logg("!buildcld: COPYING file not found\n");
        status = FC_EFAILEDUPDATE;
        goto done;
    }

    if (-1 == tar_addfile(fd, gzs, "COPYING")) {
        logg("!buildcld: Can't add COPYING to new %s.cld - please check if there is enough disk space available\n", database);
        status = FC_EFAILEDUPDATE;
        goto done;
    }

    if (-1 != access(info, R_OK)) {
        if (-1 == tar_addfile(fd, gzs, info)) {
            logg("!buildcld: Can't add %s to new %s.cld - please check if there is enough disk space available\n", info, database);
            status = FC_EFAILEDUPDATE;
            goto done;
        }
    }

    if (-1 != access("daily.cfg", R_OK)) {
        if (-1 == tar_addfile(fd, gzs, "daily.cfg")) {
            logg("!buildcld: Can't add daily.cfg to new %s.cld - please check if there is enough disk space available\n", database);
            status = FC_EFAILEDUPDATE;
            goto done;
        }
    }

    if (NULL == (dir = opendir("."))) {
        logg("!buildcld: Can't open directory %s\n", tmpdir);
        status = FC_EDIRECTORY;
        goto done;
    }

    while (NULL != (dent = readdir(dir))) {
        if (dent->d_ino) {
            if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, "..") || !strcmp(dent->d_name, "COPYING") || !strcmp(dent->d_name, "daily.cfg") || !strcmp(dent->d_name, info))
                continue;

            if (tar_addfile(fd, gzs, dent->d_name) == -1) {
                logg("!buildcld: Can't add %s to new %s.cld - please check if there is enough disk space available\n", dent->d_name, database);
                status = FC_EFAILEDUPDATE;
                goto done;
            }
        }
    }

    status = FC_SUCCESS;

done:

    if (-1 != fd) {
        if (-1 == close(fd)) {
            logg("!buildcld: close() failed for %s\n", newfile);
        }
    }
    if (NULL != gzs) {
        if (gzclose(gzs)) {
            logg("!buildcld: gzclose() failed for %s\n", newfile);
        }
    }
    if (NULL != dir) {
        closedir(dir);
    }

    if (FC_SUCCESS != status) {
        if (NULL != newfile) {
            unlink(newfile);
        }
    }

    if ('\0' != olddir[0]) {
        if (-1 == chdir(olddir)) {
            logg("!buildcld: Can't return to previous directory %s\n", olddir);
            status = FC_EDIRECTORY;
        }
    }

    return status;
}

static fc_error_t query_remote_database_version(
    const char *database,
    uint32_t ifModifiedSince,
    const char *dnsUpdateInfo,
    char *server,
    int bPrivateMirror,
    int logerr,
    uint32_t *remoteVersion,
    char **remoteFilename)
{
    fc_error_t ret;
    fc_error_t status = FC_EARG;

    uint32_t newVersion = 0;
    char cvdfile[DB_FILENAME_MAX];
    char cldfile[DB_FILENAME_MAX];

#ifdef HAVE_RESOLV_H
    char *dnqueryDomain = NULL;
    char *extradnsreply = NULL;
#endif

    struct cl_cvd *remote = NULL;
    int remote_is_cld     = 0;

    if ((NULL == database) || (NULL == server) || (NULL == remoteVersion) || (NULL == remoteFilename)) {
        logg("!query_remote_database_version: Invalid args!\n");
        goto done;
    }

    *remoteVersion  = 0;
    *remoteFilename = NULL;

    snprintf(cvdfile, sizeof(cvdfile), "%s.cvd", database);
    cvdfile[sizeof(cvdfile) - 1] = 0;
    snprintf(cldfile, sizeof(cldfile), "%s.cld", database);
    cldfile[sizeof(cldfile) - 1] = 0;

    if ((!bPrivateMirror) && (NULL != dnsUpdateInfo)) {
        /*
         * Use Primary DNS Update Info record to find the version.
         */
        int field              = 0;
        char *verStrDnsPrimary = NULL;

        if (0 == (field = textrecordfield(database))) {
            logg("*query_remote_database_version: Database name \"%s\" isn't listed in DNS update info.\n", database);
        } else if (NULL == (verStrDnsPrimary = cli_strtok(dnsUpdateInfo, field, ":"))) {
            logg("^Invalid DNS update info. Falling back to HTTP mode.\n");
        } else if (!cli_isnumber(verStrDnsPrimary)) {
            logg("^Broken database version in TXT record. Falling back to HTTP mode.\n");
        } else {
            newVersion = atoi(verStrDnsPrimary);
            logg("*query_remote_database_version: %s version from DNS: %d\n", cvdfile, newVersion);
        }
        free(verStrDnsPrimary);

#ifdef HAVE_RESOLV_H
        if (newVersion == 0) {
            /*
             * Primary DNS Update Info record didn't have the version # for this database.
             * Try to use a <database>.cvd.clamav.net DNS query to find the version #.
             */
            size_t dnqueryDomainLen = strlen(database) + strlen(".cvd.clamav.net");

            dnqueryDomain = malloc(dnqueryDomainLen + 1);
            snprintf(dnqueryDomain, dnqueryDomainLen + 1, "%s.cvd.clamav.net", database);
            if (NULL == (extradnsreply = dnsquery(dnqueryDomain, T_TXT, NULL))) {
                logg("^No timestamp in TXT record for %s\n", cvdfile);
            } else {
                char *recordTimeStr  = NULL;
                char *verStrDnsExtra = NULL;

                if (NULL == (recordTimeStr = cli_strtok(extradnsreply, DNS_EXTRADBINFO_RECORDTIME, ":"))) {
                    logg("^No recordtime field in TXT record for %s\n", cvdfile);
                } else {
                    int recordTime;
                    time_t currentTime;

                    recordTime = atoi(recordTimeStr);
                    free(recordTimeStr);
                    time(&currentTime);
                    if ((int)currentTime - recordTime > 10800) {
                        logg("^DNS record is older than 3 hours.\n");
                    } else if (NULL != (verStrDnsExtra = cli_strtok(extradnsreply, 0, ":"))) {
                        if (!cli_isnumber(verStrDnsExtra)) {
                            logg("^Broken database version in TXT record for %s\n", cvdfile);
                        } else {
                            newVersion = atoi(verStrDnsExtra);
                            logg("*%s version from DNS: %d\n", cvdfile, newVersion);
                        }
                        free(verStrDnsExtra);
                    } else {
                        logg("^Invalid DNS reply. Falling back to HTTP mode.\n");
                    }
                }
            }
        }
#endif
    }

    if (newVersion == 0) {
        /*
         * Was unable to use DNS info records to determine database version.
         * Use HTTP GET to get version info from CVD/CLD header.
         */
        if (bPrivateMirror) {
            /*
             * For a private mirror, get the CLD instead of the CVD.
             *
             * On the mirror, they should have CDIFFs/scripted/incremental
             * updates enabled, so they should have CLD's to distribute.
             */
            ret = remote_cvdhead(cldfile, ifModifiedSince, server, logerr, &remote);
            if ((FC_SUCCESS == ret) || (FC_UPTODATE == ret)) {
                remote_is_cld = 1;
            } else {
                /*
                 * Failed to get CLD update, and it's unknown if the status is up-to-date.
                 *
                 * If it's a relatively new mirror, the CLD won't have been replaced with a CVD yet.
                 * Attempt to get the CVD instead.
                 */
                ret = remote_cvdhead(cvdfile, ifModifiedSince, server, logerr, &remote);
            }
        } else {
            /*
             * Official update servers will only have the CVD.
             */
            ret = remote_cvdhead(cvdfile, ifModifiedSince, server, logerr, &remote);
        }

        switch (ret) {
            case FC_SUCCESS: {
                logg("*%s database version obtained using HTTP GET: %u\n", database, remote->version);
                break;
            }
            case FC_UPTODATE: {
                logg("*%s database version up-to-date, according to HTTP response code from server.\n", database);
                status = FC_UPTODATE;
                goto done;
            }
            default: {
                logg("^Failed to get %s database version information from server: %s\n", database, server);
                status = ret;
                goto done;
            }
        }

        newVersion = remote->version;
    }

    if (remote_is_cld) {
        *remoteFilename = cli_strdup(cldfile);
    } else {
        *remoteFilename = cli_strdup(cvdfile);
    }
    *remoteVersion = newVersion;

    status = FC_SUCCESS;

done:

    if (NULL != remote) {
        cl_cvdfree(remote);
    }
#ifdef HAVE_RESOLV_H
    if (NULL != dnqueryDomain) {
        free(dnqueryDomain);
    }
    if (NULL != extradnsreply) {
        free(extradnsreply);
    }
#endif

    return status;
}

static fc_error_t check_for_new_database_version(
    const char *database,
    const char *dnsUpdateInfo,
    char *server,
    int bPrivateMirror,
    int logerr,
    uint32_t *localVersion,
    uint32_t *remoteVersion,
    char **localFilename,
    char **remoteFilename)
{
    fc_error_t ret;
    fc_error_t status = FC_EARG;

    char *localname               = NULL;
    struct cl_cvd *local_database = NULL;
    char *remotename              = NULL;

    uint32_t localver       = 0;
    uint32_t localTimestamp = 0;
    uint32_t remotever      = 0;

    if ((NULL == database) || (NULL == server) ||
        (NULL == localVersion) || (NULL == remoteVersion) ||
        (NULL == localFilename) || (NULL == remoteFilename)) {
        logg("!check_for_new_database_version: Invalid args!\n");
        goto done;
    }

    *localVersion   = 0;
    *remoteVersion  = 0;
    *localFilename  = NULL;
    *remoteFilename = NULL;

    /*
     * Check local database version (if exists)
     */
    if (NULL == (local_database = currentdb(database, &localname))) {
        logg("*check_for_new_database_version: No local copy of \"%s\" database.\n", database);
    } else {
        logg("*check_for_new_database_version: Local copy of %s found: %s.\n", database, localname);
        localTimestamp = local_database->stime;
        localver       = local_database->version;
    }

    /*
     * Look up the latest available database version.
     */
    ret = query_remote_database_version(
        database,
        localTimestamp,
        dnsUpdateInfo,
        server,
        bPrivateMirror,
        logerr,
        &remotever,
        &remotename);
    switch (ret) {
        case FC_SUCCESS: {
            if (0 == localver) {
                logg("%s database available for download (remote version: %d)\n",
                     database, remotever);
                break;
            } else if (localver < remotever) {
                logg("%s database available for update (local version: %d, remote version: %d)\n",
                     database, localver, remotever);
                break;
            }
            // else: Fall-through to Up-to-date case.
        }
        case FC_UPTODATE: {
            if (NULL == local_database) {
                logg("!check_for_new_database_version: server claims we're up to date, but we don't have a local database!\n");
                status = FC_EFAILEDGET;
                goto done;
            }
            logg("%s database is up to date (version: %d, sigs: %d, f-level: %d, builder: %s)\n",
                 localname,
                 local_database->version,
                 local_database->sigs,
                 local_database->fl,
                 local_database->builder);

            /* The remote version wouldn't be set if the server returned "Not-Modified".
               We know it will be the same as the local version though. */
            remotever = localver;
            break;
        }
        default: {
            logg("!check_for_new_database_version: Failed to find %s database using server %s.\n", database, server);
            status = FC_EFAILEDGET;
            goto done;
        }
    }

    *remoteVersion = remotever;
    if (NULL != remotename) {
        *remoteFilename = cli_strdup(remotename);
        if (NULL == *remoteFilename) {
            logg("!check_for_new_database_version: Failed to allocate memory for remote filename.\n");
            status = FC_EMEM;
            goto done;
        }
    }
    if (NULL != localname) {
        *localVersion  = localver;
        *localFilename = cli_strdup(localname);
        if (NULL == *localFilename) {
            logg("!check_for_new_database_version: Failed to allocate memory for local filename.\n");
            status = FC_EMEM;
            goto done;
        }
    }

    status = FC_SUCCESS;

done:

    if (NULL != localname) {
        free(localname);
    }
    if (NULL != remotename) {
        free(remotename);
    }
    if (NULL != local_database) {
        cl_cvdfree(local_database);
    }

    return status;
}

fc_error_t updatedb(
    const char *database,
    const char *dnsUpdateInfo,
    char *server,
    int bPrivateMirror,
    void *context,
    int bScriptedUpdates,
    int logerr,
    int *signo,
    char **dbFilename,
    int *bUpdated)
{
    fc_error_t ret;
    fc_error_t status = FC_EARG;

    struct cl_cvd *cvd = NULL;

    uint32_t localVersion  = 0;
    uint32_t remoteVersion = 0;
    char *localFilename    = NULL;
    char *remoteFilename   = NULL;
    char *newLocalFilename = NULL;

    char *tmpdir  = NULL;
    char *tmpfile = NULL;

    unsigned int flevel;

    unsigned int i, j;

    if ((NULL == database) || (NULL == server) || (NULL == signo) || (NULL == dbFilename) || (NULL == bUpdated)) {
        logg("!updatedb: Invalid args!\n");
        goto done;
    }

    *signo      = 0;
    *dbFilename = NULL;
    *bUpdated   = 0;

    /*
     * Check if new version exists.
     */
    if (FC_SUCCESS != (ret = check_for_new_database_version(
                           database,
                           dnsUpdateInfo,
                           server,
                           bPrivateMirror,
                           logerr,
                           &localVersion,
                           &remoteVersion,
                           &localFilename,
                           &remoteFilename))) {
        logg("*updatedb: %s database update failed.\n", database);
        status = ret;
        goto done;
    }

    if ((localVersion >= remoteVersion) && (NULL != localFilename)) {
        *dbFilename = cli_strdup(localFilename);
        goto up_to_date;
    }

    /* Download CVD or CLD to temp file */
    tmpfile = cli_gentemp(g_tempDirectory);
    if (!tmpfile) {
        status = FC_EMEM;
        goto done;
    }

    if ((localVersion == 0) || (!bScriptedUpdates)) {
        /*
         * Download entire file.
         */
        ret = getcvd(remoteFilename, tmpfile, server, remoteVersion, logerr);
        if (FC_SUCCESS != ret) {
            status = ret;
            goto done;
        }

        newLocalFilename = cli_strdup(remoteFilename);
    } else {
        /*
         * Attempt scripted/CDIFF incremental update.
         */
        ret = FC_SUCCESS;

        tmpdir = cli_gentemp(g_tempDirectory);
        if (!tmpdir) {
            status = FC_EMEM;
            goto done;
        }

        for (i = localVersion + 1; i <= remoteVersion; i++) {
            for (j = 1; j <= g_maxAttempts; j++) {
                int llogerr = logerr;
                if (logerr)
                    llogerr = (j == g_maxAttempts);
                ret = downloadPatch(database, tmpdir, i, server, llogerr);
                if (ret == FC_ECONNECTION || ret == FC_EFAILEDGET) {
                    continue;
                } else {
                    break;
                }
            }
            if (FC_SUCCESS != ret)
                break;
        }

        if (FC_SUCCESS != ret) {
            /*
             * Incremental update failed or intentionally disabled.
             */
            if (ret == FC_EEMPTYFILE) {
                logg("*Empty CDIFF found. Skip incremental updates for this version and download %s\n", remoteFilename);
            } else {
                logg("^Incremental update failed, trying to download %s\n", remoteFilename);
            }

            ret = getcvd(remoteFilename, tmpfile, server, remoteVersion, logerr);
            if (FC_SUCCESS != ret) {
                status = ret;
                goto done;
            }

            newLocalFilename = cli_strdup(remoteFilename);
        } else {
            /*
             * CDIFFs downloaded; Use CDIFFs to turn old CVD/CLD into new updated CLD.
             */
            size_t newLocalFilenameLen = 0;
            if (FC_SUCCESS != buildcld(tmpdir, database, tmpfile, g_bCompressLocalDatabase)) {
                logg("!updatedb: Incremental update failed. Failed to build CLD.\n");
                status = FC_EFAILEDUPDATE;
                goto done;
            }

            newLocalFilenameLen = strlen(database) + strlen(".cld");
            newLocalFilename    = malloc(newLocalFilenameLen + 1);
            snprintf(newLocalFilename, newLocalFilenameLen + 1, "%s.cld", database);
        }
    }

    /*
     * Update downloaded.
     * Test database before replacing original database with new database.
     */
    if (NULL != g_cb_download_complete) {
        char *tmpfile_with_extension      = NULL;
        size_t tmpfile_with_extension_len = strlen(tmpfile) + 1 + strlen(newLocalFilename);

        /* Suffix tmpfile with real database name & extension so it can be loaded. */
        tmpfile_with_extension = malloc(tmpfile_with_extension_len + 1);
        if (!tmpfile_with_extension) {
            status = FC_ETESTFAIL;
            goto done;
        }
        snprintf(tmpfile_with_extension, tmpfile_with_extension_len + 1, "%s-%s", tmpfile, newLocalFilename);
        if (rename(tmpfile, tmpfile_with_extension) == -1) {
            free(tmpfile_with_extension);

            logg("!updatedb: Can't rename %s to %s: %s\n", tmpfile, tmpfile_with_extension, strerror(errno));
            status = FC_EDBDIRACCESS;
            goto done;
        }
        free(tmpfile);
        tmpfile                = tmpfile_with_extension;
        tmpfile_with_extension = NULL;

        /* Run callback to test it. */
        logg("*updatedb: Running g_cb_download_complete callback...\n");
        if (FC_SUCCESS != (ret = g_cb_download_complete(tmpfile, context))) {
            logg("*updatedb: callback failed: %s (%d)\n", fc_strerror(ret), ret);
            status = ret;
            goto done;
        }
    }

    /*
     * Replace original database with new database.
     */
#ifdef _WIN32
    if (!access(newLocalFilename, R_OK) && unlink(newLocalFilename)) {
        logg("!updatedb: Can't delete old database %s. Please fix the problem manually and try again.\n", newLocalFilename);
        status = FC_EEMPTYFILE;
        goto done;
    }
#endif
    if (rename(tmpfile, newLocalFilename) == -1) {
        logg("!updatedb: Can't rename %s to %s: %s\n", tmpfile, newLocalFilename, strerror(errno));
        status = FC_EDBDIRACCESS;
        goto done;
    }

    /* If we just updated from a CVD to a CLD, delete the old CVD */
    if ((NULL != localFilename) && !access(localFilename, R_OK) && strcmp(newLocalFilename, localFilename))
        if (unlink(localFilename))
            logg("^updatedb: Can't delete the old database file %s. Please remove it manually.\n", localFilename);

    /* Parse header to record number of sigs. */
    if (NULL == (cvd = cl_cvdhead(newLocalFilename))) {
        logg("!updatedb: Can't parse new database %s\n", newLocalFilename);
        status = FC_EFILE;
        goto done;
    }

    logg("%s updated (version: %d, sigs: %d, f-level: %d, builder: %s)\n",
         newLocalFilename, cvd->version, cvd->sigs, cvd->fl, cvd->builder);

    flevel = cl_retflevel();
    if (flevel < cvd->fl) {
        logg("^Your ClamAV installation is OUTDATED!\n");
        logg("^Current functionality level = %d, recommended = %d\n", flevel, cvd->fl);
        logg("DON'T PANIC! Read https://www.clamav.net/documents/installing-clamav\n");
    }

    *signo      = cvd->sigs;
    *bUpdated   = 1;
    *dbFilename = cli_strdup(newLocalFilename);
    if (NULL == *dbFilename) {
        logg("!updatedb: Failed to allocate memory for database filename.\n");
        status = FC_EMEM;
        goto done;
    }

up_to_date:

    status = FC_SUCCESS;

done:

    if (NULL != cvd) {
        cl_cvdfree(cvd);
    }

    if (NULL != localFilename) {
        free(localFilename);
    }
    if (NULL != remoteFilename) {
        free(remoteFilename);
    }
    if (NULL != newLocalFilename) {
        free(newLocalFilename);
    }

    if (NULL != tmpfile) {
        unlink(tmpfile);
        free(tmpfile);
    }
    if (NULL != tmpdir) {
        cli_rmdirs(tmpdir);
        free(tmpdir);
    }

    return status;
}

fc_error_t updatecustomdb(
    const char *url,
    void *context,
    int logerr,
    int *signo,
    char **dbFilename,
    int *bUpdated)
{
    fc_error_t ret;
    fc_error_t status = FC_EARG;

    unsigned int sigs = 0;
    char *tmpfile     = NULL;
    const char *databaseName;
    STATBUF statbuf;
    time_t dbtime = 0;

    if ((NULL == url) || (NULL == signo) || (NULL == dbFilename) || (NULL == bUpdated)) {
        logg("!updatecustomdb: Invalid args!\n");
        goto done;
    }

    *signo      = 0;
    *dbFilename = NULL;
    *bUpdated   = 0;

    tmpfile = cli_gentemp(g_tempDirectory);
    if (!tmpfile) {
        status = FC_EFAILEDUPDATE;
        goto done;
    }

    if (!strncasecmp(url, "file://", strlen("file://"))) {
        /*
         * Copy from local file.
         */
        time_t remote_dbtime;
        const char *rpath;

        rpath = &url[strlen("file://")];
#ifdef _WIN32
        databaseName = strrchr(rpath, '\\');
#else
        databaseName = strrchr(rpath, '/');
#endif
        if ((NULL == databaseName) || strlen(databaseName++) < strlen(".ext") + 1) {
            logg("DatabaseCustomURL: Incorrect URL\n");
            status = FC_EFAILEDUPDATE;
            goto done;
        }

        if (CLAMSTAT(rpath, &statbuf) == -1) {
            logg("DatabaseCustomURL: file %s missing\n", rpath);
            status = FC_EFAILEDUPDATE;
            goto done;
        }
        remote_dbtime = statbuf.st_mtime;
        dbtime        = (CLAMSTAT(databaseName, &statbuf) != -1) ? statbuf.st_mtime : 0;
        if (dbtime > remote_dbtime) {
            logg("%s is up to date (version: custom database)\n", databaseName);
            goto up_to_date;
        }

        /* FIXME: preserve file permissions, calculate % */
        if (-1 == cli_filecopy(rpath, tmpfile)) {
            logg("DatabaseCustomURL: Can't copy file %s into database directory\n", rpath);
            status = FC_EFAILEDUPDATE;
            goto done;
        }

        logg("Downloading %s [100%%]\n", databaseName);
    } else {
        /*
         * Download from URL.  http(s) or ftp(s)
         */
        databaseName = strrchr(url, '/');
        if ((NULL == databaseName) || (strlen(databaseName++) < 5)) {
            logg("DatabaseCustomURL: Incorrect URL\n");
            status = FC_EFAILEDUPDATE;
            goto done;
        }

        dbtime = (CLAMSTAT(databaseName, &statbuf) != -1) ? statbuf.st_mtime : 0;

        ret = downloadFile(url, tmpfile, 1, logerr, dbtime);
        if (ret == FC_UPTODATE) {
            logg("%s is up to date (version: custom database)\n", databaseName);
            goto up_to_date;
        } else if (ret > FC_UPTODATE) {
            logg("%cCan't download %s from %s\n", logerr ? '!' : '^', databaseName, url);
            status = ret;
            goto done;
        }
    }

    /*
     * Update downloaded.
     * Test database before replacing original database with new database.
     */
    if (NULL != g_cb_download_complete) {
        char *tmpfile_with_extension      = NULL;
        size_t tmpfile_with_extension_len = strlen(tmpfile) + 1 + strlen(databaseName);

        /* Suffix tmpfile with real database name & extension so it can be loaded. */
        tmpfile_with_extension = malloc(tmpfile_with_extension_len + 1);
        if (!tmpfile_with_extension) {
            status = FC_ETESTFAIL;
            goto done;
        }
        snprintf(tmpfile_with_extension, tmpfile_with_extension_len + 1, "%s-%s", tmpfile, databaseName);
        if (rename(tmpfile, tmpfile_with_extension) == -1) {
            free(tmpfile_with_extension);

            logg("!updatecustomdb: Can't rename %s to %s: %s\n", tmpfile, tmpfile_with_extension, strerror(errno));
            status = FC_EDBDIRACCESS;
            goto done;
        }
        free(tmpfile);
        tmpfile                = tmpfile_with_extension;
        tmpfile_with_extension = NULL;

        /* Run callback to test it. */
        logg("*updatecustomdb: Running g_cb_download_complete callback...\n");
        if (FC_SUCCESS != (ret = g_cb_download_complete(tmpfile, context))) {
            logg("*updatecustomdb: callback failed: %s (%d)\n", fc_strerror(ret), ret);
            status = ret;
            goto done;
        }
    }

    /*
     * Replace original database with new database.
     */
#ifdef _WIN32
    if (!access(databaseName, R_OK) && unlink(databaseName)) {
        logg("!updatecustomdb: Can't delete old database %s. Please fix the problem manually and try again.\n", databaseName);
        status = FC_EEMPTYFILE;
        goto done;
    }
#endif
    if (rename(tmpfile, databaseName) == -1) {
        logg("!updatecustomdb: Can't rename %s to %s: %s\n", tmpfile, databaseName, strerror(errno));
        status = FC_EDBDIRACCESS;
        goto done;
    }

    /*
     * Record # of signatures in updated database.
     */
    if (cli_strbcasestr(databaseName, ".cld") || cli_strbcasestr(databaseName, ".cvd")) {
        struct cl_cvd *cvd = NULL;
        unsigned int flevel;

        if (NULL == (cvd = cl_cvdhead(databaseName))) {
            logg("!updatecustomdb: Can't parse new database %s\n", databaseName);
            status = FC_EFILE;
            goto done;
        }

        sigs = cvd->sigs;

        flevel = cl_retflevel();
        if (flevel < cvd->fl) {
            logg("^Your ClamAV installation is OUTDATED!\n");
            logg("^Current functionality level = %d, recommended = %d\n", flevel, cvd->fl);
            logg("DON'T PANIC! Read https://www.clamav.net/documents/installing-clamav\n");
        }

        cl_cvdfree(cvd);
    } else if (cli_strbcasestr(databaseName, ".cbc")) {
        sigs = 1;
    } else {
        sigs = countlines(databaseName);
    }

    logg("%s updated (version: custom database, sigs: %u)\n", databaseName, sigs);
    *signo    = sigs;
    *bUpdated = 1;

up_to_date:

    *dbFilename = cli_strdup(databaseName);
    if (NULL == *dbFilename) {
        logg("!Failed to allocate memory for database filename.\n");
        status = FC_EMEM;
        goto done;
    }

    status = FC_SUCCESS;

done:

    if (NULL != tmpfile) {
        unlink(tmpfile);
        free(tmpfile);
    }

    return status;
}
