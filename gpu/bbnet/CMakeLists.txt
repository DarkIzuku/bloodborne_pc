# Network-only native bridge adapted from Supermedo/bloodborne_pc e076119.
# The renderer and the existing shared runtime shims stay authoritative.
find_package(Protobuf CONFIG REQUIRED GLOBAL)
find_package(nlohmann_json CONFIG REQUIRED GLOBAL)
find_package(OpenSSL REQUIRED GLOBAL)
find_package(ZLIB REQUIRED GLOBAL)
set(BBNET_VENDOR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/shadps4)
set(BBNET_PROTO_OUT ${CMAKE_CURRENT_BINARY_DIR}/proto)
file(MAKE_DIRECTORY ${BBNET_PROTO_OUT})
add_custom_command(
    OUTPUT ${BBNET_PROTO_OUT}/shadnet.pb.cc ${BBNET_PROTO_OUT}/shadnet.pb.h
    COMMAND protobuf::protoc --proto_path=${BBNET_VENDOR}/shadnet
            --cpp_out=${BBNET_PROTO_OUT} ${BBNET_VENDOR}/shadnet/shadnet.proto
    DEPENDS ${BBNET_VENDOR}/shadnet/shadnet.proto)
file(GLOB_RECURSE BBNET_SOURCES CONFIGURE_DEPENDS
    vendor/shadps4/core/libraries/network/*.cpp vendor/shadps4/core/libraries/np/*.cpp
    vendor/shadps4/shadnet/*.cpp)
add_library(bbnet STATIC ${BBNET_SOURCES} ${BBNET_PROTO_OUT}/shadnet.pb.cc bbnet_glue.cpp)
target_include_directories(bbnet PRIVATE include ../shim ${BBNET_VENDOR}
    ${BBNET_VENDOR}/shadnet ../shadps4 ${BBNET_PROTO_OUT} ../third_party/imgui third_party/httplib)
target_compile_definitions(bbnet PRIVATE BOOST_ASIO_STANDALONE NDEBUG
    CPPHTTPLIB_OPENSSL_SUPPORT CPPHTTPLIB_ZLIB_SUPPORT)
target_compile_options(bbnet PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
target_link_libraries(bbnet PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    protobuf::libprotobuf nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
if(WIN32)
    target_sources(bbnet PRIVATE third_party/wepoll/wepoll.c)
    target_include_directories(bbnet PRIVATE third_party/wepoll)
    find_path(BBNET_MINIUPNPC_INCLUDE miniupnpc.h PATH_SUFFIXES miniupnpc REQUIRED)
    find_library(BBNET_MINIUPNPC_LIBRARY NAMES libminiupnpc.a miniupnpc REQUIRED)
    target_include_directories(bbnet PRIVATE ${BBNET_MINIUPNPC_INCLUDE})
    target_compile_definitions(bbnet PRIVATE MINIUPNP_STATICLIB NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(bbnet PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-include ${CMAKE_CURRENT_SOURCE_DIR}/include/bb_win_compat.h>)
    target_link_libraries(bbnet PUBLIC ${BBNET_MINIUPNPC_LIBRARY} ws2_32 iphlpapi bcrypt crypt32)
endif()
