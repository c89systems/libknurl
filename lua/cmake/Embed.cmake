file(READ "${INPUT}" bytes HEX)
string(REGEX REPLACE "(..)" "0x\\1," bytes "${bytes}")
file(WRITE "${OUTPUT}" "static const unsigned char rotmos_facade[] = {${bytes}};\n")
