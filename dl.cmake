# cmake -DOUT=<file> -P dl.cmake: one HTTPS download; prints RESULT <curl code> and, on failure,
# curl's connect lines.
file(DOWNLOAD https://github.com/ "${OUT}" STATUS st LOG lg TLS_CAINFO /zip/usr/share/ssl/cert.pem)
list(GET st 0 code)
message("RESULT ${code}")
if(code)
  string(REGEX MATCHALL "[^\n]*[Cc]onnect[^\n]*" lines "${lg}")
  foreach(l IN LISTS lines)
    message("  ${l}")
  endforeach()
endif()
