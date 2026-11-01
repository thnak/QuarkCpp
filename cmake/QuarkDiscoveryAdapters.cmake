# Optional 021 LAN discovery platform (ADR-070, issue #42): the Windows DNS-SD implementation over
# dnsapi (DnsServiceRegister/Browse/Resolve, Windows 10 1809+). OFF by default: the std-only core and
# the in-memory fake (include/quark/net/lan_discovery_fake.hpp) need nothing here. The adapter, the
# encoding and the fake are header-only and always available; this option only adds the real Windows
# platform and its smoke check. Linux (Avahi) is OUT OF SCOPE for ADR-070: there the application
# supplies its own DnsSdPlatform.
option(QUARK_WITH_DNSSD "Build the Windows DNS-SD platform (dnsapi) and its real-network smoke check (ADR-070)" OFF)

if(QUARK_WITH_DNSSD)
  if(NOT WIN32)
    message(FATAL_ERROR "QUARK_WITH_DNSSD is Windows-only (dnsapi). Linux/Avahi is out of scope for ADR-070; "
                        "supply your own quark::net::DnsSdPlatform.")
  endif()
  add_library(quark_discovery_dnssd INTERFACE)
  target_link_libraries(quark_discovery_dnssd INTERFACE quark::quark dnsapi ws2_32)
  add_library(quark::discovery_dnssd ALIAS quark_discovery_dnssd)
  message(STATUS "Quark: Windows DNS-SD platform ENABLED")

  if(QUARK_BUILD_TESTS)
    add_executable(dnssd_windows_smoke_check tests/adapters/dnssd_windows_smoke_check.cpp)
    target_link_libraries(dnssd_windows_smoke_check PRIVATE quark::discovery_dnssd quark_warnings)
    add_test(NAME dnssd_windows_smoke_check COMMAND dnssd_windows_smoke_check)
    # Exit code 77 = skipped with an explicit reason printed on stdout (mDNS blocked/unavailable here).
    set_tests_properties(dnssd_windows_smoke_check PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()
