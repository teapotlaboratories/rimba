idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)

# DELIBERATELY NO TEST_AP_OPEN BLOCK -- do not add one.
#
# app_main.c carries an `#ifdef TEST_AP_OPEN` branch that runs the SoftAP with no security. It is there
# only because this file's app_main.c must stay byte-identical to test-mesh-gate-ap's below the banner
# (the T0 clone-mirror guard, manifest.py CLONE_MIRRORS) -- the invariant that stops the shipped gate and
# its fixture from drifting apart, which is exactly how a malformed proxy-ARP reply once lived in three
# copies at once.
#
# The branch is UNREACHABLE here, and this absence is the only thing making it so: the macro is never
# defined for this target, so the compiler takes the SAE/PMF `#else` unconditionally. Adding a
# TEST_AP_OPEN block here would ship a gate whose link encryption can be switched off from the command
# line. The capture build belongs to the fixture; this app has no capture mode.
