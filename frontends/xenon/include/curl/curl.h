/* Minimal libcurl header for builds with NETSURF_USE_CURL=NO.
 * NetSurf's content/fetch.c includes the curl fetcher header everywhere,
 * but only compiles the curl code when WITH_CURL is defined. */
#ifndef XENON_STUB_CURL_H
#define XENON_STUB_CURL_H
typedef void CURL;
typedef void CURLM;
typedef void CURLcode;
#endif
