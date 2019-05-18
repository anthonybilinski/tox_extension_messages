project(tox_extension_messages)

find_package(PkgConfig REQUIRED)

pkg_search_module(TOXCORE REQUIRED toxcore)

add_library(tox_extension_messages tox_extension_messages.c)
target_compile_options(tox_extension_messages PRIVATE -Wall -Wextra -pedantic -Werror -std=c11)
target_link_libraries(tox_extension_messages ${TOXCORE_LIBRARIES})
set_target_properties(tox_extension_messages PROPERTIES PUBLIC_HEADER "tox_extension_messages.h")
target_include_directories(tox_extension_messages PRIVATE "${TOXCORE_INCLUDEDIR}")

install(TARGETS tox_extension_messages ARCHIVE DESTINATION lib PUBLIC_HEADER DESTINATION include)

enable_testing()
add_subdirectory(test)
